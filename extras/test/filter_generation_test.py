"""Real compiler transactions, with pipe-held preconditions and settled children."""
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import fcntl
import json
import os
import re
from pathlib import Path
import select
import shutil
import signal
import stat
import sqlite3
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
        self.root.mkdir(mode=0o700)
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
        self.assertEqual(set(p.name for p in first[2].iterdir()),
                         {'dependencies', 'Filters.sieve', 'Filters.svbin', 'provenance', 'manifest.json'})
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

    def test_runtime_binding_and_bytecode_reuse(self):
        runtime = self.case / 'runtime'
        shutil.copytree(self.objects / 'jaro-sieve-runtime', runtime)
        self.env['JARO_SIEVE_RUNTIME'] = str(runtime)
        first = self.run_compile()
        binary = first[2] / 'Filters.svbin'
        original = binary.read_bytes()
        manifest = json.loads((first[2] / 'manifest.json').read_text())
        for field, path in [('svbin_sha256', binary), ('sieve_sha256', first[2] / 'Filters.sieve'),
                            ('provenance_sha256', first[2] / 'provenance'),
                            ('runtime_sha256', runtime / 'SHA256SUMS')]:
            self.assertEqual(manifest[field], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(manifest['profile_sha256'], hashlib.sha256(b'').hexdigest())
        self.assertEqual(self.run_compile(extra={'JF_TEST_FAIL': 'sieve-compile'}), (first[0], 0, first[2]))
        # A valid different runtime at the same path must invalidate the warm result.
        config = runtime / 'dovecot.conf'
        old_digest = hashlib.sha256(config.read_bytes()).hexdigest()
        config.write_bytes(config.read_bytes() + b'\n# updated runtime configuration\n')
        sums = runtime / 'SHA256SUMS'
        sums.write_text(sums.read_text().replace(old_digest, hashlib.sha256(config.read_bytes()).hexdigest()))
        self.run_compile(expected=3, extra={'JF_TEST_FAIL': 'sieve-compile'})
        self.assertEqual(binary.read_bytes(), original)
        second = self.run_compile()
        self.assertNotEqual(first[0], second[0])
        config.unlink()
        self.assertIn(b'E_SIEVE_RUNTIME', self.run_compile(expected=3).stderr)
        self.assertEqual(self.selected(), 'generations/' + second[0])

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
            self.root.mkdir(mode=0o700)
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
        self.contact('allow', 'known@example.org', 'Known')
        ids.append(self.run_compile()[0])
        with closing(sqlite3.connect(self.root / 'contacts.sqlite')) as db, db:
            db.execute('PRAGMA foreign_keys=ON')
            db.execute('DELETE FROM contacts')
        ids.append(self.run_compile()[0])
        (self.root / 'FilterProfile.txt').write_text('version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\n')
        ids.append(self.run_compile()[0])
        self.assertEqual(len(set(ids)), 6)
        self.assertNotEqual(ids[2], ids[3])
        self.assertEqual(ids[3], ids[5])
        self.assertEqual(len(list((self.engine / 'generations').iterdir())), 6)

    def test_pinned_reader_survives_publication_and_returns_original_source(self):
        old = self.run_compile()
        held = Held(self, 'pinned', [str(self.reader), str(self.root)])
        held.ready()
        binary_reader = Held(self, 'pinned', [str(self.reader), str(self.root), '--bytecode'])
        binary_reader.ready()
        self.rules.write_text('from-address second@example.org move Second\n')
        new = self.run_compile()
        self.assertNotEqual(old[0], new[0])
        held.resume()
        code, out, err = held.finish()
        self.assertEqual(code, 0, err)
        self.assertEqual(out, old[0].encode()+b'\0'+(old[2] / 'Filters.sieve').read_bytes())
        binary_reader.resume()
        code, out, err = binary_reader.finish()
        self.assertEqual(code, 0, err)
        self.assertEqual(out, old[0].encode()+b'\0'+(old[2] / 'Filters.svbin').read_bytes())
        self.assertNotEqual(out[65:], (new[2] / 'Filters.svbin').read_bytes())
        self.assertNotEqual((new[2] / "Filters.sieve").read_bytes(), (old[2] / "Filters.sieve").read_bytes())

    def test_corruption_version_and_output_symlink_rejected(self):
        for name, mutation in [('dependencies', lambda b: b[:8]+b'\0\0\0\0'+b[12:]),
                               ('provenance', lambda b: b[:-1]),
                               ('Filters.svbin', lambda b: b[:-1]), ('manifest.json', lambda b: b+b'extra')]:
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
        phases = ['stage-name', 'write-product', 'write-sieve', 'write-provenance', 'sieve-compile', 'fsync-bytecode', 'write-manifest', 'fsync-file', 'fsync-stage',
                  'validate', 'rename-generation', 'fsync-generations', 'write-backup', 'write-export',
                  'write-journal', 'fsync-journal', 'prepared', 'rename-export', 'export-renamed',
                  'rename-current', 'selector-renamed', 'fsync-root', 'fsync-engine', 'write-commit',
                  'commit-written', 'rename-commit', 'fsync-commit']
        for index, phase in enumerate(phases):
            self.rules.write_text('to-address person%d@example.org move Dest%d\n' % (index, index))
            self.run_compile(expected=3, extra={'JF_TEST_FAIL': phase})
            self.assert_old(before)

    def test_failed_old_format_migration_restores_publication_and_mail(self):
        old = 'a' * 64
        generation = self.engine / 'generations' / old
        generation.mkdir(parents=True)
        for directory in (self.engine, generation.parent, generation):
            directory.chmod(0o700)
        source = b'require "fileinto"; fileinto "prior";\n'
        manifest = {'schema': 1, 'generation': old, 'policy_sha256': hashlib.sha256(b'JFNCACHE').hexdigest(),
                    'zsh_sha256': hashlib.sha256(b'').hexdigest(), 'sieve_sha256': hashlib.sha256(source).hexdigest()}
        for path, contents in [(generation / 'policy.bin', b'JFNCACHE'), (generation / 'filters.zsh', b''),
                               (generation / 'Filters.sieve', source),
                               (self.root / 'Filters.sieve', source),
                               (generation / 'manifest.json', json.dumps(manifest, separators=(',', ':')).encode())]:
            path.write_bytes(contents)
            path.chmod(0o600)
        (self.root / 'Filters.sieve').chmod(0o640)
        current = self.engine / 'current'
        current.symlink_to('generations/' + old)
        inbox = self.root / 'incoming/new/message'
        inbox.parent.mkdir(parents=True)
        original = b'From: first@example.org\n\nRetain the entire body.\n'
        inbox.write_bytes(original)
        for phase in ('sieve-compile', 'export-renamed', 'selector-renamed'):
            self.run_compile(expected=3, extra={'JF_TEST_FAIL': phase})
            self.assertEqual(current.resolve(), generation)
            self.assertEqual((self.root / 'Filters.sieve').read_bytes(), source)
            self.assertEqual(stat.S_IMODE((self.root / 'Filters.sieve').stat().st_mode), 0o640)
            self.assertEqual(inbox.read_bytes(), original)
        _, _, published = self.run_compile()
        self.assertEqual(current.resolve(), published)
        self.assertNotEqual(published, generation)
        self.assertEqual(inbox.read_bytes(), original)

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

    def test_persistent_membership_reuse_ignores_names_and_notes(self):
        self.contact('allow', 'known@example.org', 'Known')
        initial = self.run_compile()
        with closing(sqlite3.connect(self.root / 'contacts.sqlite')) as db, db:
            db.execute("UPDATE contacts SET name='Edited', notes='New notes', revision=revision+1")
        self.assertEqual(self.run_compile(extra={'JF_TEST_FAIL': 'parse'}),
                         (initial[0], 0, initial[2]))
        self.contact('block', 'known@example.org', 'Overlap')
        overlapping = self.run_compile()
        self.assertNotEqual(overlapping[0], initial[0])
        self.assertEqual((overlapping[2] / 'Filters.sieve').read_bytes(),
                         (initial[2] / 'Filters.sieve').read_bytes())
        self.contact('block', 'block-only@example.org', 'Block only')
        changed = self.run_compile()
        self.assertNotEqual(changed[0], overlapping[0])
        self.assertIn(b'block-only@example.org', (changed[2] / 'Filters.sieve').read_bytes())

    def test_fresh_jaromail_zero_byte_books_compile(self):
        for name in ('whitelist.abook', 'blacklist.abook'):
            (self.root / name).touch()
        check = self.run_compile('--check')
        first = self.run_compile()
        self.assertEqual(check[0], first[0])
        self.assertEqual(self.run_compile()[1], 0)
        self.assertEqual((self.root / 'whitelist.abook').read_bytes(), b'')
        self.assertEqual((self.root / 'blacklist.abook').read_bytes(), b'')

    def test_empty_legacy_variants_share_one_logical_generation(self):
        from filter_compiler_test import records
        # Exercise the production helper, including the exact accepted L1 input.
        self.compiler = self.native
        variants = [b'# empty synthetic blacklist\n', b'', b' \t\r\n\n',
                    b'  # comment\r\n\t# another comment',
                    b'# abook addressbook file\n\n[format]\nprogram=abook\nversion=0.6.1\n',
                    b' [format] \r\n# header comment\n program=abook\t\nversion=0.6.0']
        self.assertEqual(hashlib.sha256(variants[0]).hexdigest(),
                         '7c69d957d8589345ae5afbb6740d42e0f88c9686ed5c71881de101c3c8df0ddf')
        identities = []
        for index, data in enumerate(variants):
            for name in ('whitelist.abook', 'blacklist.abook'):
                (self.root / name).write_bytes(b'' if index == 0 and name == 'whitelist.abook' else data)
            before = {p: p.read_bytes() for p in self.root.glob('*.abook')}
            check = self.run_compile('--check')
            first = self.run_compile()
            dependencies = {(row[0], row[1]): row for tag, row in
                            records((first[2] / 'dependencies').read_bytes()) if tag == b'D'}
            for role in (b'contacts.allow', b'contacts.block'):
                self.assertEqual(dependencies[role, os.fsencode(self.root / 'contacts.sqlite')][2:],
                                 [b'1', hashlib.sha256(b'').digest(), b''])
            identities.append(first[0])
            self.assertEqual(first[:2], (check[0], int(index == 0)))
            products = {p: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes())
                        for p in first[2].iterdir()}
            self.assertEqual(self.run_compile(), (first[0], 0, first[2]))
            self.assertEqual({p: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes())
                              for p in products}, products)
            self.assertEqual({p: p.read_bytes() for p in before}, before)
            self.assertEqual(stat.S_IMODE(first[2].stat().st_mode), 0o700)
            for path in first[2].iterdir():
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(len(set(identities)), 1, 'legacy bytes affected membership fingerprint')
        message = self.case / 'unknown.eml'
        message.write_bytes(b'From: unknown@example.org\n\nUnchanged body.\n')
        self.oracle.compile(first[2] / 'Filters.sieve')
        self.oracle.expect(first[2] / 'Filters.sieve', message, 'unsorted', True)

    def test_accepted_benchmark_books_compile_without_fixture_changes(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('filter_baseline_fixture', REPO / 'extras/bench/filter_baseline.py')
        baseline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(baseline)
        self.compiler = self.native
        self.root = self.case / 'accepted fixture'
        fixture = baseline.make_fixture(self.root, 10, 8)
        self.assertEqual(hashlib.sha256((self.root / 'blacklist.abook').read_bytes()).hexdigest(),
                         '7c69d957d8589345ae5afbb6740d42e0f88c9686ed5c71881de101c3c8df0ddf')
        qualification = hashlib.sha256((REPO / 'docs/filter-regex-qualification.json').read_bytes()).hexdigest()
        (self.root / 'FilterProfile.txt').write_text(
            'version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\n'
            'capability mailbox\ncapability regex\nregex-qualification ' + qualification + '\n')
        first = self.run_compile()
        self.assertEqual(self.run_compile(), (first[0], 0, first[2]))
        for name, digest in fixture['inputs'].items():
            self.assertEqual(hashlib.sha256((self.root / name).read_bytes()).hexdigest(), digest)
        self.oracle.compile(first[2] / 'Filters.sieve')
        messages = sorted((self.root / 'incoming').glob('*/*'))
        self.assertEqual(len(messages), 8)
        for message in messages:
            ident = message.name.split(':')[0]
            expected = fixture['target_expected'][ident]
            self.oracle.expect(first[2] / 'Filters.sieve', message,
                               'INBOX' if expected == 'known' else expected, True)

    def test_storage_failure_keeps_previous_generation_and_archive_is_ignored(self):
        self.compiler = self.native
        book = self.root / 'whitelist.abook'
        book.write_bytes(b'[0]\nname=Known\nemail=known@example.org\n')
        first = self.run_compile()
        self.run_compile()
        before = self.snapshot()
        database = self.root / 'contacts.sqlite'
        valid = database.read_bytes()
        for invalid in (b'', b'garbage', b'SQLite format 3\x00'):
            database.write_bytes(invalid)
            self.assertIn(b'E_CONTACT_STORAGE', self.run_compile(expected=3).stderr)
            self.assert_old(before)
        database.write_bytes(valid)
        book.write_bytes(b'malformed archive')
        self.assertEqual(self.run_compile(), (first[0], 0, first[2]))
        book.unlink()
        self.assertEqual(self.run_compile(), (first[0], 0, first[2]))

    def test_raw_paths_and_input_link_targets_are_fingerprinted(self):
        self.root = Path(os.fsdecode(os.fsencode(self.case) + b'/raw\n\xff root'))
        self.root.mkdir(mode=0o700)
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

    def test_writer_during_snapshot_cannot_split_memberships_and_invalidates_generation(self):
        self.contact('allow', 'old-allow@example.org', 'Allow')
        self.contact('block', 'old-block@example.org', 'Block')
        compiler = Held(self, 'contacts-allow-read,snapshot')
        compiler.ready()
        with closing(sqlite3.connect(self.root / 'contacts.sqlite', timeout=0.1)) as writer:
            writer.execute('BEGIN IMMEDIATE')
            writer.execute("UPDATE addresses SET spelling=replace(spelling,'old-','new-'), "
                           "mailbox_key=replace(mailbox_key,'old-','new-')")
            with self.assertRaises(sqlite3.OperationalError) as held:
                writer.commit()
            self.assertEqual(held.exception.sqlite_errorcode, sqlite3.SQLITE_BUSY)
            compiler.resume()
            compiler.ready()
            writer.commit()
            compiler.resume()
        compiler.ready()
        compiler.resume()
        compiler.ready()
        compiler.resume()
        code, out, err = compiler.finish()
        self.assertEqual(code, 0, err)
        generation = self.engine / 'generations' / out.split(b'\0')[1].decode()
        script = (generation / 'Filters.sieve').read_bytes()
        self.assertIn(b'new-allow@example.org', script)
        self.assertIn(b'new-block@example.org', script)
        self.assertNotIn(b'old-', script)
        self.assertEqual(self.run_compile()[0], generation.name)

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

    def contact(self, book, email, name, expected=0, check=False):
        command = [str(self.native), '--add-contact', str(self.root), book, email, name]
        if check:
            command.append('--check')
        result = subprocess.run(command, env=self.env, capture_output=True, timeout=10)
        self.event(command, result.returncode, result.stdout, result.stderr)
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertEqual(result.stdout, b'')
        if expected in (0, 1):
            self.assertEqual(result.stderr, b'')
        return result

    def test_learn_preserves_migrated_fields_and_private_database_permissions(self):
        book = self.root / 'whitelist.abook'
        original = b'# preserve comment\n[7]\nname=Existing\nemail=existing@example.org\ncustom=preserve me\n'
        book.write_bytes(original)
        book.chmod(0o640)
        inode = book.stat().st_ino
        email = 'a'*50 + '@' + 'b'*50 + '.example.org'
        name = 'Last, "First" (Team) \\ Full unabridged display name'
        self.contact('allow', email, name)
        self.assertEqual(book.read_bytes(), original)
        self.assertEqual(book.stat().st_ino, inode)
        self.assertEqual(stat.S_IMODE(book.stat().st_mode), 0o640)
        database = self.root / 'contacts.sqlite'
        self.assertEqual(stat.S_IMODE(database.stat().st_mode), 0o600)
        query = subprocess.run([str(self.native), '--query-contacts', str(self.root), 'allow', ''],
                               env=self.env, capture_output=True, check=True)
        columns = [row.split(b'\t') for row in query.stdout.splitlines() if b'\t' in row]
        self.assertIn([email.encode(), name.encode(), b' '], columns)
        self.assertIn([b'existing@example.org', b'Existing', b' '], columns)
        with closing(sqlite3.connect(database)) as db, db:
            self.assertEqual(db.execute('SELECT key,value FROM fields').fetchall(),
                             [('custom', 'preserve me')])

    def test_contact_duplicate_dry_run_and_invalid_fields_leave_store_unchanged(self):
        database = self.root / 'contacts.sqlite'
        self.contact('allow', 'other@example.org', 'Check only', check=True)
        self.assertFalse(database.exists())
        self.contact('allow', 'Name@Example.Org', 'Original name')
        before = database.read_bytes()
        for email in ['name@example.org', '"name"@EXAMPLE.ORG']:
            self.contact('allow', email, 'Do not replace', expected=1)
        self.contact('custom', 'other@example.org', 'Check only', check=True)
        self.contact('allow', 'person@invalid.example...', 'Invalid', expected=4)
        self.assertEqual(database.read_bytes(), before)
        self.contact('allow', '"a,b"@example.org', 'Valid multiline\ntext')
        query = subprocess.run([str(self.native), '--query-contacts', str(self.root), 'allow', 'multiline'],
                               env=self.env, capture_output=True, check=True)
        self.assertEqual(query.stdout, b'\n"a,b"@example.org\tValid multiline text\t \n')
        for email in ['bad@host...', '""@example.org']:
            result = subprocess.run([str(self.native), '--check-address', email], capture_output=True)
            self.assertEqual(result.returncode, 4)

    def test_contact_lock_conflict_and_nonregular_target_are_safe(self):
        database = self.root / 'contacts.sqlite'
        self.contact('allow', 'existing@example.org', 'Existing')
        before = database.read_bytes()
        with closing(sqlite3.connect(database)) as held, held:
            held.execute('BEGIN IMMEDIATE')
            blocked = self.contact('allow', 'new@example.org', 'New', expected=6)
            self.assertIn(b'E_CONTACT_BUSY', blocked.stderr)
            self.assertEqual(database.read_bytes(), before)
        self.contact('allow', 'new@example.org', 'New')
        self.assertNotEqual(database.read_bytes(), before)
        database.unlink()
        database.mkdir()
        self.contact('allow', 'new@example.org', 'New', expected=3)
        database.rmdir()
        os.mkfifo(database)
        self.contact('allow', 'new@example.org', 'New', expected=3)

    def test_migration_rejects_invalid_mailboxes_instead_of_publishing_partial_membership(self):
        data = b'[0]\nname=Good\nemail=known@example.org\n[1]\nname=Damaged\nemail=person@invalid.example...\n'
        book = self.root / 'whitelist.abook'
        book.write_bytes(data)
        self.assertIn(b'E_CONTACT_STORAGE', self.run_compile('--check', expected=3).stderr)
        self.assertFalse((self.root / 'cache').exists())
        self.assertIn(b'E_CONTACT_STORAGE', self.run_compile(expected=3).stderr)
        self.assertFalse((self.root / 'contacts.sqlite').exists())
        self.assertEqual(book.read_bytes(), data)

    def test_alias_errors_stay_fatal_when_sqlite_is_healthy(self):
        self.contact('allow', 'known@example.org', 'Known')
        self.run_compile()
        self.run_compile()
        before = self.snapshot()
        (self.root / 'Aliases.txt').write_text('not-a-mailbox\n')
        failed = self.run_compile(expected=4)
        self.assertIn(b'E_ADDRESS', failed.stderr)
        self.assert_old(before)


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
