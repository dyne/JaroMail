"""Exercise actual shared C parser; expected identities never use backend output."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
FIELDS = 'id selector predicate action logical local sieve source identity line occurrence kind stage fields'.split()
INVOCATIONS = 0
CLASSIFY_LOADS = 0
DECISIONS = 0
SELFTEST_INVOCATIONS = 0
HEADER_LOADS = 0
HEADER_DECISIONS = 0
FIXTURES = REPO / 'extras/test/fixtures/filter-policy'


def ascii_fold(value):
    return value.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz'))


def explicit(selector, predicate, logical, identity='Filters.txt', occurrence=1):
    fields = [selector, predicate, 'move', logical, identity, str(occurrence)]
    return 'r:' + hashlib.sha256(b'\0'.join(s.encode() for s in fields)).hexdigest()


class PolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get('JARO_FILTER_NATIVE_WORK')
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            if not cls.work.is_absolute():
                raise ValueError('JARO_FILTER_NATIVE_WORK must be an absolute new directory')
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-native-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.driver = cls.work / 'objects/filter-policy-driver'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-policy-driver',
                        'FILTER_BUILD_DIR=' + str(cls.driver.parent), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)

    @classmethod
    def tearDownClass(cls):
        if cls.temp:
            cls.temp.cleanup()

    def parse(self, *sources, error=None):
        global INVOCATIONS
        command = [str(self.driver)]
        for i, (role, identity, path, data) in enumerate(sources):
            file = self.work / ('input-%d' % i)
            file.write_bytes(data.encode() if isinstance(data, str) else data)
            command += [role, identity, path, str(file)]
        result = subprocess.run(command, capture_output=True)
        INVOCATIONS += 1
        if error:
            self.assertEqual(result.returncode, 4, result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertTrue(result.stderr.startswith(error.encode() + b' '), result.stderr)
            return result.stderr.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        values = result.stdout.split(b'\0')
        self.assertEqual(values.pop(), b'')
        self.assertEqual(len(values) % len(FIELDS), 0)
        return [dict(zip(FIELDS, (v.decode() for v in values[i:i+len(FIELDS)])))
                for i in range(0, len(values), len(FIELDS))]

    def rules(self, data, **kw):
        return self.parse(('filters', 'Filters.txt', '/mail/Filters.txt', data), **kw)

    def expected_decision(self, case, rules, known=(), blacklist=(), own=()):
        """Resolve authored winner aliases; never consult native records."""
        alias = case['rule']
        selector, value = alias.split(':', 1)
        line, occurrence = 0, 1
        source, identity = '', ''
        if selector == 'builtin':
            selector = value
            predicate, stage, fields, kind = {
                'bounce': ('mailman-bounce', 2, 2, 4),
                'spam': ('YES', 6, 16, 5), 'unsorted': ('', 8, 0, 6)}[selector]
            id_ = alias
        elif selector in ('allow', 'block', 'own'):
            predicate = ascii_fold(value)
            book, entries, stage, fields = {
                'allow': ('whitelist.abook', known, 5, 1),
                'block': ('blacklist.abook', blacklist, 1, 1),
                'own': ('Aliases.txt', own, 7, 12)}[selector]
            kind = 0
            line = [ascii_fold(v) for v in entries].index(predicate) + 1
            identity, source = book, '/mail/' + book
            id_ = selector + ':' + hashlib.sha256(predicate.encode()).hexdigest()
        else:
            self.assertIn(selector, ('filters', 'domain'))
            line = int(value)
            tokens = rules[line-1].split()
            selector, predicate = tokens[:2]
            if selector not in ('from', 'to'):
                predicate = ascii_fold(predicate)
            stage = 5 if selector == 'known-domain' else 3 if selector.startswith('from') else 4
            fields = 1 if selector == 'known-domain' else 3 if selector.startswith('from') else 12
            kind = 3 if selector in ('from', 'to') else 0 if selector.endswith('address') else 2 if predicate.startswith('*.') else 1
            identity, source = 'Filters.txt', '/mail/Filters.txt'
            logical = case['local']
            for previous in rules[:line-1]:
                prior = previous.split()
                prior_predicate = prior[1] if prior[0] in ('from', 'to') else ascii_fold(prior[1])
                prior_logical = prior[3] if len(prior) == 4 else 'known'
                if (prior[0], prior_predicate, prior_logical) == (selector, predicate, logical):
                    occurrence += 1
            id_ = explicit(selector, predicate, logical, occurrence=occurrence)
        logical = 'spam' if selector == 'spam' else case['local']
        return dict(zip(FIELDS, map(str, [id_, selector, predicate, 'move', logical,
                    case['local'], case['sieve'], source, identity, line, occurrence, kind, stage, fields])))

    def test_native_allocation_and_hash_selftest(self):
        global SELFTEST_INVOCATIONS
        SELFTEST_INVOCATIONS += 1
        subprocess.run([self.driver, '--selftest'], check=True, timeout=15)

    def test_comments_crlf_normalization_and_provenance(self):
        rows = self.rules('# comment\r\n\r\nfrom-address TEAM@EXAMPLE.ORG save "Project People" # comment\r\n')
        row = rows[1]
        self.assertEqual((row['selector'], row['predicate'], row['action'], row['logical'], row['line']),
                         ('from-address', 'team@example.org', 'move', 'Project People', '3'))
        self.assertEqual(row['source'], '/mail/Filters.txt')
        self.assertEqual(row['id'], explicit('from-address', 'team@example.org', 'Project People'))
        self.assertEqual([r['stage'] for r in rows], ['2', '3', '6', '8'])

    def test_json_quotes_unicode_and_literal_shell_data(self):
        text = 'from "^list\\\\+news@example[.]org$" move "a;$(touch sentinel)`id`/b"\n'
        row = self.rules(text)[1]
        self.assertEqual(row['predicate'], '^list\\+news@example[.]org$')
        self.assertEqual(row['logical'], 'a;$(touch sentinel)`id`/b')
        self.assertFalse((REPO / 'sentinel').exists())
        row = self.rules('to-address x@example.org move "Caf\\u00e9 \\ud83d\\ude00"')[1]
        self.assertEqual(row['logical'], 'Café 😀')
        self.assertEqual(row['id'], explicit('to-address', 'x@example.org', 'Café 😀'))
        self.assertEqual(self.rules('from a#b move semi;colon')[1]['predicate'], 'a#b')

    def test_last_duplicate_and_occurrence_ids(self):
        rows = self.rules('from-address X@example.org move old\nfrom-address x@EXAMPLE.ORG save new\n'
                          'from-address x@example.org move new\nto overlap move first\nto overlap@example.org move second\n')
        explicit_rows = [r for r in rows if r['id'].startswith('r:')]
        self.assertEqual([r['logical'] for r in explicit_rows], ['new', 'first', 'second'])
        row = explicit_rows[0]
        self.assertEqual((row['line'], row['occurrence']), ('3', '2'))
        self.assertEqual(row['id'], explicit('from-address', 'x@example.org', 'new', occurrence=2))

    def test_stable_blank_lines_root_relocation_and_determinism(self):
        text = 'from ^x@example[.]org$ save first\nto-address team@example.org move last\n'
        first = self.rules(text)
        second = self.parse(('filters', 'Filters.txt', '/elsewhere/Filters.txt', '\n# comment\n' + text))
        self.assertEqual([r['id'] for r in first], [r['id'] for r in second])
        self.assertEqual(first, self.rules(text))
        extra = self.rules('known-domain example.org\n' + text)
        self.assertTrue(set(r['id'] for r in first).issubset(r['id'] for r in extra))

    def test_snapshots_accounts_aliases_contacts_order(self):
        sources = [
            ('aliases', 'Aliases.txt', '/mail/Aliases.txt', 'OWN@example.org\nAlias@example.org\n'),
            ('account', 'Accounts/z', '/mail/Accounts/z', 'name some spaces and "unclosed\nemail OWN@example.org\n'),
            ('account', 'Accounts/a', '/mail/Accounts/a', '# account\nemail Own@Example.Org\n'),
            ('known', 'whitelist.abook', '/mail/whitelist.abook', 'Known@example.org\nKNOWN@example.org\n'),
            ('blacklist', 'blacklist.abook', '/mail/blacklist.abook', 'Blocked@example.org\n'),
            ('filters', 'Filters.txt', '/mail/Filters.txt', 'known-domain example.org\n')]
        rows = self.parse(*sources)
        self.assertEqual([int(r['stage']) for r in rows], [1, 2, 5, 5, 6, 7, 7, 8])
        own = next(r for r in rows if r['predicate'] == 'own@example.org')
        self.assertEqual((own['source'], own['line']), ('/mail/Accounts/a', '2'))
        self.assertEqual(own['id'], 'own:' + hashlib.sha256(b'own@example.org').hexdigest())
        self.assertEqual(rows[2]['predicate'], 'known@example.org')
        self.assertEqual(rows[2]['sieve'], 'INBOX')
        self.assertEqual(rows[3]['selector'], 'known-domain')
        # Account input enumeration cannot alter canonical provenance/order.
        self.assertEqual(rows, self.parse(*reversed(sources)))

    def test_domain_normalization_and_invalid_fixture(self):
        rows = self.rules('known-domain *.EXAMPLE.ORG\nfrom-domain XN--BCHER-KVA.example move vendors\n')
        self.assertEqual(rows[1]['predicate'], 'xn--bcher-kva.example')
        self.assertEqual(rows[2]['predicate'], '*.example.org')
        invalid = json.loads((REPO / 'extras/test/fixtures/filter-policy/target.json').read_text())['invalid_domains']
        for value in invalid + ['a' * 64 + '.org', '.org', 'a.-b', 'a.b-', 'a_foo.org', '*', 'a/b.org']:
            with self.subTest(value=value):
                self.rules('known-domain ' + value, error='E_DOMAIN')

    def test_typed_address_validation(self):
        for address in ['a', '@example.org', 'x@', '.x@example.org', 'x..y@example.org',
                        'x.@example.org', 'x@exämple.org', 'x@-example.org',
                        'x@example.org,y@example.org', '*@*.example.org']:
            with self.subTest(address=address):
                self.rules('to-address ' + address + ' move target', error='E_ADDRESS')
        self.assertEqual(self.rules('from-address Üser@EXAMPLE.ORG move target')[1]['predicate'], 'Üser@example.org')
        self.assertEqual(self.rules('from-address "\\\"A B\\\"@example.org" move target')[1]['predicate'], '"a b"@example.org')

    def test_malformed_rules_have_no_partial_policy(self):
        cases = [('from-address', 'E_FIELDS'), ('subject x move dst', 'E_SELECTOR'),
                 ('to x delete dst', 'E_ACTION'), ('to x MOVE dst', 'E_ACTION'),
                 ('from [ move dst', 'E_REGEX'), ('to "" move dst', 'E_PREDICATE'),
                 ('to x move ""', 'E_DESTINATION'), ('to x move /absolute', 'E_DESTINATION'),
                 ('to x move a/../b', 'E_DESTINATION'), ('to x move a//b', 'E_DESTINATION'),
                 ('to x move a/./b', 'E_DESTINATION'), ('to x move .', 'E_DESTINATION'),
                 ('to x move a/', 'E_DESTINATION'), ('to x move "a\\nb"', 'E_DESTINATION'),
                 ('to x move "a\\u007fb"', 'E_DESTINATION'), ('to x move a extra', 'E_FIELDS'),
                 ('to "unclosed move a', 'E_QUOTE'), ('to "x"y move a', 'E_QUOTE'),
                 ('to "\\q" move a', 'E_QUOTE'), ('to "\\u0000" move a', 'E_NUL'),
                 ('to "\\ud800" move a', 'E_QUOTE'), ('to "\\udc00" move a', 'E_QUOTE'),
                 ('to "\\uZZZZ" move a', 'E_QUOTE')]
        for text, code in cases:
            with self.subTest(text=text):
                diagnostic = self.rules('known-domain good.example\n\n' + text, error=code)
                self.assertIn('source=0 line=3 ', diagnostic)

    def test_utf8_nul_raw_controls_and_size_bounds(self):
        for data in [b'\x00', b'\xff', b'\xc0\x80', b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xe2\x82']:
            self.rules(b'\n' + data, error='E_UTF8')
        self.rules(b'to x\x01 move a', error='E_TOKEN')
        self.rules(b'to "x\t" move a', error='E_QUOTE')
        self.rules('#' + 'a' * 65535)  # Exactly 64 KiB is supported.
        self.rules('#' + 'a' * 65536, error='E_LINE_LIMIT')
        # One valid destination with long decoded content must remain intact.
        dest = 'd' * 65000
        self.assertEqual(self.rules('to x move ' + dest)[1]['logical'], dest)

    def test_input_contact_errors_and_source_index(self):
        sources = [('account', 'Accounts/z', '/z', 'email good@example.org\n'),
                   ('aliases', 'Aliases.txt', '/aliases', 'not-a-mailbox\n')]
        self.assertIn('source=1 line=1 ', self.parse(*sources, error='E_ADDRESS'))
        self.parse(('known', 'whitelist.abook', '/known', 'a@example.org b@example.org'), error='E_ADDRESS')
        self.parse(('account', 'Accounts/a', '/a', 'email\n'), error='E_FIELDS')

    def test_raw_contact_exports_are_not_policy_tokens(self):
        rows = self.parse(('known', 'whitelist.abook', '/known',
                           '"A B"@EXAMPLE.ORG\n#HASH@EXAMPLE.ORG\n'))
        known = [r for r in rows if r['selector'] == 'allow']
        self.assertEqual([r['predicate'] for r in known], ['"a b"@example.org', '#hash@example.org'])
        for row in known:
            self.assertEqual(row['id'], 'allow:' + hashlib.sha256(row['predicate'].encode()).hexdigest())

    def test_rule_limits_count_effective_records(self):
        self.rules('known-domain example.org\n' * 100001)
        text = ''.join('to-address u%d@example.org move t\n' % i for i in range(100001))
        self.rules(text, error='E_RULE_LIMIT')

    def test_independent_fixture_rules_parse_with_documented_ids(self):
        for filename in ['target.json', 'casing.json']:
            fixture = json.loads((REPO / 'extras/test/fixtures/filter-policy' / filename).read_text())
            for key in ['rules', 'domain_rules']:
                if key not in fixture:
                    continue
                text = '\n'.join(fixture[key])
                rows = self.rules(text)
                # Independent expected source identities, not expected winners.
                for index, line in enumerate(fixture[key], 1):
                    selector, predicate, *tail = line.split()
                    logical = tail[-1] if tail else 'known'
                    if selector not in ['from', 'to']:
                        predicate = predicate.lower()
                    if any(later.split()[:2] == [selector, predicate] for later in fixture[key][index:]):
                        continue
                    found = next(r for r in rows if r['line'] == str(index))
                    self.assertEqual(found['id'], explicit(selector, predicate, logical))

if __name__ == '__main__':
    program = unittest.main(exit=False, verbosity=2)
    print('Native parser driver invocations: %d' % INVOCATIONS)
    raise SystemExit(not program.result.wasSuccessful())
