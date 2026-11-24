"""Actual native products checked against independently authored contracts.

No policy evaluator or expected-winner generator lives here. The pinned Sieve
interpreter executes generated source; Python decodes the documented cache.
"""
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
from sieve_oracle import SieveOracle

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / 'extras/test/fixtures/filter-policy'
COUNTS = {'compiler_invocations': 0, 'oracle_decisions': 0, 'cache_decodes': 0}


def sha(data):
    return hashlib.sha256(data).digest()


def records(data):
    """Independent v1 decoder including fingerprint and canonical framing."""
    assert len(data) >= 92 and data[:8] == b'JFDEPS01'
    assert struct.unpack_from('<IIQ', data, 8) == (1, 1, len(data)-88)
    assert data[56:88] == sha(data[88:])
    count, = struct.unpack_from('<I', data, 88)
    pos, result, deps = 92, [], []
    for _ in range(count):
        start = pos
        tag, fields = data[pos:pos+1], struct.unpack_from('<I', data, pos+1)[0]
        pos += 5
        assert fields == {b'M': 7, b'D': 5}[tag]
        row = []
        for _ in range(fields):
            size, = struct.unpack_from('<I', data, pos)
            pos += 4
            row.append(data[pos:pos+size])
            assert len(row[-1]) == size
            pos += size
        result.append((tag, row))
        if tag == b'D':
            deps.append(data[start:pos])
    assert pos == len(data)
    assert data[24:56] == sha(b'JFFP1\0' + b''.join(deps))
    assert result[0][0] == b'M'
    keys = [(r[0], r[1]) for t, r in result if t == b'D']
    assert keys == sorted(set(keys))
    COUNTS['cache_decodes'] += 1
    return result


def provenance(generation):
    fields = (generation / 'provenance').read_bytes().split(b'\0')
    assert fields.pop() == b'' and len(fields) % 4 == 0
    return list(zip(*[iter(fields)] * 4))


class CompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get('JARO_FILTER_COMPILER_WORK')
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            assert cls.work.is_absolute()
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-compiler-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        cls.driver = cls.objects / 'filter-compiler-driver'
        cls.events = []
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-compiler-driver',
                        'FILTER_BUILD_DIR=' + str(cls.objects),
                        'CFLAGS=' + os.environ.get('JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')],
                       cwd=REPO, check=True)
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        (cls.work / 'counts.json').write_text(json.dumps(COUNTS, sort_keys=True) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()
        self.root = self.case / 'mail root'
        self.root.mkdir(mode=0o700)
        self.out_index = 0
        self.environment = dict(os.environ)

    def book(self, name, addresses):
        data = '[format]\nprogram=abook\nversion=0.6.0\n'
        for i, address in enumerate(addresses):
            data += '\n[%d]\nname=Fixture %d\nemail=%s\n' % (i, i, address)
        (self.root / (name + '.abook')).write_text(data)

    def build(self, rules='', known=(), blacklist=(), own=(), profile=None, error=None, reuse=False, accounts=None):
        (self.root / 'Filters.txt').write_text('\n'.join(rules) if isinstance(rules, list) else rules)
        (self.root / 'Aliases.txt').write_text('\n'.join(json.dumps(v) for v in own))
        if known:
            self.book('whitelist', known)
        if blacklist:
            self.book('blacklist', blacklist)
        if profile is not None:
            (self.root / 'FilterProfile.txt').write_text(profile)
        self.out_index += 1
        out = self.case / ('products-%d' % self.out_index)
        out.mkdir()
        command = [str(self.driver), str(self.root), str(out), '', str(accounts or '')]
        if reuse:
            command.append('reuse' if reuse is True else reuse)
        before = {os.fsencode(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        result = subprocess.run(command, capture_output=True, env=self.environment)
        self.assertEqual({os.fsencode(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}, before,
                         'compiler product construction changed an input')
        COUNTS['compiler_invocations'] += 1
        self.events.append({'command': command, 'cwd': str(REPO), 'exit': result.returncode,
                            'observedAt': datetime.now(timezone.utc).isoformat(),
                            'stdout_hex': result.stdout.hex(), 'stderr': result.stderr.decode('ascii')})
        if error:
            self.assertEqual(result.returncode, error[0], result.stderr)
            self.assertIn(error[1].encode(), result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertEqual(list(out.iterdir()), [])
            return result
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertEqual(len(result.stdout), 65)
        data = (out / 'dependencies').read_bytes()
        decoded = records(data)
        metadata = decoded[0][1]
        self.assertEqual(metadata[3:5], [b'ascii-mailbox-casemap-v1', b'i;ascii-casemap'])
        self.assertEqual(metadata[5], sha((out / 'provenance').read_bytes()))
        self.assertEqual(metadata[6], sha((out / 'Filters.sieve').read_bytes()))
        self.assertEqual(data[24:56].hex().encode()+b'\0', result.stdout)
        return out, decoded

    def expect(self, out, case, create=True):
        message = self.case / (case['id'] + '.eml')
        headers = case['headers']
        pairs = headers.items() if isinstance(headers, dict) else headers
        message.write_text('\n'.join('%s: %s' % tuple(pair) for pair in pairs) + '\nSubject: Fixture\n\nUnchanged body.\n')
        self.oracle.expect(out / 'Filters.sieve', message, case.get('sieve', case.get('mailbox')), create)
        COUNTS['oracle_decisions'] += 1

    def test_glob_source_fields_escapes_and_lines(self):
        pattern = r'*+Q?[x].\*\\@*.CH'
        out, decoded = self.build(['# line 1', 'to-glob ' + json.dumps(pattern) + ' move shop',
                                   'from-glob *+list@*.org move list'])
        text = (out / 'Filters.sieve').read_text()
        self.assertIn('address :all :matches :comparator "i;ascii-casemap" ["To","Cc"] ' +
                      json.dumps(r'*+q\?[x].\*\\@*.ch'), text)
        self.assertIn('["From","Sender"] "*+list@*.org"', text)
        self.assertNotIn('"regex"', text)
        locations = [int(authored) for _, rule, _, authored in provenance(out) if rule.startswith(b'r:')]
        self.assertEqual(locations, [3, 2])
        for invalid in [r'x\q', 'unfinished' + chr(92)]:
            self.build(['to-glob ' + json.dumps(invalid) + ' move folder'], error=(4, 'E_GLOB'))

    def test_independent_domain_corpus(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        out, _ = self.build(target['domain_rules'])
        self.oracle.compile(out / 'Filters.sieve')
        for case in target['domain_cases']:
            self.expect(out, case)
        for case in target['isolated_domain_cases']:
            out, _ = self.build(case['rules'])
            self.oracle.compile(out / 'Filters.sieve')
            self.expect(out, case)

    def test_independent_casing_priority_corpus(self):
        corpus = json.loads((FIXTURES / 'casing.json').read_text())
        out, _ = self.build(corpus['rules'], corpus['known'], corpus['blacklist'], corpus['own'])
        self.oracle.compile(out / 'Filters.sieve')
        for case in corpus['cases']:
            self.expect(out, case)

    def test_typed_stage_priority(self):
        out, _ = self.build(['from-address route@example.org move frombucket',
                             'to-address team@example.org move tobucket', 'known-domain example.org'],
                            known=['known@example.org', 'blocked@example.org'],
                            blacklist=['blocked@example.org'], own=['own@example.org'])
        cases = [
            ('allow-overlap-keeps-route-priority', {'From': 'blocked@example.org', 'To': 'team@example.org'}, 'tobucket'),
            ('bounce-before-from', {'Sender': 'mailman-bounce@other.example', 'From': 'route@example.org'}, 'zz.bounces'),
            ('from-before-to', {'From': 'route@example.org', 'To': 'team@example.org'}, 'frombucket'),
            ('sender-before-to', {'Sender': 'route@example.org', 'To': 'team@example.org'}, 'frombucket'),
            ('to-before-known', {'From': 'known@example.org', 'To': 'team@example.org'}, 'tobucket'),
            ('known-before-spam', {'From': 'known@example.org', 'X-Spam-Flag': 'YES'}, 'INBOX'),
            ('spam-before-own', {'X-Spam-Flag': 'YES', 'To': 'own@example.org'}, 'Spam'),
            ('own-before-fallback', {'Cc': 'own@example.org'}, 'priv'),
            ('fallback', {'From': 'stranger@elsewhere.example'}, 'unsorted')]
        self.oracle.compile(out / 'Filters.sieve')
        for ident, headers, expected in cases:
            self.expect(out, {'id': ident, 'headers': headers, 'sieve': expected})

    def test_literal_destinations_remain_data(self):
        destinations = ['Mixed CASE', 'Dollar$(touch marker)', "single'quote", 'back\\slash', '`touch marker`']
        rules = ['from-address person%d@example.org move %s' % (i, json.dumps(v)) for i, v in enumerate(destinations)]
        out, _ = self.build(rules)
        self.oracle.compile(out / 'Filters.sieve')
        for i, dest in enumerate(destinations):
            self.expect(out, {'id': str(i), 'headers': {'From': f'person{i}@example.org'}, 'sieve': dest})

    def test_quoted_local_and_spam_whitespace_oracle(self):
        out, _ = self.build(['from-address ' + json.dumps('"a b"@example.org') + ' move quoted',
                             'from-address ' + json.dumps('"a\\"b"@example.org') + ' move quoted'])
        cases = json.loads((REPO / 'extras/test/fixtures/sieve-oracle/cases.json').read_text())['cases']
        selected = [c for c in cases if c['script'] in ('quoted.sieve', 'spam.sieve')]
        self.assertEqual(len(selected), 13)
        for case in selected:
            self.expect(out, case)

    def test_namespace_mapping_capabilities_and_determinism(self):
        profile = 'version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\nnamespace-prefix "Folders."\nnamespace-separator "."\nmailbox spam "Junk Mail"\n'
        out, _ = self.build(['to-address team@example.org move Project/People'], profile=profile)
        text = (out / 'Filters.sieve').read_text()
        self.assertNotIn('variables', text)
        self.assertNotIn('mailbox', text)
        self.assertNotIn(':create', text)
        self.oracle.compile(out / 'Filters.sieve')
        self.expect(out, {'id': 'mapped-route', 'headers': {'To': 'team@example.org'}, 'sieve': 'Folders.Project.People'}, False)
        self.expect(out, {'id': 'mapped-spam', 'headers': {'X-Spam-Flag': 'YES'}, 'sieve': 'Junk Mail'}, False)
        self.expect(out, {'id': 'mapped-known', 'headers': {'From': 'known@example.org'}, 'sieve': 'Folders.unsorted'}, False)
        second, _ = self.build(['to-address team@example.org move Project/People'], profile=profile)
        for name in ('dependencies', 'Filters.sieve', 'provenance'):
            self.assertEqual((out / name).read_bytes(), (second / name).read_bytes())

    def test_rejected_profiles_and_legacy_source_diagnostics(self):
        core = 'version 1\nprofile core-v1\ncapability fileinto\ncapability mailbox\n'
        result = self.build('from ^x@example[.]org$ move folder', profile=core, error=(4, 'E_REGEX_EXPORT'))
        self.assertIn(b'Filters.txt":1', result.stderr)
        self.assertIn(b'core-v1', result.stderr)
        for profile in ['version 1\nprofile core-v1\ncapability variables\n',
                        'version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\nregex-qualification ' + 'a'*64 + '\n',
                        'version 1\nprofile core-v1\ncapability fileinto\ncapability fileinto\n',
                        'version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes yes\n',
                        'version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\nmailbox known one\nmailbox known two\n']:
            self.build(profile=profile, error=(4, 'E_'))

    def test_default_capabilities_need_no_profile(self):
        rules = ['from mute@metamute save pol.newsletters', 'to team@project move Project']
        out, _ = self.build(rules)
        self.assertFalse((self.root / 'FilterProfile.txt').exists())
        self.assertEqual((out / 'Filters.sieve').read_text().splitlines()[0],
                         'require ["fileinto","mailbox","regex"];')
        self.oracle.compile(out / 'Filters.sieve')
        for ident, headers, destination in [
                ('partial-from', {'From': 'mute@metamute.org'}, 'pol.newsletters'),
                ('partial-cc', {'From': 'other@example.org', 'Cc': 'team@project.net'}, 'Project'),
                ('partial-miss', {'From': 'other@metamute.org'}, 'unsorted')]:
            self.expect(out, {'id': ident, 'headers': headers, 'sieve': destination})
        typed, _ = self.build('from-address person@example.org move Exact')
        self.assertEqual((typed / 'Filters.sieve').read_text().splitlines()[0],
                         'require ["fileinto","mailbox"];')
        self.oracle.compile(typed / 'Filters.sieve', extensions='-regex')
        empty_profile, _ = self.build(rules, profile='')
        self.assertEqual((empty_profile / 'Filters.sieve').read_bytes(), (out / 'Filters.sieve').read_bytes())
        self.assertEqual((self.root / 'FilterProfile.txt').read_bytes(), b'')

    def test_colliding_mailbox_mappings(self):
        profile = 'version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\nmailbox first Shared\nmailbox second Shared\n'
        self.build(['from-address first@example.org move first', 'to-address second@example.org move second'],
                   profile=profile, error=(4, 'E_MAILBOX_MAPPING'))

    def test_empty_accounts_and_profile_input_limits(self):
        (self.root / 'Accounts').mkdir()
        out, decoded = self.build('')
        self.assertEqual(len(provenance(out)), 3)
        self.oracle.compile(out / 'Filters.sieve')
        self.expect(out, {'id': 'empty-policy', 'headers': {}, 'sieve': 'unsorted'})
        self.build(profile='version 1\nprofile core-v1\n' + '#' * 65537, error=(4, 'E_LINE_LIMIT'))

    def test_effective_user_rule_limit_serialization(self):
        # Independent contract limit: three mandatory builtins are additional
        # records, not user predicates. Decode and validate every serialized row.
        rules = ''.join('known-domain d%d.example.org\n' % i for i in range(100000))
        out, decoded = self.build(rules)
        rows = provenance(out)
        self.assertEqual(len(rows), 100003)
        self.assertEqual(sum(r[1].startswith(b'r:') for r in rows), 100000)
        self.assertEqual({r[1] for r in rows if r[1].startswith(b'builtin:')},
                         {b'builtin:bounce', b'builtin:spam', b'builtin:unsorted'})
        result = subprocess.run([str(self.driver), '--validate', str(out / 'dependencies'),
                                 str(out / 'provenance'), str(out / 'Filters.sieve')], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'')
        self.build(rules + 'known-domain extra.example.org\n', error=(4, 'E_RULE_LIMIT'))

    def test_payload_header_and_overflow_arithmetic(self):
        result = subprocess.run([str(self.driver), '--bounds'], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'PASS 12 product-boundary checks:'), result.stdout)

    def test_zero_byte_books_from_jaromail_initialization(self):
        for name in ('whitelist.abook', 'blacklist.abook'):
            (self.root / name).touch()
        out, decoded = self.build('')
        rows = provenance(out)
        self.assertEqual(len(rows), 3)
        for tag, row in decoded:
            if tag == b'D' and row[0] in (b'contacts.allow', b'contacts.block'):
                self.assertEqual(row[2:5], [b'1', sha(b''), b''])
        self.expect(out, {'id': 'zero-book-fallback', 'headers': {'From': 'unknown@example.org'}, 'sieve': 'unsorted'})

    def test_cache_rejects_corrupt_and_incompatible_products(self):
        out, _ = self.build('known-domain example.org')
        original = (out / 'dependencies').read_bytes()
        corrupt = self.case / 'corrupt.bin'
        command = [str(self.driver), '--validate', str(corrupt), str(out / 'provenance'), str(out / 'Filters.sieve')]
        variants = [b'', original[:87], original + b'extra']
        for offset, byte in [(0, 88), (8, 0), (12, 99), (24, original[24] ^ 1),
                             (56, original[56] ^ 1), (88, 255), (92, ord('X'))]:
            data = bytearray(original)
            data[offset] = byte
            if offset >= 88:
                data[56:88] = sha(data[88:])
            variants.append(data)
        for data in variants:
            corrupt.write_bytes(data)
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 5, result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertIn(b'E_CACHE', result.stderr)
        corrupt.write_bytes(original)
        (out / 'provenance').write_bytes(b'changed')
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 5)

    def traced_abook(self):
        binaries = self.case / 'exporter bin'
        binaries.mkdir()
        counter = self.case / 'export-count'
        real_abook = shutil.which('abook')
        self.assertIsNotNone(real_abook)
        wrapper = binaries / 'abook'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           'with open(%r,"ab") as stream: stream.write(b"export\\n")\n'
                           'os.execv(%r, [%r]+sys.argv[1:])\n' % (str(counter), real_abook, real_abook))
        wrapper.chmod(0o700)
        self.environment['PATH'] = str(binaries) + os.pathsep + os.environ['PATH']
        return counter

    def test_byte_paths_accounts_symlinks_and_contact_snapshot(self):
        # Path bytes travel through argv/cache fields without JSON decoding.
        self.root = Path(os.fsdecode(os.fsencode(self.case) + b'/raw\n\xff root'))
        self.root.mkdir(mode=0o700)
        accounts = self.case / 'accounts\nexternal'
        (accounts / 'sub dir').mkdir(parents=True)
        (accounts / 'sub dir/account').write_text('email "Own@Example.Org"\npassword NEVER-EVALUATED\n')
        source = accounts / 'sub dir/account'
        (accounts / 'linked account').symlink_to(source)
        counter = self.traced_abook()
        out, decoded = self.build(['known-domain *.example.org'], known=['"a b"@example.org'], accounts=accounts, reuse=True)
        self.assertFalse(counter.exists(), 'SQLite snapshots invoked the text exporter')
        rows = provenance(out)
        self.assertIn(os.fsencode(self.root / 'Filters.txt'), [r[2] for r in rows])
        self.assertIn(b'own:' + hashlib.sha256(b'own@example.org').hexdigest().encode(), [r[1] for r in rows])
        dependencies = {(r[0], r[1]): r for t, r in decoded if t == b'D'}
        self.assertIn((b'link.input.account', os.fsencode(accounts / 'linked account')), dependencies)
        exported = dependencies[b'contacts.allow', os.fsencode(self.root / 'contacts.sqlite')]
        self.assertEqual(exported[4], b'"a b"@example.org\n')
        self.assertEqual(exported[3], sha(exported[4]))
        self.expect(out, {'id': 'raw-path-known', 'headers': {'From': '"a b"@example.org'}, 'sieve': 'INBOX'})

    def test_sqlite_membership_fingerprint_uses_logical_content(self):
        _, first = self.build(known=['UPPER@example.org', 'second@example.org'])
        book = self.root / 'whitelist.abook'
        book.write_text(book.read_text().replace('Fixture 0', 'Edited').replace('UPPER@', 'upper@'))
        _, second = self.build()
        self.assertEqual(first, second)
        roles = {row[0]: row[4] for tag, row in second if tag == b'D'}
        self.assertEqual(roles[b'contacts.allow'], b'second@example.org\nupper@example.org\n')
        self.assertEqual(roles[b'config.contact-membership'], b'sqlite-sorted-mailbox-keys-v1')
        self.assertNotIn(b'input.exporter', roles)


if __name__ == '__main__':
    program = unittest.main(verbosity=2, exit=False)
    result = program.result
    summary = dict(COUNTS, executed=result.testsRun, failed=len(result.failures)+len(result.errors),
                   skipped=len(result.skipped), total=result.testsRun,
                   observedAt=datetime.now(timezone.utc).isoformat())
    if os.environ.get('JARO_FILTER_COMPILER_WORK'):
        (Path(os.environ['JARO_FILTER_COMPILER_WORK']) / 'summary.json').write_text(json.dumps(summary, sort_keys=True) + '\n')
    print('Compiler checks: %d executed, %d failed, %d skipped; %d actual generated Sieve decisions' % (
          summary['executed'], summary['failed'], summary['skipped'], summary['oracle_decisions']))
    sys.exit(0 if result.wasSuccessful() else 1)
