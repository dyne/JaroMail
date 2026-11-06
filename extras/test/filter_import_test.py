"""Bounded real Sieve imports: canonical products and journal failure recovery.

No Sieve interpreter lives here. Execution against Pigeonhole is in the parity
suite; these tests decode the documented cache and exercise real publications.
"""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import select
import stat
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
from filter_compiler_test import records

REPO = Path(__file__).resolve().parents[2]
FIXTURE = json.loads((REPO / 'extras/test/fixtures/filter-policy/roundtrip.json').read_text())
FILES = ['Filters.txt', 'Aliases.txt', 'whitelist.abook', 'blacklist.abook']


class ImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get('JARO_FILTER_IMPORT_WORK')
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            cls.work.mkdir(parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-import-', dir=os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-generation-tools', 'filter-compile',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler = cls.objects / 'jaro-filter-compile-test'
        cls.reader = cls.objects / 'filter-generation-pin'
        cls.events = []

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()
        self.source = self.case / 'source'
        self.source.mkdir()
        self.root = self.case / 'destination'
        self.root.mkdir()
        self.fixture_root(self.source)
        self.fixture_root(self.root, empty=True)
        self.compile(self.source)
        self.script = self.case / 'source script.sieve'
        self.script.write_bytes((self.source / 'Filters.sieve').read_bytes())

    def fixture_root(self, root, empty=False):
        (root / 'Filters.txt').write_text('' if empty else '\n'.join(FIXTURE['rules']) + '\n')
        (root / 'FilterProfile.txt').write_text(FIXTURE['profile'])
        (root / 'Aliases.txt').write_text('' if empty else '\n'.join(FIXTURE['own']) + '\n')
        if not empty:
            for name in ('known', 'blacklist'):
                filename = 'whitelist.abook' if name == 'known' else 'blacklist.abook'
                (root / filename).write_text('\n'.join('[%d]\nname=Fixture\nemail=%s\n' % (i, x)
                                                     for i, x in enumerate(FIXTURE[name])))

    def run_command(self, command, expected=0, extra=None):
        env = dict(os.environ, **(extra or {}))
        result = subprocess.run(list(map(str, command)), cwd=REPO, env=env, capture_output=True, timeout=30)
        self.events.append({'command': list(map(str, command)), 'cwd': str(REPO), 'exit': result.returncode,
                            'stdout_hex': result.stdout.hex(), 'stderr': result.stderr.decode('utf8', 'replace'),
                            'faults': extra, 'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def compile(self, root=None, *args, expected=0, extra=None):
        root = root or self.root
        result = self.run_command([self.compiler, '--root', root, '--report0', *args], expected, extra)
        if expected:
            self.assertEqual(result.stdout, b'')
        else:
            fields = result.stdout.split(b'\0')
            self.assertEqual(len(fields), 5)
            self.assertEqual(fields[0], b'JFC1')
        return result

    def do_import(self, expected=0, extra=None, args=()):
        return self.compile(self.root, '--import-sieve', str(self.script), *args, expected=expected, extra=extra)

    def policy(self, root):
        generation = (root / 'cache/filter-engine/current').resolve(strict=True)
        # Ignore IDs and source lines, but compare every behavioral field/order.
        return [tuple(row[i] for i in (1, 2, 3, 5, 6, 7, 8, 11))
                for tag, row in records((generation / 'policy.bin').read_bytes()) if tag == b'R']

    def snapshot(self):
        result = {}
        for name in FILES + ['Filters.sieve', 'cache/filter-engine/current']:
            path = self.root / name
            if path.is_symlink():
                result[name] = ('symlink', os.readlink(path))
            elif path.exists():
                result[name] = ('file', stat.S_IMODE(path.stat().st_mode), path.read_bytes())
            else:
                result[name] = None
        return result

    def assert_restored(self, before, allow_abandoned=False):
        self.assertEqual(self.snapshot(), before)
        engine = self.root / 'cache/filter-engine'
        self.assertFalse((engine / 'import.journal').exists())
        self.assertFalse((engine / 'journal').exists())
        if not allow_abandoned:
            self.assertEqual(list(engine.glob('.building-*')), [])

    def test_domain_address_mapping_order_roundtrip_and_idempotency(self):
        first = self.do_import()
        self.assertEqual(self.policy(self.source), self.policy(self.root))
        self.assertEqual((self.source / 'Filters.sieve').read_bytes(), (self.root / 'Filters.sieve').read_bytes())
        before = self.snapshot()
        second = self.do_import()
        self.assertEqual(second.stdout.split(b'\0')[2], b'0')
        self.assertEqual(first.stdout.split(b'\0')[1], second.stdout.split(b'\0')[1])
        self.assertEqual(self.snapshot(), before)

    def test_comments_whitespace_tag_order_and_string_lists(self):
        text = self.script.read_text().replace('require [', '/* audit comments are irrelevant */\nReQuIrE [')
        text = text.replace('address :all :is :comparator "i;ascii-casemap"',
                            'address :comparator "i;ascii-casemap" :is :all')
        text = text.replace('"From","Sender"', '"Sender", # unordered fields\n "From"')
        text = text.replace('"team@project.example" {', '["team@project.example"] {')
        self.script.write_text(text.replace('; }', ';\r\n }').replace('{', '{ /* block */\n'))
        self.do_import()
        self.assertEqual(self.policy(self.source), self.policy(self.root))

    def test_check_builds_complete_prospective_policy_without_writes(self):
        before = self.snapshot()
        check = self.do_import(args=('--check',))
        self.assertEqual(check.stdout.split(b'\0')[2:4], [b'0', b''])
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.root / 'cache').exists())
        self.assertEqual(check.stdout.split(b'\0')[1], self.do_import().stdout.split(b'\0')[1])

    def test_preserve_unrelated_rules_accounts_books_and_original_source(self):
        rules = b'# retain formatting\nfrom-address unrelated@elsewhere.example move Other\n'
        (self.root / 'Filters.txt').write_bytes(rules)
        (self.root / 'Accounts').mkdir()
        account = self.root / 'Accounts/account'
        account.write_text('email own@home.example\n')
        book = b'# original contact\n[99]\nname=Existing Name\nemail=existing@elsewhere.example\n'
        (self.root / 'whitelist.abook').write_bytes(book)
        (self.root / 'Aliases.txt').write_text('# keep aliases comment\n')
        script = self.script.read_bytes()
        self.do_import()
        self.assertTrue((self.root / 'Filters.txt').read_bytes().startswith(rules))
        self.assertTrue((self.root / 'whitelist.abook').read_bytes().startswith(book))
        self.assertEqual(account.read_text(), 'email own@home.example\n')
        self.assertNotIn('own@home.example', (self.root / 'Aliases.txt').read_text())
        self.assertEqual(self.script.read_bytes(), script)
        self.do_import()

    def test_fail_closed_unsupported_tests_actions_extensions_and_truncations(self):
        self.compile()
        # Recover the preceding successful publication before taking the baseline.
        self.compile()
        before = self.snapshot()
        valid = self.script.read_text()
        variants = [valid[:-1] + ' redirect "x@example.org";', valid.replace('address :domain', 'envelope :domain', 1),
                    valid.replace('fileinto :create "Vendors & Friends";', 'discard;', 1),
                    valid.replace('"i;ascii-casemap"', '"i;octet"', 1),
                    valid.replace('"mailbox"]', '"mailbox","include"]', 1),
                    valid.replace('address :all :is :comparator', 'address :all :contains :comparator', 1),
                    valid.replace('if true {', 'if allof (true) {', 1),
                    valid.replace('"vendor.example"', '"bad..example"', 1),
                    valid.replace('stop;', 'keep;', 1), valid + '/* unterminated',
                    valid.replace('"vendor.example"', 'text:\nignored\n.\n', 1),
                    valid.replace('"vendor.example"', '"\\0"', 1), valid + '\x00']
        variants += [valid[:cut] for cut in (0, 1, 20, len(valid)//2, len(valid)-10)]
        for text in variants:
            with self.subTest(script=text[:60]):
                self.script.write_text(text)
                result = self.do_import(expected=4)
                self.assertIn(b'E_', result.stderr)
                self.assert_restored(before)

    def test_existing_selector_destination_and_source_order_collisions(self):
        for rules in ('from-domain vendor.example move WRONG\n',
                      'from-address exact@vendor.example move exact\nfrom-domain vendor.example move vendors\n',
                      'from-address exact@vendor.example move exact\n'):
            (self.root / 'Filters.txt').write_text(rules)
            self.compile()
            self.compile()
            before = self.snapshot()
            self.do_import(expected=4)
            self.assert_restored(before)

    def test_ambiguous_mailbox_mapping_rejected(self):
        profile = FIXTURE['profile'] + 'mailbox exact "Vendors & Friends"\n'
        (self.root / 'FilterProfile.txt').write_text(profile)
        before = self.snapshot()
        result = self.do_import(expected=4)
        self.assertIn(b'ambiguous mailbox', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_named_mapping_and_noninvertible_namespace_fail_closed(self):
        (self.source / 'whitelist.abook').unlink()
        (self.source / 'blacklist.abook').unlink()
        for profile, rules in (('version 1\nprofile core-v1\ncapability fileinto\ncapability mailbox\n',
                                'from-address one@example.org move INBOX\n'),
                               ('version 1\nprofile core-v1\ncapability fileinto\ncapability mailbox\nnamespace-separator .\n',
                                'from-address one@example.org move nested/folder\n')):
            (self.source / 'Filters.txt').write_text(rules)
            (self.source / 'FilterProfile.txt').write_text(profile)
            (self.root / 'FilterProfile.txt').write_text(profile)
            self.compile(self.source)
            self.script.write_bytes((self.source / 'Filters.sieve').read_bytes())
            before = self.snapshot()
            self.assertIn(b'ambiguous', self.do_import(expected=4).stderr)
            self.assertEqual(self.snapshot(), before)

    def test_input_change_during_staging_is_detected_and_lock_blocks_readers(self):
        self.compile()
        self.compile()
        before = self.snapshot()
        notify, notify_write = os.pipe()
        release_read, release = os.pipe()
        env = dict(os.environ, JF_TEST_HOLD='validate', JF_TEST_NOTIFY_FD=str(notify_write),
                   JF_TEST_RELEASE_FD=str(release_read))
        command = [str(self.compiler), '--root', str(self.root), '--import-sieve', str(self.script)]
        child = subprocess.Popen(command, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 pass_fds=(notify_write, release_read))
        os.close(notify_write)
        os.close(release_read)
        try:
            self.assertTrue(select.select([notify], [], [], 20)[0], 'stage precondition not reached')
            self.assertEqual(os.read(notify, 1), b'1')
            self.run_command([self.reader, self.root], expected=6)
            self.compile(self.root, '--check', expected=6)
            (self.root / 'Filters.txt').write_bytes(b'# concurrent editor retained\n')
            before['Filters.txt'] = ('file', stat.S_IMODE((self.root / 'Filters.txt').stat().st_mode), b'# concurrent editor retained\n')
            os.write(release, b'1')
            out, err = child.communicate(timeout=20)
            self.assertEqual(child.returncode, 6, err)
            self.assertEqual(out, b'')
            self.events.append({'command': command, 'cwd': str(REPO), 'exit': child.returncode,
                                'held': 'validate', 'settled': child.poll() is not None,
                                'stderr': err.decode(), 'observedAt': datetime.now(timezone.utc).isoformat()})
            self.assert_restored(before)
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate(timeout=5)
            os.close(notify)
            os.close(release)

    def test_reject_duplicate_or_missing_builtin_and_predicate_precedence(self):
        valid = self.script.read_text()
        line = next(x for x in valid.splitlines(True) if '"vendor.example"' in x)
        bounce = next(x for x in valid.splitlines(True) if '"mailman-bounce"' in x)
        for text in (valid.replace(line, line + line.replace('Vendors & Friends', 'Box/WRONG')),
                     valid.replace(bounce, ''), valid.replace(bounce, bounce+bounce)):
            self.script.write_text(text)
            before = self.snapshot()
            self.do_import(expected=4)
            self.assertEqual(self.snapshot(), before)

    def test_failure_during_every_multi_file_publication_phase(self):
        self.compile()
        self.compile()
        before = self.snapshot()
        phases = ['import-backup', 'import-stage', 'import-journal', 'import-fsync-journal', 'import-prepared',
                  'import-rename-input', 'import-input-renamed', 'import-fsync-root', 'import-inputs-published',
                  'write-backup', 'write-export', 'write-journal', 'prepared', 'rename-export', 'export-renamed',
                  'rename-current', 'selector-renamed', 'fsync-root', 'fsync-engine', 'write-commit', 'commit-written',
                  'rename-commit', 'fsync-commit', 'committed']
        for phase in phases:
            for at in (range(1, 5) if phase in ('import-rename-input', 'import-input-renamed') else (1,)):
                with self.subTest(phase=phase, at=at):
                    self.do_import(expected=3, extra={'JF_TEST_FAIL': phase, 'JF_TEST_FAIL_AT': str(at)})
                    self.assert_restored(before)
                    self.assertEqual(self.run_command([self.reader, self.root]).returncode, 0)

    def test_crash_rollback_and_committed_recovery(self):
        self.compile()
        self.compile()
        before = self.snapshot()
        for phase in ('import-prepared', 'import-input-renamed', 'import-inputs-published', 'prepared', 'selector-renamed'):
            self.do_import(expected=-signal.SIGKILL, extra={'JF_TEST_CRASH': phase})
            self.run_command([self.reader, self.root], expected=5)
            self.compile(self.root, '--check', expected=5)
            self.compile()
            # A killed owner can leave its private immutable build directory.
            # Recovery never sweeps another owner's staging.
            self.assert_restored(before, allow_abandoned=True)
        self.do_import(expected=-signal.SIGKILL, extra={'JF_TEST_CRASH': 'committed'})
        self.run_command([self.reader, self.root])
        self.compile()
        self.assertEqual(self.policy(self.source), self.policy(self.root))
        self.assertFalse((self.root / 'cache/filter-engine/import.journal').exists())

    def test_input_symlink_fails_without_replacement(self):
        (self.root / 'Aliases.txt').unlink()
        target = self.case / 'real aliases'
        target.write_bytes(b'')
        (self.root / 'Aliases.txt').symlink_to(target)
        before = self.snapshot()
        self.do_import(expected=5)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(target.read_bytes(), b'')

    def test_production_executable_does_not_enable_faults(self):
        self.run_command([self.objects / 'jaro-filter-compile', '--root', self.root,
                          '--import-sieve', self.script], extra={'JF_TEST_FAIL': 'import-input-renamed'})
        self.assertEqual(self.policy(self.source), self.policy(self.root))

    def regex_profile(self):
        digest = hashlib.sha256((REPO / 'doc/filter-regex-qualification.json').read_bytes()).hexdigest()
        return ('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\n'
                'capability mailbox\ncapability regex\nregex-qualification ' + digest + '\n')

    def test_legacy_generated_recovery_is_explicit_qualified_and_idempotent(self):
        self.script.write_bytes((REPO / 'extras/test/fixtures/filter-policy/legacy-generated.txt').read_bytes())
        (self.root / 'FilterProfile.txt').write_text(self.regex_profile())
        (self.root / 'Accounts').mkdir()
        account = self.root / 'Accounts/account'
        account.write_text('email own@example.org\n')
        before = self.snapshot()
        self.do_import(expected=4)
        self.assertEqual(self.snapshot(), before)
        result = self.do_import(args=('--legacy-generated',))
        self.assertIn(b'W_LEGACY_SIEVE', result.stderr)
        semantic = self.policy(self.root)
        routes = [(x[0], x[3], x[4]) for x in semantic if x[0] in (b'3', b'4')]
        self.assertEqual(routes, [(b'3', b'list@example.org', b'lists'), (b'4', b'team@example.org', b'team')])
        self.assertEqual((self.root / 'Aliases.txt').read_text(), '"alias@example.org"\n')
        self.assertEqual(sum(x[0] == b'1' for x in semantic), 1)
        self.assertEqual(sum(x[0] == b'5' for x in semantic), 1)
        before = self.snapshot()
        self.do_import(args=('--legacy-generated',))
        self.assertEqual(self.snapshot(), before)
        self.script.write_bytes((self.root / 'Filters.sieve').read_bytes())
        self.do_import()
        self.assertEqual(self.snapshot(), before)

    def test_legacy_route_needs_qualified_export_before_any_write(self):
        self.script.write_bytes((REPO / 'extras/test/fixtures/filter-policy/legacy-generated.txt').read_bytes())
        (self.root / 'FilterProfile.txt').write_text('')
        before = self.snapshot()
        result = self.do_import(expected=4, args=('--legacy-generated',))
        self.assertIn(b'E_REGEX_EXPORT', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_legacy_omitted_sections_and_unsupported_commands(self):
        (self.root / 'FilterProfile.txt').write_text('')
        self.script.write_text('require ["fileinto", "mailbox", "variables"];\nfileinto :create "unsorted";\n')
        self.do_import(args=('--legacy-generated',))
        self.assertEqual(len(self.policy(self.root)), 3)
        self.compile()
        before = self.snapshot()
        for text in ('require ["fileinto","mailbox","variables"]; set "name" "value"; fileinto :create "unsorted";',
                     'require ["fileinto","mailbox"]; if body :contains "x" { fileinto :create "x"; stop; } fileinto :create "unsorted";',
                     'require ["fileinto","mailbox"]; if header :contains "Subject" "x" { fileinto :create "x"; stop; } fileinto :create "unsorted";',
                     'require ["fileinto","mailbox"]; fileinto :create "unsorted"; redirect "x@example.org";'):
            self.script.write_text(text)
            self.do_import(expected=4, args=('--legacy-generated',))
            self.assert_restored(before)

    def test_invalid_predicate_reports_original_script_physical_line(self):
        text = '\n' * 13 + self.script.read_text().replace('"vendor.example"', '"bad..example"', 1)
        line = next(i for i, value in enumerate(text.splitlines(), 1) if 'bad..example' in value)
        self.script.write_text(text)
        result = self.do_import(expected=4)
        self.assertIn((':%d ' % line).encode(), result.stderr)
        self.assertIn(os.fsencode(self.script), result.stderr)

    def test_empty_policy_core_without_mailbox_extension_and_native_regex_roundtrips(self):
        for rules, profile in (('', 'version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\n'),
                               ('from "^list\\\\+news@example[.]org$" move Lists\n', self.regex_profile())):
            for name in FILES:
                path = self.source / name
                if path.exists():
                    path.unlink()
            (self.source / 'Filters.txt').write_text(rules)
            (self.source / 'FilterProfile.txt').write_text(profile)
            (self.root / 'FilterProfile.txt').write_text(profile)
            self.compile(self.source)
            self.script.write_bytes((self.source / 'Filters.sieve').read_bytes())
            self.do_import()
            self.assertEqual(self.policy(self.source), self.policy(self.root))


if __name__ == '__main__':
    unittest.main(verbosity=2)
