"""Real compiler transactions, with pipe-held preconditions and settled children."""
from datetime import datetime, timezone
import hashlib
import json
import os
import re
from pathlib import Path
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
from sieve_oracle import SieveOracle

REPO = Path(__file__).resolve().parents[2]


class Held:
    def __init__(self, owner, phase, command=None, extra=None):
        self.owner = owner
        self.phase, self.finished = phase, False
        self.notify, write = os.pipe()
        read, self.release = os.pipe()
        env = dict(owner.env, JF_TEST_HOLD=phase, JF_TEST_NOTIFY_FD=str(write), JF_TEST_RELEASE_FD=str(read))
        env.update(extra or {})
        self.environment = env
        self.process = subprocess.Popen(command or owner.command(), env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, pass_fds=(write, read))
        os.close(write)
        os.close(read)
        owner.held.append(self)
        owner.events.append({'control': 'started', 'phase': phase, 'pid': self.process.pid,
                             'observedAt': datetime.now(timezone.utc).isoformat()})

    def ready(self):
        if not select.select([self.notify], [], [], 20)[0] or os.read(self.notify, 1) != b'1':
            raise AssertionError('held precondition not reached; child status %r' % self.process.poll())
        self.owner.events.append({'control': 'ready', 'phase': self.phase, 'pid': self.process.pid,
                                  'observedAt': datetime.now(timezone.utc).isoformat()})

    def resume(self):
        os.write(self.release, b'1')
        self.owner.events.append({'control': 'released', 'phase': self.phase, 'pid': self.process.pid,
                                  'observedAt': datetime.now(timezone.utc).isoformat()})

    def finish(self):
        out, err = self.process.communicate(timeout=20)
        self.finished = True
        self.owner.event(self.process.args, self.process.returncode, out, err, self.environment)
        self.owner.events.append({'control': 'settled', 'phase': self.phase, 'pid': self.process.pid,
                                  'exit': self.process.returncode, 'observedAt': datetime.now(timezone.utc).isoformat()})
        return self.process.returncode, out, err

    def close(self):
        if not self.finished:
            if self.process.poll() is None:
                self.process.terminate()
            try:
                out, err = self.process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                out, err = self.process.communicate(timeout=5)
            self.finished = True
            self.owner.event(self.process.args, self.process.returncode, out, err, self.environment)
            self.owner.events.append({'control': 'settled', 'phase': self.phase, 'pid': self.process.pid,
                                      'exit': self.process.returncode, 'cleanup': True,
                                      'observedAt': datetime.now(timezone.utc).isoformat()})
        os.close(self.notify)
        os.close(self.release)


class GenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ.get('JARO_FILTER_GENERATION_WORK')
        cls.temp = None
        if path:
            cls.work = Path(path)
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-generation-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-generation-tools', 'filter-compile',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler, cls.reader = cls.objects / 'jaro-filter-compile-test', cls.objects / 'filter-generation-pin'
        cls.native = cls.objects / 'jaro-filter-compile'
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])
        cls.events = []

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()
        self.root = self.case / 'mail root'
        self.root.mkdir()
        self.rules = self.root / 'Filters.txt'
        self.rules.write_text('from-address first@example.org move First\n')
        self.env = dict(os.environ)
        self.held = []

    def tearDown(self):
        for child in self.held:
            child.close()
            self.assertIsNotNone(child.process.poll(), 'owned process not settled')

    @property
    def engine(self):
        return self.root / 'cache/filter-engine'

    def event(self, command, code, out, err, environment=None):
        environment = environment or self.env
        self.events.append({'command': list(map(str, command)), 'cwd': str(REPO), 'exit': code,
                            'stdout_hex': out.hex(), 'stderr': err.decode('ascii'),
                            'environment': {k: v for k, v in environment.items() if k.startswith('JF_TEST_') or k in
                                            ('PATH', 'LD_LIBRARY_PATH', 'CCACHE_DIR', 'SIEVE_DEPS_PREFIX',
                                             'SIEVE_TOOLS_OUTPUT', 'ASAN_OPTIONS', 'UBSAN_OPTIONS')},
                            'observedAt': datetime.now(timezone.utc).isoformat()})

    def command(self, *args):
        return [str(self.compiler), '--root', str(self.root), '--report0', *args]

    def run_compile(self, *args, expected=0, extra=None):
        env = dict(self.env, **(extra or {}))
        result = subprocess.run(self.command(*args), env=env, capture_output=True, timeout=30)
        self.event(self.command(*args), result.returncode, result.stdout, result.stderr, env)
        self.assertEqual(result.returncode, expected, result.stderr)
        if expected:
            self.assertEqual(result.stdout, b'')
            if extra and 'JF_TEST_FAIL' in extra:
                self.assertIn(('INJECTED:' + extra['JF_TEST_FAIL'] + '\n').encode(), result.stderr)
            return result
        fields = result.stdout.split(b'\0')
        self.assertEqual(len(fields), 5)
        self.assertEqual(fields[0], b'JFC1')
        self.assertEqual(fields[-1], b'')
        self.assertEqual(len(fields[1]), 64)
        return fields[1].decode(), int(fields[2]), Path(os.fsdecode(fields[3])) if fields[3] else None

    def selected(self):
        return os.readlink(self.engine / 'current')

    def snapshot(self):
        return self.selected(), (self.root / 'Filters.sieve').read_bytes()

    def assert_old(self, before):
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.engine / 'journal').exists())
        self.assertEqual(list(self.engine.glob('.building-*')), [])

    def test_publication_fast_path_check_and_real_sieve(self):
        check = self.run_compile('--check')
        self.assertEqual(check[1:], (0, None))
        self.assertFalse((self.root / 'cache').exists())
        first = self.run_compile()
        self.assertEqual(first[0], check[0])
        self.assertEqual(first[1], 1)
        self.assertEqual(self.selected(), 'generations/' + first[0])
        self.assertEqual(self.run_compile('--check'), (first[0], 0, None))
        self.assertEqual(set(p.name for p in first[2].iterdir()), {'policy.bin', 'filters.zsh', 'Filters.sieve', 'manifest.json'})
        self.assertEqual(stat.S_IMODE(first[2].stat().st_mode), 0o700)
        for path in first[2].iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        before = {p: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes()) for p in first[2].iterdir()}
        self.assertEqual(self.run_compile(), (first[0], 0, first[2]))
        self.assertEqual(self.run_compile(extra={'JF_TEST_FAIL': 'parse'}), (first[0], 0, first[2]))
        self.assertEqual({p: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes()) for p in first[2].iterdir()}, before)
        message = self.case / 'message.eml'
        message.write_text('From: FIRST@EXAMPLE.ORG\n\nBody unchanged.\n')
        self.oracle.compile(first[2] / 'Filters.sieve')
        self.oracle.expect(first[2] / 'Filters.sieve', message, 'First', True)

    def test_production_build_ignores_test_fault_environment_and_cli_errors(self):
        command = [str(self.native), '--root', str(self.root), '--report0']
        result = subprocess.run(command, env=dict(self.env, JF_TEST_FAIL='write-product'), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'JFC1\0'))
        self.assertEqual(subprocess.run([str(self.reader), str(self.root)], capture_output=True).returncode, 0)
        for args in [[], ['--root'], ['--root', ''], ['--root', str(self.root), '--root', str(self.root)],
                     ['--unknown'], ['--root', str(self.root), '--check', '--check']]:
            failed = subprocess.run([str(self.native), *args], capture_output=True)
            self.assertEqual(failed.returncode, 2, failed.stderr)
            self.assertEqual(failed.stdout, b'')
        for flag in ('--help', '--version'):
            self.assertEqual(subprocess.run([str(self.native), flag], capture_output=True).returncode, 0)

    hierarchy_phases = ['fsync-hierarchy-generations', 'fsync-hierarchy-engine',
                        'fsync-hierarchy-cache', 'fsync-hierarchy-root']

    def hierarchy_trace(self):
        return re.findall(r'^TRACE:((?:fsync-hierarchy-[a-z]+)|hierarchy-synced)$',
                          self.events[-1]['stderr'], re.M)

    def test_hierarchy_durability_order_and_read_only_check(self):
        trace = {'JF_TEST_TRACE': '1'}
        self.run_compile('--check', extra=dict(trace, JF_TEST_FAIL='fsync-hierarchy-cache'))
        self.assertEqual(self.hierarchy_trace(), [])
        self.assertFalse((self.root / 'cache').exists())
        first = self.run_compile(extra=trace)
        # The final marker is reached only after all four fsync calls succeed.
        self.assertEqual(self.hierarchy_trace(), self.hierarchy_phases + ['hierarchy-synced'])
        self.assertEqual(stat.S_IMODE((self.root / 'cache').stat().st_mode), 0o700)
        before = {p: (p.stat().st_ino, p.stat().st_mtime_ns, p.stat().st_mode)
                  for p in (self.root, self.root / 'cache', self.engine, self.engine / 'generations')}
        self.assertEqual(self.run_compile('--check', extra=dict(trace, JF_TEST_FAIL='fsync-hierarchy-cache')),
                         (first[0], 0, None))
        self.assertEqual(self.hierarchy_trace(), [])
        self.assertEqual({p: (p.stat().st_ino, p.stat().st_mtime_ns, p.stat().st_mode) for p in before}, before)

    def test_hierarchy_parent_fsync_failures_are_retried_after_mkdir(self):
        for index, phase in enumerate(self.hierarchy_phases):
            self.root = self.case / phase
            self.root.mkdir()
            self.rules = self.root / 'Filters.txt'
            self.rules.write_text('known-domain example.org\n')
            extra = {'JF_TEST_TRACE': '1', 'JF_TEST_FAIL': phase}
            for _ in range(2):
                self.run_compile(expected=3, extra=extra)
                self.assertEqual(self.hierarchy_trace(), self.hierarchy_phases[:index+1])
                self.assertTrue((self.engine / 'generations').is_dir())
                self.assertFalse((self.engine / 'current').exists())
                self.assertFalse((self.root / 'Filters.sieve').exists())
                self.assertFalse((self.engine / 'journal').exists())
            self.run_compile(extra={'JF_TEST_TRACE': '1'})
            self.assertEqual(self.hierarchy_trace(), self.hierarchy_phases + ['hierarchy-synced'])

    def test_hierarchy_parent_fsync_failure_preserves_existing_policy_and_cache_mode(self):
        cache = self.root / 'cache'
        cache.mkdir(mode=0o755)
        cache.chmod(0o755)
        self.run_compile()
        self.run_compile()  # settle the accepted transaction before injecting errors
        before = self.snapshot()
        self.rules.write_text('known-domain changed.example\n')
        for index, phase in enumerate(self.hierarchy_phases):
            self.run_compile(expected=3, extra={'JF_TEST_TRACE': '1', 'JF_TEST_FAIL': phase})
            self.assertEqual(self.hierarchy_trace(), self.hierarchy_phases[:index+1])
            self.assert_old(before)
            self.assertEqual(stat.S_IMODE(cache.stat().st_mode), 0o755)
        self.assertEqual(self.run_compile(extra={'JF_TEST_TRACE': '1'})[1], 1)
        self.assertEqual(self.hierarchy_trace(), self.hierarchy_phases + ['hierarchy-synced'])
        self.assertEqual(stat.S_IMODE(cache.stat().st_mode), 0o755)

    def test_preserved_mtime_inventory_and_profile_changes(self):
        ids = [self.run_compile()[0]]
        old = self.rules.stat()
        self.rules.write_text('known-domain example.org\n')
        os.utime(self.rules, ns=(old.st_atime_ns, old.st_mtime_ns))
        ids.append(self.run_compile()[0])
        accounts = self.root / 'Accounts'
        accounts.mkdir()
        (accounts / 'new account').write_text('email own@example.org\n')
        ids.append(self.run_compile()[0])
        (accounts / 'new account').unlink()
        ids.append(self.run_compile()[0])
        book = self.root / 'whitelist.abook'
        book.write_text('[format]\nprogram=abook\nversion=0.6.0\n[0]\nname=Known\nemail=known@example.org\n')
        ids.append(self.run_compile()[0])
        book.unlink()
        ids.append(self.run_compile()[0])
        (self.root / 'FilterProfile.txt').write_text('version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\n')
        ids.append(self.run_compile()[0])
        self.assertEqual(len(set(ids)), 6)
        self.assertNotEqual(ids[2], ids[3])
        self.assertEqual(ids[3], ids[5])
        self.assertEqual(len(list((self.engine / 'generations').iterdir())), 6)

    def test_pinned_reader_survives_publication_and_stale_zwc(self):
        old = self.run_compile()
        held = Held(self, 'pinned', [str(self.reader), str(self.root)])
        held.ready()
        self.rules.write_text('from-address second@example.org move Second\n')
        new = self.run_compile()
        self.assertNotEqual(old[0], new[0])
        held.resume()
        code, out, err = held.finish()
        self.assertEqual(code, 0, err)
        self.assertEqual(out, old[0].encode()+b'\0'+(old[2] / 'filters.zsh').read_bytes())
        source = new[2] / 'filters.zsh'
        original, metadata = source.read_bytes(), source.stat()
        source.write_text('filter_rule_destinations=(Stale)\n')
        subprocess.run(['zsh', '-f', '-c', 'zcompile "$1"', 'zsh', str(source)], check=True)
        source.write_bytes(original)
        os.utime(source, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        # Exact immutable inventory detects an unexpected sibling cache.
        self.assertEqual(subprocess.run([str(self.reader), str(self.root)], capture_output=True).returncode, 5)
        stale = subprocess.run(['zsh', '-f', '-c', 'source "$1"; print -r -- $filter_rule_destinations',
                                'zsh', str(source)], capture_output=True)
        self.assertEqual(stale.stdout.strip(), b'Stale')
        result = subprocess.run(['zsh', '-f', '-c', 'exec {fd}<"$1"; source /dev/fd/$fd; exec {fd}<&-; print -r -- $filter_rule_destinations',
                                 'zsh', str(new[2] / 'filters.zsh')], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'Second', result.stdout)
        (new[2] / 'filters.zsh.zwc').unlink()

    def test_corruption_version_and_output_symlink_rejected(self):
        for name, mutation in [('policy.bin', lambda b: b[:8]+b'\0\0\0\0'+b[12:]),
                               ('filters.zsh', lambda b: b+b'changed'), ('manifest.json', lambda b: b+b'extra')]:
            current = self.run_compile()
            path = current[2] / name
            original = path.read_bytes()
            path.write_bytes(mutation(original))
            self.run_compile(expected=5)
            path.write_bytes(original)
        current = self.run_compile()
        victim = self.case / 'victim'
        victim.write_bytes(b'untouched')
        path = current[2] / 'Filters.sieve'
        original = path.read_bytes()
        path.unlink()
        path.symlink_to(victim)
        self.run_compile(expected=5)
        self.assertEqual(victim.read_bytes(), b'untouched')
        path.unlink()
        path.write_bytes(original)
        path.chmod(0o600)

    def test_faults_restore_prior_selector_and_export(self):
        self.run_compile()
        before = self.snapshot()
        phases = ['stage-name', 'write-product', 'write-zsh', 'write-sieve', 'write-manifest', 'fsync-file', 'fsync-stage',
                  'validate', 'rename-generation', 'fsync-generations', 'write-backup', 'write-export',
                  'write-journal', 'fsync-journal', 'prepared', 'rename-export', 'export-renamed',
                  'rename-current', 'selector-renamed', 'fsync-root', 'fsync-engine', 'write-commit',
                  'commit-written', 'rename-commit', 'fsync-commit']
        for index, phase in enumerate(phases):
            self.rules.write_text('to-address person%d@example.org move Dest%d\n' % (index, index))
            self.run_compile(expected=3, extra={'JF_TEST_FAIL': phase})
            self.assert_old(before)

    def test_interruption_journal_refusal_and_recovery(self):
        self.run_compile()
        before = self.snapshot()
        for index, phase in enumerate(['prepared', 'export-renamed', 'selector-renamed', 'commit-written']):
            self.rules.write_text('to-address user%d@example.org move Changed%d\n' % (index, index))
            held = Held(self, phase)
            held.ready()
            held.process.kill()
            code, _, _ = held.finish()
            self.assertEqual(code, -signal.SIGKILL)
            self.assertEqual(subprocess.run([str(self.reader), str(self.root)], capture_output=True).returncode, 5)
            # Invalid inputs force recovery without a subsequent new publication.
            self.rules.write_text('invalid-rule\n')
            self.run_compile(expected=4)
            self.assert_old(before)

    def test_cold_failures_restore_absent_public_views(self):
        for phase in ('prepared', 'export-renamed', 'selector-renamed'):
            self.run_compile(expected=3, extra={'JF_TEST_FAIL': phase})
            self.assertFalse((self.engine / 'current').exists())
            self.assertFalse((self.root / 'Filters.sieve').exists())
            self.assertFalse((self.engine / 'journal').exists())

    def test_committed_interruption_finishes_cleanup(self):
        old = self.run_compile()
        self.rules.write_text('known-domain committed.example\n')
        held = Held(self, 'committed')
        held.ready()
        held.process.kill()
        self.assertEqual(held.finish()[0], -signal.SIGKILL)
        committed = self.snapshot()
        self.assertNotEqual(committed[0], 'generations/' + old[0])
        self.assertEqual(subprocess.run([str(self.reader), str(self.root)], capture_output=True).returncode, 0)
        self.rules.write_text('invalid-rule\n')
        self.run_compile(expected=4)
        self.assert_old(committed)

    def test_existing_legacy_public_export_mode_is_restored(self):
        public = self.root / 'Filters.sieve'
        public.write_bytes(b'legacy valid export\n')
        public.chmod(0o644)
        self.run_compile(expected=3, extra={'JF_TEST_FAIL': 'selector-renamed'})
        self.assertEqual(public.read_bytes(), b'legacy valid export\n')
        self.assertEqual(stat.S_IMODE(public.stat().st_mode), 0o644)
        self.assertFalse((self.engine / 'current').exists())
        self.run_compile()
        self.assertEqual(stat.S_IMODE(public.stat().st_mode), 0o600)

    def test_retained_native_generation_repairs_a_legacy_public_export(self):
        native = self.run_compile()
        self.run_compile()  # settle the native journal before a legacy export
        public = self.root / 'Filters.sieve'
        public.write_bytes(b'legacy compatibility export\n')
        public.chmod(0o644)
        before = self.snapshot()
        self.run_compile(expected=3, extra={'JF_TEST_FAIL': 'rename-export'})
        self.assert_old(before)
        self.assertEqual(stat.S_IMODE(public.stat().st_mode), 0o644)
        repaired = self.run_compile()
        self.assertEqual(repaired[:2], (native[0], 1))
        self.assertEqual(public.read_bytes(), (native[2] / 'Filters.sieve').read_bytes())
        self.assertEqual(stat.S_IMODE(public.stat().st_mode), 0o600)

    def test_repeated_interruption_during_rollback_is_recoverable(self):
        self.run_compile()
        before = self.snapshot()
        self.rules.write_text('known-domain changed.example\n')
        publisher = Held(self, 'selector-renamed')
        publisher.ready()
        publisher.process.kill()
        self.assertEqual(publisher.finish()[0], -signal.SIGKILL)
        recovery = Held(self, 'recover-selector')
        recovery.ready()
        recovery.process.kill()
        self.assertEqual(recovery.finish()[0], -signal.SIGKILL)
        self.rules.write_text('invalid-rule\n')
        self.run_compile(expected=4)
        self.assert_old(before)

    def test_partial_unpublished_commit_marker_does_not_prevent_rollback(self):
        self.run_compile()
        before = self.snapshot()
        self.rules.write_text('known-domain partial.example\n')
        publisher = Held(self, 'selector-renamed')
        publisher.ready()
        publisher.process.kill()
        self.assertEqual(publisher.finish()[0], -signal.SIGKILL)
        (self.engine / 'committed.next').write_bytes(b'partial')
        (self.engine / 'committed.next').chmod(0o600)
        self.rules.write_text('invalid-rule\n')
        self.run_compile(expected=4)
        self.assert_old(before)

    def test_persistent_export_reuse_and_changed_book(self):
        binaries = self.case / 'bin'
        binaries.mkdir()
        counter = self.case / 'exports'
        real = shutil.which('abook')
        wrapper = binaries / 'abook'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           'with open(%r,"ab") as f: f.write(b"export\\n")\n'
                           'os.execv(%r,[%r]+sys.argv[1:])\n' % (str(counter), real, real))
        wrapper.chmod(0o700)
        self.env['PATH'] = str(binaries) + os.pathsep + self.env['PATH']
        book = self.root / 'whitelist.abook'
        book.write_text('[format]\nprogram=abook\nversion=0.6.0\n[0]\nname=Known\nemail=known@example.org\n')
        self.run_compile()
        self.run_compile()
        self.assertEqual(counter.read_bytes(), b'export\n')
        self.rules.write_text('known-domain example.org\n')
        self.run_compile()
        self.assertEqual(counter.read_bytes(), b'export\n')
        previous = book.stat()
        book.write_text(book.read_text().replace('known@example.org', 'second@example.org'))
        os.utime(book, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        self.run_compile()
        self.assertEqual(counter.read_bytes(), b'export\nexport\n')

    def test_fresh_jaromail_zero_byte_books_compile(self):
        for name in ('whitelist.abook', 'blacklist.abook'):
            (self.root / name).touch()
        check = self.run_compile('--check')
        first = self.run_compile()
        self.assertEqual(check[0], first[0])
        self.assertEqual(self.run_compile()[1], 0)
        self.assertEqual((self.root / 'whitelist.abook').read_bytes(), b'')
        self.assertEqual((self.root / 'blacklist.abook').read_bytes(), b'')

    def test_raw_paths_and_input_link_targets_are_fingerprinted(self):
        self.root = Path(os.fsdecode(os.fsencode(self.case) + b'/raw\n\xff root'))
        self.root.mkdir()
        self.rules = self.root / 'Filters.txt'
        first, second = self.root / 'rules-a', self.root / 'rules-b'
        first.write_text('known-domain example.org\n')
        second.write_bytes(first.read_bytes())
        self.rules.symlink_to(first.name)
        old = self.run_compile()
        self.rules.unlink()
        self.rules.symlink_to(second.name)
        new = self.run_compile()
        self.assertNotEqual(old[0], new[0])
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_changed_book_export_is_reused_across_source_retry(self):
        binaries = self.case / 'bin'
        binaries.mkdir()
        counter = self.case / 'exports'
        real = shutil.which('abook')
        wrapper = binaries / 'abook'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           'with open(%r,"ab") as f: f.write(b"export\\n")\n'
                           'os.execv(%r,[%r]+sys.argv[1:])\n' % (str(counter), real, real))
        wrapper.chmod(0o700)
        self.env['PATH'] = str(binaries) + os.pathsep + self.env['PATH']
        book = self.root / 'whitelist.abook'
        book.write_text('[format]\nprogram=abook\nversion=0.6.0\n[0]\nname=Known\nemail=first@example.org\n')
        compiler = Held(self, 'snapshot')
        compiler.ready()
        book.write_text(book.read_text().replace('first@example.org', 'second@example.org'))
        compiler.resume()
        compiler.ready()
        compiler.resume()
        code, _, err = compiler.finish()
        self.assertEqual(code, 0, err)
        self.assertEqual(counter.read_bytes(), b'export\nexport\n')

    def test_held_account_membership_change_is_rechecked(self):
        accounts = self.root / 'Accounts'
        accounts.mkdir()
        (accounts / 'old').write_text('email old@example.org\n')
        self.run_compile()
        compiler = Held(self, 'snapshot')
        compiler.ready()
        (accounts / 'old').unlink()
        (accounts / 'new').write_text('email new@example.org\n')
        compiler.resume()
        compiler.ready()
        compiler.resume()
        code, output, err = compiler.finish()
        self.assertEqual(code, 0, err)
        generation = Path(os.fsdecode(output.split(b'\0')[3]))
        for local, expected in [('new', 'priv'), ('old', 'unsorted')]:
            message = self.case / (local + '.eml')
            message.write_text('From: stranger@elsewhere.example\nTo: %s@example.org\n\nUnchanged body.\n' % local)
            self.oracle.expect(generation / 'Filters.sieve', message, expected, True)

    def test_held_source_changes_retry_twice_then_status_six(self):
        self.run_compile()
        before = self.snapshot()
        self.rules.write_text('known-domain initial.example\n')
        held = Held(self, 'snapshot')
        for index in range(3):
            held.ready()
            self.rules.write_text('known-domain changed%d.example\n' % index)
            held.resume()
        code, _, err = held.finish()
        self.assertEqual(code, 6, err)
        self.assert_old(before)

    def test_concurrent_compilers_settle_on_one_valid_generation(self):
        first = Held(self, 'snapshot')
        first.ready()
        second = Held(self, 'snapshot')
        second.ready()
        first.resume()
        second.resume()
        results = [first.finish(), second.finish()]
        self.assertIn(0, [r[0] for r in results])
        self.assertTrue(all(r[0] in (0, 6) for r in results), results)
        self.run_compile()
        self.assertEqual(len(list((self.engine / 'generations').iterdir())), 1)
        self.assertFalse((self.engine / 'journal').exists())

    def test_explicit_busy_lock_with_held_writer(self):
        writer = Held(self, 'prepared')
        writer.ready()
        self.run_compile(expected=6)
        self.assertEqual(subprocess.run([str(self.reader), str(self.root)], capture_output=True).returncode, 6)
        writer.resume()
        self.assertEqual(writer.finish()[0], 0)
        self.assertEqual(self.run_compile()[1], 0)

    def test_unselected_generation_is_synced_before_reuse(self):
        self.run_compile()
        before = self.snapshot()
        self.rules.write_text('known-domain reuse.example\n')
        for _ in range(2):
            self.run_compile(expected=3, extra={'JF_TEST_FAIL': 'fsync-generations'})
            self.assert_old(before)
        updated = self.run_compile()
        self.assertEqual(updated[1], 1)
        self.assertEqual(len(list((self.engine / 'generations').iterdir())), 2)


if __name__ == '__main__':
    program = unittest.main(verbosity=2, exit=False)
    result = program.result
    summary = {'executed': result.testsRun, 'failed': len(result.errors)+len(result.failures),
               'skipped': len(result.skipped), 'total': result.testsRun,
               'observedAt': datetime.now(timezone.utc).isoformat()}
    events = getattr(GenerationTests, 'events', [])
    started = {e['pid'] for e in events if e.get('control') == 'started'}
    settled = {e['pid'] for e in events if e.get('control') == 'settled'}
    summary.update(held_processes=len(started), settled_processes=len(settled),
                   all_owned_processes_settled=started == settled,
                   held_preconditions=sum(e.get('control') == 'ready' for e in events),
                   injected_fault_phases=sorted({phase for e in events for phase in re.findall(
                       r'^INJECTED:([^\n]+)$', e.get('stderr', ''), re.M)}))
    if os.environ.get('JARO_FILTER_GENERATION_WORK'):
        (Path(os.environ['JARO_FILTER_GENERATION_WORK']) / 'summary.json').write_text(json.dumps(summary, sort_keys=True)+'\n')
    print('Generation tests: ' + json.dumps(summary, sort_keys=True))
    sys.exit(0 if result.wasSuccessful() and started == settled else 1)
