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
        cls.work = Path(os.environ.get('JARO_FILTER_WORKFLOW_WORK') or tempfile.mkdtemp(prefix='jaro-workflows-'))
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
  return ${JARO_WORKFLOW_TRANSPORT_STATUS:-0}
}
''')
        cls.bins = cls.work / 'instrumented bin'
        cls.bins.mkdir()
        for name in ('cat', 'awk', 'abook', 'find', 'date'):
            real = shutil.which(name)
            script = cls.bins / name
            script.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                              'with open(os.environ["JARO_WORKFLOW_PROCESS_LOG"],"ab") as f: f.write(%r)\n'
                              'os.execv(%r,[%r]+sys.argv[1:])\n' % ((name+'\n').encode(), real, real))
            script.chmod(0o700)
        (cls.bins / 'notmuch').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$JARO_WORKFLOW_INDEX_LOG"\nexit 0\n')
        (cls.bins / 'notmuch').chmod(0o700)

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2)+'\n')

    def setUp(self):
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

    def test_fetch_compile_pipeline_stops_before_missing_native_batch(self):
        body = self.message.read_bytes()
        self.run_cli('fetch', code=127)
        self.assertEqual(self.transportlog.read_text(), 'fetch\n')
        self.assertIn(b'"First"', self.snapshot()[1])
        self.assertEqual(self.message.read_bytes(), body)
        self.assertFalse(self.indexlog.exists())
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

    def test_native_batch_and_import_are_explicitly_unavailable(self):
        before = self.message.read_bytes()
        self.run_cli('-n', 'filter', str(self.root / 'incoming'), code=127)
        self.assertEqual(self.message.read_bytes(), before)
        self.assertFalse((self.root / 'Filters.sieve').exists())
        self.run_cli('sieve-import', str(self.root / 'Filters.txt'), code=4)
        self.assertFalse((self.root / 'Filters.sieve').exists())

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
    raise SystemExit(0 if result.wasSuccessful() else 1)
