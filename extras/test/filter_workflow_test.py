"""Installed actual CLI pipelines with disposable transport/index fixtures."""
from datetime import datetime, timezone
from pathlib import Path
import json
import os
import shutil
import select
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class WorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = None
        retained = os.environ.get('JARO_FILTER_WORKFLOW_WORK')
        if not retained and os.environ.get('JARO_FILTER_TEST_TMP'):
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-workflows-',dir=os.environ['JARO_FILTER_TEST_TMP'])
        cls.work = Path(retained or (cls.temp.name if cls.temp else tempfile.mkdtemp(prefix='jaro-workflows-')))
        cls.work.mkdir(mode=0o700, parents=True, exist_ok=True)
        cls.source, cls.prefix = cls.work / 'source with spaces', cls.work / 'prefix with spaces'
        cls.source.mkdir()
        for name in ('src', 'build', 'extras', 'doc', 'Makefile', '.github', 'README.md', 'vendor'):
            p = REPO / name
            if p.is_dir():
                def ignore(directory, names):
                    return [n for n in names if n.endswith(('.o', '.d')) or n == '__pycache__' or
                            (Path(directory) == REPO / 'build' and n in ('gnu', 'win', 'sieve-tools'))]
                shutil.copytree(p, cls.source / name, ignore=ignore)
            else:
                shutil.copy2(p, cls.source / name)
        cls.events = []
        command = ['make', '-j2', 'install', 'PREFIX='+str(cls.prefix)]
        build_env = dict(os.environ)
        if 'JARO_FILTER_COMPILER_CFLAGS' in build_env:
            # Instrument installed helpers without imposing a new warning gate
            # on retained legacy dotlock code. Focused native suites retain
            # -Werror; the same native sources are tested there independently.
            build_env['CFLAGS'] = build_env['JARO_FILTER_COMPILER_CFLAGS'].replace('-Werror', '')
        cls.build_env = build_env
        with (cls.work / 'install.log').open('wb') as log:
            result = subprocess.run(command, cwd=cls.source, env=build_env, stdout=log, stderr=subprocess.STDOUT)
        cls.events.append({'command': command, 'cwd': str(cls.source), 'exit': result.returncode,
                           'CFLAGS': build_env.get('CFLAGS'),
                           'observedAt': datetime.now(timezone.utc).isoformat()})
        assert result.returncode == 0, str(cls.work / 'install.log')
        cls.runtime = cls.prefix / 'share/jaromail'
        cls.cli = cls.prefix / 'bin/jaro'
        # Only transport is substituted; main, bootstrap, command chain and compiler are real.
        (cls.runtime / 'zlibs/zz-workflow-fixture').write_text('''fetch() {
  print -r -- fetch >> "$JARO_WORKFLOW_TRANSPORT_LOG"
  print -r -- "$account" >> "$JARO_WORKFLOW_ACCOUNT_LOG"
  return ${JARO_WORKFLOW_TRANSPORT_STATUS:-0}
}
''')
        cls.bins = cls.work / 'instrumented bin'
        cls.bins.mkdir()
        for name in ('cat', 'awk', 'abook', 'find', 'date', 'maddr', 'mpick', 'mseq', 'sort', 'uniq'):
            real = shutil.which(name)
            script = cls.bins / name
            script.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                              'with open(os.environ["JARO_WORKFLOW_PROCESS_LOG"],"ab") as f: f.write(%r)\n'
                              'os.execv(%r,[%r]+sys.argv[1:])\n' % ((name+'\n').encode(), real, real))
            script.chmod(0o700)
        (cls.bins / 'notmuch').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$JARO_WORKFLOW_INDEX_LOG"\nexit "${JARO_WORKFLOW_INDEX_STATUS:-0}"\n')
        (cls.bins / 'notmuch').chmod(0o700)
        cls.instrument_helpers()

    @classmethod
    def instrument_helpers(cls):
        for name in ('jaro-filter-compile', 'jaro-filter'):
            helper = cls.runtime / 'bin' / name
            real = helper.with_name(name+'.real')
            if helper.read_bytes()[:2] == b'#!':
                continue
            helper.replace(real)
            helper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                              'with open(os.environ["JARO_WORKFLOW_PROCESS_LOG"],"ab") as f: f.write(%r)\n'
                              'os.execv(%r,[%r]+sys.argv[1:])\n' % ((name+'\n').encode(), str(real), str(real)))
            helper.chmod(0o700)

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2)+'\n')

    def setUp(self):
        self.instrument_helpers()
        self.root = self.work / self._testMethodName / 'mail root'
        self.root.mkdir(parents=True)
        self.accounts = self.root.parent / 'external accounts'
        self.accounts.mkdir()
        (self.accounts / 'default.txt').write_text('name Fixture\nemail own@example.org\nlogin fixture\nproto pop\nhost fixture.invalid\nport 995\n')
        (self.root / 'Filters.txt').write_text('from-address route@example.org move First\n')
        (self.root / 'Aliases.txt').write_text('alias@example.org\n')
        for name in ('incoming', 'unsorted'):
            for sub in ('new', 'cur', 'tmp'):
                (self.root / name / sub).mkdir(parents=True)
        self.message = self.root / 'incoming/new/disposable'
        self.message.write_text('From: route@example.org\nTo: own@example.org\n\nBody retained.\n')
        self.processlog, self.indexlog, self.transportlog = [self.root.parent / s for s in ('processes', 'index', 'transport')]
        self.env = dict(os.environ, JAROMAILDIR=str(self.root), JAROACCOUNT=str(self.accounts),
                        JAROWORKDIR=str(self.runtime), PATH=str(self.bins)+os.pathsep+os.environ['PATH'],
                        JARO_WORKFLOW_PROCESS_LOG=str(self.processlog), JARO_WORKFLOW_INDEX_LOG=str(self.indexlog),
                        JARO_WORKFLOW_ACCOUNT_LOG=str(self.root.parent / 'selected-account'),
                        JARO_WORKFLOW_TRANSPORT_LOG=str(self.transportlog))
        self.env.pop('JARO_FILTER_ENGINE', None)

    def run_cli(self, *args, code=0, engine='native'):
        env = dict(self.env)
        if engine is not None:
            env['JARO_FILTER_ENGINE'] = engine
        result = subprocess.run([str(self.cli), *args], cwd=self.root.parent, env=env, capture_output=True, timeout=40)
        self.events.append({'command': [str(self.cli), *args], 'cwd': str(self.root.parent), 'exit': result.returncode,
                            'engine': engine, 'root': str(self.root), 'accounts': str(self.accounts),
                            'stdout_hex': result.stdout.hex(), 'stderr_hex': result.stderr.hex(),
                            'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(result.returncode, code, result.stderr+result.stdout)
        return result

    def snapshot(self):
        return os.readlink(self.root / 'cache/filter-engine/current'), (self.root / 'Filters.sieve').read_bytes()

    def test_native_sieve_update_repeats_and_index_boundary(self):
        self.run_cli('sieve')
        first = self.snapshot()
        self.assertIn(b'"First"', first[1])
        self.assertIn(b'own@example.org', first[1])
        self.assertFalse(self.indexlog.exists())
        self.assertFalse((self.root / 'cache/filters').exists())
        self.run_cli('sieve')
        self.assertEqual(self.snapshot(), first)
        (self.root / '.notmuch/xapian').mkdir(parents=True)
        self.run_cli('update')
        self.assertIn('new', self.indexlog.read_text())
        self.assertEqual(self.snapshot(), first)

    def test_engine_selection_legacy_guard_and_missing_helper(self):
        for engine in ('legacy', None):
            self.run_cli('sieve', engine=engine, code=4)
            self.assertFalse((self.root / 'Filters.sieve').exists())
        for engine in ('', 'unknown'):
            self.run_cli('sieve', engine=engine, code=2)
        helper = self.runtime / 'bin/jaro-filter-compile'
        hidden = helper.with_name('compiler-away')
        helper.rename(hidden)
        try:
            self.run_cli('sieve', code=127)
            self.assertFalse((self.root / 'cache/filters').exists())
        finally:
            hidden.rename(helper)
        (self.root / 'Filters.txt').write_text('from route@example.org move First\n')
        self.run_cli('sieve', engine=None)
        self.assertIn(b'header :contains', (self.root / 'Filters.sieve').read_bytes())
        self.assertFalse((self.root / 'cache/filter-engine/current').exists())
        self.run_cli('sieve', engine='legacy')
        self.message.write_bytes(b'From: route@example.org\n\nDefault engine probe.\n')
        self.run_cli('filter', engine=None)
        survivors = [p.read_bytes() for p in (self.root / 'unsorted').rglob('*') if p.is_file() and p.parent.name in ('cur','new')]
        self.assertEqual(survivors, [b'From: route@example.org\n\nDefault engine probe.\n'])

    def test_errors_preserve_export_and_stop_indexing(self):
        self.run_cli('sieve')
        before = self.snapshot()
        (self.root / '.notmuch/xapian').mkdir(parents=True)
        for rules in ('invalid rule\n', 'from unsupported+ move Bad\n'):
            (self.root / 'Filters.txt').write_text(rules)
            self.run_cli('update', code=4)
            self.assertEqual(self.snapshot(), before)
            self.assertFalse(self.indexlog.exists())
        self.assertTrue(self.message.exists())

    def test_fetch_compile_apply_pipeline_and_failures(self):
        body = self.message.read_bytes()
        (self.root / '.notmuch/xapian').mkdir(parents=True)
        self.run_cli('fetch')
        self.assertEqual(self.transportlog.read_text(), 'fetch\n')
        self.assertIn(b'"First"', self.snapshot()[1])
        self.assertFalse(self.message.exists())
        self.assertEqual((self.root / 'First/cur/disposable').read_bytes(), body)
        self.assertEqual(sum(line.split()[-1] == 'new' for line in self.indexlog.read_text().splitlines()), 1)
        self.indexlog.unlink()
        before = self.snapshot()
        (self.root / 'Filters.txt').write_text('invalid rule\n')
        self.run_cli('fetch', code=4)
        self.assertEqual(self.snapshot(), before)
        self.env['JARO_WORKFLOW_TRANSPORT_STATUS'] = '9'
        self.run_cli('fetch', code=9)

    def test_compile_process_counts_do_not_grow_per_rule(self):
        counts = []
        for size in (1, 1000):
            (self.root / 'Filters.txt').write_text(''.join('known-domain d%d.example.org\n' % i for i in range(size)))
            self.processlog.write_text('')
            self.run_cli('sieve')
            counts.append(self.processlog.read_text().splitlines())
        self.assertTrue(counts[0], "instrumentation must observe the actual workflow")
        self.assertEqual(counts[0], counts[1])
        self.assertLess(len(counts[1]), 20)

    def test_custom_update_root_and_native_dry_check(self):
        second = self.root.parent / 'selected mail root'
        shutil.copytree(self.root, second)
        self.run_cli('update', str(second))
        self.assertTrue((second / 'cache/filter-engine/current').is_symlink())
        self.assertFalse((self.root / 'Filters.sieve').exists())
        self.run_cli('-n', 'sieve')
        self.assertFalse((self.root / 'Filters.sieve').exists())
        self.assertFalse((self.root / 'cache/filter-engine').exists())

    def test_legacy_export_bridge_holds_native_publication_lock(self):
        (self.root / 'Filters.txt').write_text('from route@example.org move First\n')
        self.run_cli('sieve', engine='legacy')
        before = (self.root / 'Filters.sieve').read_bytes()
        notify, write = os.pipe()
        read, release = os.pipe()
        command = [str(self.runtime / 'bin/jaro-filter-legacy'), '--root', str(self.root), '--',
                   'zsh', '-f', '-c', 'source "$1" source || exit $?; '
                   'print -n -u "$2" 1; read -rk 1 -u "$3"; legacy_update_sieve',
                   'zsh', str(self.runtime / 'bin/jaro'), str(write), str(read)]
        child = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 pass_fds=(write, read))
        os.close(write)
        os.close(read)
        ready = False
        try:
            self.assertTrue(select.select([notify], [], [], 20)[0], 'bridge did not reach held command')
            self.assertEqual(os.read(notify, 1), b'1')
            ready = True
            self.run_cli('sieve', code=6)
            self.assertEqual((self.root / 'Filters.sieve').read_bytes(), before)
        finally:
            try:
                os.write(release, b'1')
            except BrokenPipeError:
                pass
            out, err = child.communicate(timeout=20)
            os.close(notify)
            os.close(release)
            self.events.append({'command': command, 'cwd': str(REPO), 'exit': child.returncode,
                                'held_ready': ready, 'settled': child.poll() is not None,
                                'stdout_hex': out.hex(), 'stderr_hex': err.hex(),
                                'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(child.returncode, 0, err)

    def test_native_readonly_requires_current_generation_and_import_rejects_non_sieve(self):
        before = self.message.read_bytes()
        self.run_cli('-n', 'filter', str(self.root / 'incoming'), code=5)
        self.assertEqual(self.message.read_bytes(), before)
        self.assertFalse((self.root / 'Filters.sieve').exists())
        self.run_cli('sieve')
        snapshot = self.tree_snapshot()
        self.run_cli('-n', 'filter', str(self.root / 'incoming'))
        self.run_cli('filter', '--explain', str(self.root / 'incoming'))
        self.run_cli('-n', 'fetch')
        self.assertEqual(self.tree_snapshot(), snapshot)
        self.assertFalse(self.transportlog.exists())
        self.assertFalse(self.indexlog.exists())
        (self.root / 'Aliases.txt').write_text('changed@example.org\n')
        snapshot = self.tree_snapshot()
        self.run_cli('filter', '--explain', code=5)
        self.assertEqual(self.tree_snapshot(), snapshot)
        self.run_cli('sieve-import', str(self.root / 'Filters.txt'), code=4)

    def test_native_import_validates_without_bootstrap_writes_then_publishes(self):
        donor = self.root.parent / 'import donor'
        donor.mkdir()
        (donor / 'Filters.txt').write_text('from-domain imported.example move NativeImport\nknown-domain *.known.example\n')
        (donor / 'Aliases.txt').write_text('imported-alias@example.org\n')
        result = subprocess.run([str(self.runtime / 'bin/jaro-filter-compile'), '--root', str(donor)],
                                env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        script = donor / 'Filters.sieve'
        before = self.tree_snapshot()
        self.run_cli('-n', 'sieve-import', str(script))
        self.assertEqual(self.tree_snapshot(), before)
        self.assertFalse((self.root / 'cache').exists())
        self.run_cli('sieve-import', code=2)
        self.run_cli('sieve-import', str(script), str(script), code=2)
        self.assertEqual(self.tree_snapshot(), before)
        self.run_cli('sieve-import', str(script))
        self.assertIn('NativeImport', (self.root / 'Filters.txt').read_text())
        self.assertIn('known-domain', (self.root / 'Filters.txt').read_text())
        self.assertIn('imported-alias@example.org', (self.root / 'Aliases.txt').read_text())
        self.run_cli('sieve-import', str(script))  # complete committed-journal cleanup
        before = self.tree_snapshot()
        self.run_cli('sieve-import', str(script))
        self.assertEqual(self.tree_snapshot(), before)
        self.assertFalse(self.indexlog.exists())
        self.assertFalse(self.transportlog.exists())
        self.assertTrue(self.message.exists())

    def test_native_import_legacy_option_and_invalid_script_do_not_change_inputs(self):
        script = self.root.parent / 'old generated.sieve'
        script.write_text('require ["fileinto","mailbox","variables"]; fileinto :create "unsorted";\n')
        before = self.tree_snapshot()
        self.run_cli('-n', 'sieve-import', '--legacy-generated', str(script))
        self.assertEqual(self.tree_snapshot(), before)
        self.run_cli('sieve-import', str(script), code=4)
        self.assertEqual({k:v for k,v in self.tree_snapshot().items() if k != 'cache' and not k.startswith('cache/')}, before)
        self.run_cli('sieve-import', '--legacy-generated', str(script))
        self.assertIn(b'W_LEGACY_SIEVE', self.run_cli('sieve-import', '--legacy-generated', str(script)).stderr)

    def tree_snapshot(self):
        import hashlib
        return {str(p.relative_to(self.root)): (p.lstat().st_mode,
                os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest()
                if p.is_file() else None) for p in self.root.rglob('*')}

    def index_count(self):
        return sum(line.split()[-1] == 'new' for line in self.indexlog.read_text().splitlines()) if self.indexlog.exists() else 0

    def test_actual_filter_raw_paths_empty_noop_and_source_entry(self):
        body = self.message.read_bytes()
        self.message.unlink()
        names = [b'has space:2,RS', b'has\nnewline:2,S', b'-leading:2,F', b'raw-\xff:2,S', b'ends-\xff\n']
        for name in names:
            path = os.fsencode(self.message.parent)+b'/'+name
            with open(path, 'wb') as f:
                f.write(body+name)
        result = self.run_cli('filter')
        self.assertIn(b'5 processed, 0 errors', result.stderr)
        self.assertIn(b'\\n', result.stdout)
        self.assertIn(b'\\377', result.stdout)
        for name in names:
            with open(os.fsencode(self.root / 'First/cur')+b'/'+name, 'rb') as f:
                self.assertEqual(f.read(), body+name)
        self.assertEqual(self.index_count(), 0)
        self.run_cli('filter', str(self.root / 'First'))
        self.assertIn(b'[noop]', bytes.fromhex(self.events[-1]['stdout_hex']))
        self.run_cli('filter')
        self.assertIn(b'0 processed, 0 errors', bytes.fromhex(self.events[-1]['stderr_hex']))
        result = subprocess.run([str(self.source / 'src/jaro'), 'filter', '--explain', str(self.root / 'First')],
                                cwd=self.root.parent, env=dict(self.env, JARO_FILTER_ENGINE='native'), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'[noop]', result.stdout)

    def test_native_filter_and_fetch_partial_errors_and_index_status(self):
        empty = self.root / 'incoming/new/empty'
        empty.touch()
        (self.root / '.notmuch/xapian').mkdir(parents=True)
        self.run_cli('fetch', code=7)
        self.assertTrue(empty.exists())
        self.assertTrue((self.root / 'First/cur/disposable').exists())
        self.assertEqual(self.index_count(), 1)
        self.env['JARO_WORKFLOW_INDEX_STATUS'] = '12'
        self.run_cli('fetch', code=7)
        self.assertEqual(self.index_count(), 2)
        empty.unlink()
        self.run_cli('fetch', code=12)
        self.assertEqual(self.index_count(), 3)

    def test_native_batch_readiness_and_no_implicit_fallback(self):
        before = self.tree_snapshot()
        helper = self.runtime / 'bin/jaro-filter'
        hidden = helper.with_name('batch-away')
        helper.rename(hidden)
        try:
            self.run_cli('fetch', code=127)
            self.run_cli('filter', code=127)
            self.assertEqual(self.tree_snapshot(), before)
            self.assertFalse(self.transportlog.exists())
        finally:
            hidden.rename(helper)
        for engine in ('', 'unknown'):
            self.run_cli('filter', code=2, engine=engine)
            self.run_cli('fetch', code=2, engine=engine)
        for engine in ('legacy', None):
            self.run_cli('filter', code=4, engine=engine)
        self.assertTrue(self.message.exists())
        self.assertFalse((self.root / 'cache/filters').exists())

    def test_jfd1_malformed_partial_and_helper_status_propagation(self):
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        header = b'JFD1\0'+generation+b'\0'
        row = [b'1', os.fsencode(self.message), b'builtin:unsorted', b'move', b'unsorted',
               os.fsencode(self.root / 'unsorted'), b'unsorted', b'moved', b'', b'0', b'']
        valid = header+b'\0'.join(row)+b'\0'
        cases = [(b'', 0, 2), (header+b'1\0partial', 0, 2),
                 (header+b'1\0partial', 9, 9), (valid, 9, 9),
                 (valid.replace(b'builtin:unsorted', b'bad-id'), 0, 2),
                 (valid.replace(b'moved', b'classified'), 0, 2),
                 (b'JFD1\0'+b'0'*64+b'\0', 0, 2)]
        helper = self.runtime / 'bin/jaro-filter'
        original = helper.read_bytes()
        (self.root / '.notmuch/xapian').mkdir(parents=True)
        body = self.message.read_bytes()
        try:
            for stream, status, expected in cases:
                helper.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.buffer.write(%r)\nsys.exit(%d)\n' % (stream, status))
                result = self.run_cli('fetch', code=expected)
                self.assertNotIn(b'0 errors', result.stderr if not stream.startswith(valid) else b'')
                self.assertEqual(self.message.read_bytes(), body)
            self.assertEqual(self.index_count(), len(cases))
        finally:
            helper.write_bytes(original)

    def test_native_batch_process_counts_do_not_grow_per_message(self):
        counts = []
        for size in (1, 100):
            for i in range(size-1):
                (self.message.parent / ('msg%d' % i)).write_bytes(self.message.read_bytes())
            self.run_cli('sieve')
            self.processlog.write_text('')
            self.run_cli('-n', 'filter')
            counts.append(self.processlog.read_text().splitlines())
        self.assertEqual(counts[0], counts[1])
        self.assertEqual(counts[0].count('jaro-filter-compile'), 1)
        self.assertEqual(counts[0].count('jaro-filter'), 1)
        for name in ('maddr', 'mpick', 'mseq', 'sort', 'uniq'):
            self.assertNotIn(name, counts[1])

    def test_binary_batch_capture_rejects_raw_trailing_lf_and_preserves_status(self):
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        header = b'JFD1\0'+generation+b'\0'
        row = [b'1',os.fsencode(self.message),b'builtin:unsorted',b'move',b'unsorted',
               os.fsencode(self.root / 'unsorted'),b'unsorted',b'classified',b'',b'0',b'']
        valid = header+b'\0'.join(row)+b'\0'
        cases = [(header+b'\n',0,2),(header+b'\n\n',0,2),(valid+b'\n',0,2),
                 (valid+b'\n\n',0,2),(header+b'1\0partial\n',0,2),
                 (header+b'\n',9,9),(valid+b'\n\n',9,9),(header+b'partial\n',9,9)]
        helper = self.runtime / 'bin/jaro-filter'
        original = helper.read_bytes()
        before = self.tree_snapshot()
        try:
            for stream,status,expected in cases:
                with self.subTest(stream=stream,status=status):
                    helper.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.buffer.write(%r)\nsys.exit(%d)\n' % (stream,status))
                    result = self.run_cli('filter','--explain',code=expected)
                    self.assertEqual(result.stdout,b'')
                    self.assertNotIn(b'Native filter:',result.stderr)
                    self.assertNotIn(b'compare path=',result.stdout)
                    self.assertEqual(self.tree_snapshot(),before)
            helper.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.buffer.write(%r)\n' % header)
            self.message.unlink()
            result = self.run_cli('filter','--explain')
            self.assertEqual(result.stdout,b'')
            self.assertIn(b'0 processed, 0 errors, helper status 0',result.stderr)
        finally:
            helper.write_bytes(original)

    def test_binary_compiler_capture_rejects_raw_trailing_lf_and_preserves_status(self):
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        valid = b'JFC1\0'+generation+b'\0' + b'0\0\0'
        cases = [(valid+b'\n',0,5),(valid+b'\n\n',0,5),(valid[:-1]+b'\n',0,5),
                 (valid,9,9),(valid+b'\n',9,9),(b'JFC1\0partial\n\n',9,9)]
        helper = self.runtime / 'bin/jaro-filter-compile'
        original = helper.read_bytes()
        before = self.tree_snapshot()
        try:
            for stream,status,expected in cases:
                with self.subTest(stream=stream,status=status):
                    helper.write_text('#!/usr/bin/env python3\nimport sys\nsys.stdout.buffer.write(%r)\nsys.exit(%d)\n' % (stream,status))
                    count = self.processlog.read_text().splitlines().count('jaro-filter')
                    result = self.run_cli('filter','--explain',code=expected)
                    self.assertEqual(result.stdout,b'')
                    self.assertNotIn(b'Native filter:',result.stderr)
                    self.assertEqual(self.processlog.read_text().splitlines().count('jaro-filter'),count)
                    self.assertEqual(self.tree_snapshot(),before)
        finally:
            helper.write_bytes(original)

    def test_binary_observation_capture_rejects_suffix_lf_and_accepts_byte_paths(self):
        path = os.fsencode(self.message.parent)+b'/valid-\xff\n'
        os.rename(self.message,path)
        self.message = Path(os.fsdecode(path))
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        valid = b'JLC1\0'+generation+b'\0'+path+b'\0unsorted\0observed\0'
        observations = self.root.parent / 'binary-observations0\n'
        before = self.tree_snapshot()
        for stream in [valid+b'\n',valid+b'\n\n',valid[:-1]+b'\n',b'JLC1\0partial\n']:
            with self.subTest(stream=stream):
                observations.write_bytes(stream)
                result = self.run_cli('filter','--compare',str(observations),code=4)
                self.assertEqual(result.stdout,b'')
                self.assertNotIn(b'Native filter:',result.stderr)
                self.assertEqual(self.tree_snapshot(),before)
        observations.write_bytes(valid)
        result = self.run_cli('filter','--compare',str(observations))
        self.assertIn(b'compare path=',result.stdout)
        self.assertIn(b'\\377\\n',result.stdout)
        self.assertEqual(self.tree_snapshot(),before)
        selector = self.root / 'cache/filter-engine/current'
        selected = os.readlink(selector)
        selector.unlink()
        selector.symlink_to(selected+'\n')
        before = self.tree_snapshot()
        result = self.run_cli('filter','--explain',code=5)
        self.assertEqual(result.stdout,b'')
        self.assertEqual(self.tree_snapshot(),before)

    def test_native_readonly_bootstrap_creates_no_defaults_or_cache(self):
        before = self.tree_snapshot()
        self.run_cli('filter', '--explain', code=5)
        self.assertEqual(self.tree_snapshot(), before)
        self.assertFalse((self.root / 'whitelist.abook').exists())
        self.assertFalse((self.root / 'Applications.txt').exists())
        self.run_cli('filter', '', code=2)
        self.run_cli('filter', '--compare', code=2)
        self.run_cli('filter', '--explain', engine='legacy', code=2)

    def test_destdir_install_custom_prefix_and_fetch_account_flag(self):
        stage = self.root.parent / 'staged install'
        prefix = '/opt/jaro test prefix'
        command = ['make', 'install', 'PREFIX='+prefix, 'DESTDIR='+str(stage)]
        with (self.root.parent / 'staged-install.log').open('wb') as log:
            result = subprocess.run(command, cwd=self.source, env=self.build_env, stdout=log, stderr=subprocess.STDOUT)
        self.events.append({'command': command, 'cwd': str(self.source), 'exit': result.returncode,
                            'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(result.returncode, 0)
        runtime = stage / prefix.lstrip('/') / 'share/jaromail'
        self.assertTrue((runtime / 'bin/jaro-filter').is_file())
        env = dict(self.env, JAROWORKDIR=str(runtime), JARO_FILTER_ENGINE='native')
        body = self.message.read_bytes()
        result = subprocess.run([str(runtime / 'bin/jaro'), 'filter'], cwd=self.root.parent, env=env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'First/cur/disposable').read_bytes(), body)
        self.assertEqual(self.index_count(), 0)
        (self.accounts / 'selected.txt').write_text('name Selected\nemail selected@example.org\nlogin selected\nproto pop\nhost fixture.invalid\nport 995\n')
        self.run_cli('-a', 'selected', 'fetch')
        self.assertEqual((self.root.parent / 'selected-account').read_text(), 'selected\n')

    def test_compare_retained_actual_legacy_observation_is_decision_only(self):
        # Run the actual retained engine on a disposable copy. The independently
        # recorded baseline says From falls through to unsorted; native routes it.
        (self.root / 'Filters.txt').write_text('from route@example.org move First\n')
        self.message.write_bytes(b'From: route@example.org\n\nIndependent From-only fixture.\n')
        legacy_root = self.root.parent / 'legacy observation mail'
        shutil.copytree(self.root, legacy_root)
        env = dict(self.env, JAROMAILDIR=str(legacy_root), JARO_FILTER_ENGINE='legacy')
        result = subprocess.run([str(self.cli), 'filter'], env=env, cwd=self.root.parent, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        found = [p for p in legacy_root.glob('*/cur/*') if p.read_bytes() == self.message.read_bytes()]
        self.assertEqual([p.parent.parent.name for p in found], ['unsorted'])
        import hashlib
        qualification = hashlib.sha256((REPO / 'doc/filter-regex-qualification.json').read_bytes()).hexdigest()
        (self.root / 'FilterProfile.txt').write_text('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\ncapability mailbox\ncapability regex\nregex-qualification '+qualification+'\n')
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        observations = self.root.parent / 'actual-legacy.observations0'
        observations.write_bytes(b'JLC1\0'+generation+b'\0'+os.fsencode(self.message)+b'\0unsorted\0observed\0')
        before = self.tree_snapshot()
        result = self.run_cli('filter', '--compare', str(observations))
        self.assertIn(b'legacy=', result.stdout)
        self.assertIn(b'unsorted', result.stdout)
        self.assertIn(b'First', result.stdout)
        self.assertEqual(self.tree_snapshot(), before)

    def test_compare_full_retained_legacy_corpus_and_approved_corrections(self):
        fixtures = REPO / 'extras/test/fixtures/filter-policy'
        target = json.loads((fixtures / 'target.json').read_text())
        retained = json.loads((fixtures / 'legacy.json').read_text())['destinations']
        cases = target['cases']
        self.message.unlink()
        (self.root / 'Filters.txt').write_text('\n'.join(target['rules'])+'\n')
        for book, addresses in [('whitelist', target['known']), ('blacklist', target['blacklist'])]:
            (self.root / (book+'.abook')).write_text('[format]\nprogram=abook\nversion=0.6.1\n'+''.join(
                '\n[%d]\nname=Fixture\nemail=%s\n' % (i,a) for i,a in enumerate(addresses)))
        for case in cases:
            headers = {'Message-ID': '<%s@fixtures.invalid>' % case['id'], 'Subject':case['id'], **case['headers']}
            (self.message.parent / case['id']).write_bytes(('\n'.join('%s: %s' % pair for pair in headers.items())+'\n\nFull corpus body.\n').encode())
        legacy_root = self.root.parent / 'legacy corpus'
        shutil.copytree(self.root, legacy_root)
        env = dict(self.env, JAROMAILDIR=str(legacy_root), JARO_FILTER_ENGINE='legacy')
        result = subprocess.run([str(self.cli), 'filter'], env=env, cwd=self.root.parent, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = {}
        for path in legacy_root.glob('*/cur/*'):
            data = path.read_bytes()
            ident = next(line[13:-18].decode() for line in data.splitlines() if line.startswith(b'Message-ID: <'))
            actual[ident] = path.parent.parent.name
            self.assertEqual(data, (self.message.parent / ident).read_bytes())
        self.assertEqual(actual, retained)
        import hashlib
        qualification = hashlib.sha256((REPO / 'doc/filter-regex-qualification.json').read_bytes()).hexdigest()
        (self.root / 'FilterProfile.txt').write_text('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\ncapability mailbox\ncapability regex\nregex-qualification '+qualification+'\n')
        self.run_cli('sieve')
        generation = os.readlink(self.root / 'cache/filter-engine/current').split('/')[-1].encode()
        observations = self.root.parent / 'legacy-corpus.observations0'
        observations.write_bytes(b'JLC1\0'+generation+b'\0'+b''.join(
            os.fsencode(self.message.parent / ident)+b'\0'+actual[ident].encode()+b'\0observed\0' for ident in sorted(actual)))
        before = self.tree_snapshot()
        result = self.run_cli('filter', '--compare', str(observations))
        comparisons = [line for line in result.stdout.splitlines() if line.startswith(b'compare ')]
        self.assertEqual(len(comparisons), 23)
        for case in cases:
            line = next(line for line in comparisons if ("/"+case['id']+"'").encode() in line)
            self.assertTrue(line.endswith(("legacy=$'%s' native=$'%s'" % (retained[case['id']], case['local'])).encode()), line)
        self.assertEqual(self.tree_snapshot(), before)
        self.assertEqual(self.index_count(), 0)
        observations.write_bytes(observations.read_bytes().replace(b'observed', b'ambiguous'))
        self.run_cli('filter', '--compare', str(observations), code=4)
        observations.write_bytes(b'legacy proposal -> unsorted\n')
        self.run_cli('filter', '--compare', str(observations), code=4)
        self.assertEqual(self.tree_snapshot(), before)

    def test_install_removes_stale_zsh_module_caches(self):
        module = self.runtime / 'zlibs/filters'
        original, metadata = module.read_bytes(), module.stat()
        module.write_text('filter_engine() { REPLY=legacy; }\nupdate_filters() { return 42; }\n')
        subprocess.run(['zsh', '-f', '-c', 'zcompile "$1"', 'zsh', str(module)], check=True)
        module.write_bytes(original)
        os.utime(module, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        self.run_cli('sieve', code=42)
        command = ['make', 'install', 'PREFIX='+str(self.prefix)]
        with (self.work / 'reinstall.log').open('wb') as log:
            result = subprocess.run(command, cwd=self.source, env=self.build_env, stdout=log, stderr=subprocess.STDOUT)
        self.events.append({'command': command, 'cwd': str(self.source), 'CFLAGS': self.build_env.get('CFLAGS'),
                            'exit': result.returncode, 'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(result.returncode, 0)
        self.assertFalse(Path(str(module)+'.zwc').exists())
        self.run_cli('sieve')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    summary = {'executed': result.testsRun, 'failed': len(result.errors)+len(result.failures),
               'skipped': len(result.skipped), 'total': result.testsRun,
               'observedAt': datetime.now(timezone.utc).isoformat(), 'all_commands_waited': True}
    (WorkflowTests.work / 'summary.json').write_text(json.dumps(summary, sort_keys=True)+'\n')
    print('Workflow tests: '+json.dumps(summary, sort_keys=True))
    if WorkflowTests.temp:
        WorkflowTests.temp.cleanup()
    raise SystemExit(0 if result.wasSuccessful() else 1)
