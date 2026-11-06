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
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-native-', dir=os.environ.get('TMPDIR'))
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

    def classify(self, rules, messages, known=(), blacklist=(), own=(), error=None):
        global CLASSIFY_LOADS, DECISIONS
        file = self.work / 'parsed-messages.bin'
        numbers = {'From': 1, 'Sender': 2, 'To': 4, 'Cc': 8, 'SpamFlag': 16, 'SenderHeader': 32}
        data = bytearray()
        for message in messages:
            for key, values in message.items():
                for value in values:
                    value = value.encode() if isinstance(value, str) else value
                    data += str(numbers[key]).encode() + b'\0' + value + b'\0'
            data += b'\0'
        file.write_bytes(data)
        command = [str(self.driver), '--classify', str(file)]
        sources = [('filters', 'Filters.txt', '/mail/Filters.txt', '\n'.join(rules)),
                   ('known', 'whitelist.abook', '/mail/whitelist.abook', '\n'.join(known)),
                   ('blacklist', 'blacklist.abook', '/mail/blacklist.abook', '\n'.join(blacklist)),
                   ('aliases', 'Aliases.txt', '/mail/Aliases.txt', '\n'.join(own))]
        for i, (role, identity, path, source) in enumerate(sources):
            input_file = self.work / ('classification-policy-%d' % i)
            input_file.write_text(source)
            command += [role, identity, path, str(input_file)]
        before = file.read_bytes()
        result = subprocess.run(command, capture_output=True)
        CLASSIFY_LOADS += 1
        self.assertEqual(file.read_bytes(), before)
        if error:
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertTrue(result.stderr.startswith(error.encode() + b' '), result.stderr)
            return
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        values = result.stdout.split(b'\0')
        self.assertEqual(values.pop(), b'')
        fields = FIELDS + ['index_comparisons', 'regex_checks']
        self.assertEqual(len(values) % len(fields), 0)
        rows = [dict(zip(fields, (v.decode() for v in values[i:i+len(fields)])))
                for i in range(0, len(values), len(fields))]
        self.assertEqual(len(rows), len(messages))
        DECISIONS += len(rows)
        return rows

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
        elif selector in ('known', 'blacklist', 'own'):
            predicate = ascii_fold(value)
            book, entries, stage, fields = {
                'known': ('whitelist.abook', known, 5, 1),
                'blacklist': ('blacklist.abook', blacklist, 1, 1),
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

    def header(self, rules, raw, known=(), blacklist=(), own=(), error=None, memory=False, inspect=False):
        global HEADER_LOADS, HEADER_DECISIONS
        file = self.work / 'raw-message.eml'
        raw = raw.encode() if isinstance(raw, str) else raw
        file.write_bytes(raw)
        mode = '--inspect-headers' if inspect else '--headers-buffer' if memory else '--headers'
        command = [str(self.driver), mode, str(file)]
        for i, (role, identity, source) in enumerate([
                ('filters', 'Filters.txt', '\n'.join(rules)), ('known', 'whitelist.abook', '\n'.join(known)),
                ('blacklist', 'blacklist.abook', '\n'.join(blacklist)), ('aliases', 'Aliases.txt', '\n'.join(own))]):
            source_file = self.work / ('header-policy-%d' % i)
            source_file.write_text(source)
            command += [role, identity, '/mail/' + identity, str(source_file)]
        result = subprocess.run(command, capture_output=True)
        HEADER_LOADS += 1
        self.assertEqual(file.read_bytes(), raw)
        if error:
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(result.stdout, b'')
            self.assertTrue(result.stderr.startswith(error.encode() + b' '), result.stderr)
            return result.stderr.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b'')
        values = result.stdout.split(b'\0')
        self.assertEqual(values.pop(), b'')
        if inspect:
            self.assertEqual(len(values) % 2, 0)
            return [(int(values[i]), values[i+1].decode()) for i in range(0, len(values), 2)]
        self.assertEqual(len(values), len(FIELDS) + 2)
        HEADER_DECISIONS += 1
        return dict(zip(FIELDS + ['index_comparisons', 'regex_checks'], (v.decode() for v in values)))

    def assert_decision(self, actual, expected):
        self.assertEqual({k: actual[k] for k in FIELDS}, expected)

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
        known = [r for r in rows if r['selector'] == 'known']
        self.assertEqual([r['predicate'] for r in known], ['"a b"@example.org', '#hash@example.org'])
        for row in known:
            self.assertEqual(row['id'], 'known:' + hashlib.sha256(row['predicate'].encode()).hexdigest())

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

    def test_all_52_independent_winners_and_decision_fields(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        casing = json.loads((FIXTURES / 'casing.json').read_text())
        parsed = json.loads((FIXTURES / 'parsed-inputs.json').read_text())['groups']
        groups = [('target', target['cases'], target['rules'], target),
                  ('domain', target['domain_cases'], target['domain_rules'], {}),
                  ('casing', casing['cases'], casing['rules'], casing)]
        executed = 0
        for name, cases, rules, contacts in groups:
            self.assertEqual(set(parsed[name]), {c['id'] for c in cases})
            books = {key: contacts.get(key, []) for key in ['known', 'blacklist', 'own']}
            rows = self.classify(rules, [parsed[name][c['id']] for c in cases], **books)
            for case, row in zip(cases, rows):
                with self.subTest(case=case['id']):
                    self.assert_decision(row, self.expected_decision(case, rules, **books))
                executed += 1
        cases = target['isolated_domain_cases']
        self.assertEqual(set(parsed['isolated']), {c['id'] for c in cases})
        for case in cases:
            row = self.classify(case['rules'], [parsed['isolated'][case['id']]])[0]
            self.assert_decision(row, self.expected_decision(case, case['rules']))
            executed += 1
        self.assertEqual(executed, 52)
        print('Independent target classification identities executed: 52/52')

    def test_source_order_wins_over_address_and_index_order(self):
        rules = ['from-address z@example.org move first', 'from-address a@example.org move second',
                 'from-domain example.org move third', 'to-address team@example.org move fourth']
        case = {'rule': 'filters:1', 'local': 'first', 'sieve': 'first'}
        messages = [{'From': ['a@example.org', 'z@example.org'], 'To': ['team@example.org']},
                    {'Sender': ['z@example.org'], 'From': ['a@example.org']},
                    {'From': ['z@example.org', 'a@example.org']}]
        for row in self.classify(rules, messages):
            self.assert_decision(row, self.expected_decision(case, rules))
        # Last duplicate's later source position loses to the intervening rule.
        rules = ['from-address x@example.org move old', 'from-domain example.org move first',
                 'from-address X@example.org move new']
        row = self.classify(rules, [{'From': ['x@example.org']}])[0]
        self.assert_decision(row, self.expected_decision(
            {'rule': 'filters:2', 'local': 'first', 'sieve': 'first'}, rules))

    def test_distinct_wildcard_suffixes_use_rule_order(self):
        rules = ['from-domain *.example.org move broad', 'from-domain *.deep.example.org move narrow']
        row = self.classify(rules, [{'From': ['x@a.deep.example.org']}])[0]
        self.assert_decision(row, self.expected_decision(
            {'rule': 'filters:1', 'local': 'broad', 'sieve': 'broad'}, rules))
        for domain in ['example.org', 'badexample.org', 'example.org.attacker.net']:
            row = self.classify(rules, [{'From': ['x@' + domain]}])[0]
            self.assertEqual(row['id'], 'builtin:unsorted')

    def test_known_contacts_before_domains_and_duplicate_provenance(self):
        rules = ['known-domain example.org']
        row = self.classify(rules, [{'From': ['x@example.org', 'y@example.org']}],
                            known=['Y@example.org', 'y@EXAMPLE.ORG'])[0]
        self.assert_decision(row, self.expected_decision(
            {'rule': 'known:y@example.org', 'local': 'known', 'sieve': 'INBOX'}, rules,
            known=['Y@example.org', 'y@EXAMPLE.ORG']))

    def test_legacy_ere_is_case_insensitive_and_not_a_glob(self):
        rules = [r'from ^LIST\+news@example[.]org$ move list',
                 'from github.com move regexdot', 'to ^team[0-9]+@example[.]org$ move team']
        messages = [{'From': ['list+NEWS@example.org']}, {'From': ['x@githubXcom']},
                    {'To': ['TEAM42@example.org']}, {'From': ['listnews@example.org']},
                    {'Sender': ['LIST+NEWS@example.org']}, {'From': ['x@github-com']}]
        rows = self.classify(rules, messages)
        self.assertEqual([r['logical'] for r in rows], ['list', 'regexdot', 'team', 'unsorted', 'list', 'regexdot'])
        self.assertEqual(rows[0]['id'], explicit('from', r'^LIST\+news@example[.]org$', 'list'))
        self.assertTrue(all(int(r['regex_checks']) > 0 for r in rows))

    def test_empty_policy_and_raw_builtin_fields(self):
        rows = self.classify([], [{}, {'SenderHeader': ['display MAILMAN-BOUNCE text']},
                                  {'From': ['mailman-bounce@example.org']},
                                  {'SpamFlag': ['NO', 'yEs']}, {'SpamFlag': ['YES extra']},
                                  {'SpamFlag': [' YES']}])
        self.assertEqual([r['id'] for r in rows], ['builtin:unsorted', 'builtin:bounce',
                         'builtin:unsorted', 'builtin:spam', 'builtin:unsorted', 'builtin:spam'])
        self.assertEqual((rows[3]['logical'], rows[3]['local'], rows[3]['sieve']), ('spam', 'zz.spam', 'Spam'))

    def test_spam_whitespace_matches_independent_pinned_oracle_cases(self):
        cases = json.loads((REPO / 'extras/test/fixtures/sieve-oracle/cases.json').read_text())['cases']
        cases = [case for case in cases if case['script'] == 'spam.sieve']
        self.assertEqual(len(cases), 10)
        # Explicit parsed field-value adapter; no RFC822 header text is parsed.
        rows = self.classify([], [{'SpamFlag': [value for name, value in case['headers']]}
                                  for case in cases])
        for case, row in zip(cases, rows):
            expected = {'rule': 'builtin:spam', 'local': 'zz.spam', 'sieve': 'Spam'} if case['mailbox'] == 'Spam' else {
                'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'}
            with self.subTest(case=case['id']):
                self.assert_decision(row, self.expected_decision(expected, []))

    def test_malformed_parsed_messages_cannot_win_early(self):
        for value in ['x@exämple.org', 'x@example..org', 'x@example.org.', 'x@-bad.example',
                      'Display <x@example.org>', '', 'x@*.example.org', b'\xff@example.org']:
            with self.subTest(value=value):
                self.classify([], [{'From': ['blocked@example.org'], 'To': [value]}],
                              blacklist=['blocked@example.org'], error='E_MESSAGE_ADDRESS')
        self.classify([], [{'SenderHeader': ['bad\rvalue']}], error='E_MESSAGE_TEXT')
        self.classify([], [{'From': ['x@example.org'], 'SpamFlag': ['Y' * 65537]}], error='E_MESSAGE_LIMIT')
        self.classify([], [{'SpamFlag': ['a' * 65536] * 17}], error='E_MESSAGE_LIMIT')

    def test_quoted_localparts_and_ascii_only_folding(self):
        rules = ['from-address "\\\"A B\\\"@Example.Org" move quoted',
                 'from-address Üser@example.org move unicode', 'from-address *@example.org move literal']
        messages = [{'From': ['"a b"@EXAMPLE.ORG']}, {'From': ['Üser@example.org']},
                    {'From': ['üser@example.org']}, {'From': ['x@example.org']}, {'From': ['*@example.org']}]
        rows = self.classify(rules, messages)
        self.assertEqual([r['logical'] for r in rows], ['quoted', 'unicode', 'unsorted', 'unsorted', 'literal'])
        self.assertEqual(rows[0]['id'], explicit('from-address', '"a b"@example.org', 'quoted'))

    def test_indexed_thousand_rule_contact_message_workload(self):
        rules = ['from-address user%d@route.example move route-%d' % (i, i) for i in range(1000)]
        rules += ['known-domain *.domain%d.example' % i for i in range(1000)]
        contacts = ['known%d@known.example' % i for i in range(1000)]
        messages = []
        for i in range(1000):
            messages.append({'From': [(['user%d@route.example' % i,
                                       'known%d@known.example' % i,
                                       'x@deep.domain%d.example' % i,
                                       'x@miss.example'][i % 4])]})
        rows = self.classify(rules, messages, known=contacts)
        for i, row in enumerate(rows):
            expected = 'route-%d' % i if i % 4 == 0 else 'unsorted' if i % 4 == 3 else 'known'
            self.assertEqual(row['logical'], expected)
            self.assertEqual(row['regex_checks'], '0')
            self.assertLess(int(row['index_comparisons']), 100)
        print('Indexed workload: 3000 predicates/1000 messages; no regex checks; <100 comparisons/decision')

    def test_all_52_raw_header_target_decisions_lf_and_crlf(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        casing = json.loads((FIXTURES / 'casing.json').read_text())
        executions = 0
        groups = [(target['cases'], target['rules'], target),
                  (target['domain_cases'], target['domain_rules'], {}), (casing['cases'], casing['rules'], casing)]
        groups += [([case], case['rules'], {}) for case in target['isolated_domain_cases']]
        for cases, rules, contacts in groups:
            books = {key: contacts.get(key, []) for key in ['known', 'blacklist', 'own']}
            for case in cases:
                for eol, memory in [('\n', False), ('\r\n', True)]:
                    raw = eol.join('%s: %s' % item for item in case['headers'].items())
                    if raw:
                        raw += eol
                    raw += 'Subject: raw target ' + case['id'] + eol + eol
                    raw = raw.encode() + b'Body From: blocked@example.org\x00\xff\r\nTo: team@example.org\n'
                    with self.subTest(case=case['id'], eol=repr(eol)):
                        row = self.header(rules, raw, memory=memory, **books)
                        self.assert_decision(row, self.expected_decision(case, rules, **books))
                    executions += 1
        self.assertEqual(executions, 104)
        print('Raw header target identities: 52/52 each LF/descriptor and CRLF/memory; 104 decisions')

    def test_raw_spam_whitespace_oracle_cases(self):
        cases = json.loads((REPO / 'extras/test/fixtures/sieve-oracle/cases.json').read_text())['cases']
        for case in [c for c in cases if c['script'] == 'spam.sieve']:
            raw = ''.join('%s: %s\r\n' % tuple(h) for h in case['headers']) + '\r\nBody\0binary'
            row = self.header([], raw)
            expected = {'rule': 'builtin:spam', 'local': 'zz.spam', 'sieve': 'Spam'} if case['mailbox'] == 'Spam' else {
                'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'}
            self.assert_decision(row, self.expected_decision(expected, []))

    def test_header_unfolding_lists_groups_comments_and_encoded_names(self):
        raw = ('From: =?UTF-8?Q?Example_=C3=9Cser?= <first@example.org>,\r\n'
               '\t"Other, Person" <second@example.org> (nested (comment))\r\n'
               'Sender: "Sender" <list@example.org> (mailman-bounce)\r\n'
               'To: Friends: "Team" <team@example.org>, other@example.org;\r\n'
               'Cc: Undisclosed recipients:;\r\n'
               'Cc: (comment) third@example.org\r\n'
               'X-Spam-Flag: \tYeS\r\n\t \r\n'
               'Subject: Irrelevant encoded display data\r\n\r\nBody')
        rows = self.header([], raw, inspect=True)
        self.assertEqual(rows[:-1], [(1, 'first@example.org'), (1, 'second@example.org'),
                         (2, 'list@example.org'), (4, 'team@example.org'), (4, 'other@example.org'),
                         (8, 'third@example.org'), (32, ' "Sender" <list@example.org> (mailman-bounce)'),
                         (16, ' \tYeS\t ')])
        self.assertEqual(rows[-1], (64, str(raw.index('\r\n\r\n') + 4)))
        self.assertEqual(self.header([], raw)['id'], 'builtin:bounce')

    def test_quoted_local_parts_raw_headers_preserve_quotes_and_escapes(self):
        for mailbox in ['"a b"@example.org', '"a\\"b"@example.org', '"a\\\\b"@example.org']:
            rules = ['from-address %s move quoted' % json.dumps(mailbox)]
            raw = 'From: "A Display Name" <%s>\n\n' % mailbox
            row = self.header(rules, raw)
            self.assertEqual(row['id'], explicit('from-address', mailbox, 'quoted'))
            inspected = self.header([], raw, inspect=True)
            self.assertEqual(inspected[0], (1, mailbox))

    def test_minimal_mailbox_quoting_policy_headers_and_direct_api(self):
        fixture = json.loads((REPO / 'extras/test/fixtures/filter-contract/mailbox-normalization.json').read_text())
        for group in fixture['groups']:
            canonical = ascii_fold(group['canonical'])
            for spelling in group['forms']:
                rules = ['from-address %s move normalized' % json.dumps(spelling)]
                for message in group['forms']:
                    row = self.classify(rules, [{'From': [message]}])[0]
                    self.assertEqual(row['id'], explicit('from-address', canonical, 'normalized'))
                    inspected = self.header([], 'From: '+message+'\n\n', inspect=True)
                    self.assertEqual(inspected[0], (1, group['canonical']))
                    self.assertEqual(self.header(rules, 'From: '+message+'\n\n')['id'], row['id'])

    def test_empty_quoted_local_is_explicitly_unsupported(self):
        self.rules('from-address '+json.dumps('""@example.org')+' move empty', error='E_ADDRESS')
        self.classify([], [{'From': ['""@example.org']}], error='E_MESSAGE_ADDRESS')
        self.header([], 'From: ""@example.org\n\n', error='E_MESSAGE_ADDRESS')

    def test_equivalent_spelling_duplicates_keep_last_source_position(self):
        rules = ['from-address '+json.dumps('"us\\er"@example.org')+' move Old',
                 'from-address other@example.org move Other',
                 'from-address user@example.org move Last']
        rows = [r for r in self.rules('\n'.join(rules)) if r['stage'] == '3']
        self.assertEqual([r['logical'] for r in rows], ['Other', 'Last'])
        winner = self.classify(rules, [{'From': ['"user"@example.org']}])[0]
        self.assertEqual(winner['id'], explicit('from-address', 'user@example.org', 'Last'))
        self.assertEqual(winner['line'], '3')

    def test_repeated_missing_and_empty_group_fields(self):
        raw = ('From: other@elsewhere.org\nFrom: blocked@example.org\n'
               'Sender: first@example.org\nSender: second@example.org\n'
               'To: other@example.org\nTo: own@example.org\n'
               'Cc: undisclosed:;\nX-Spam-Flag: NO\nX-Spam-Flag: \tYES\n\n')
        rows = self.header([], raw, inspect=True)
        self.assertEqual([n for n, _ in rows].count(1), 2)
        self.assertEqual([n for n, _ in rows].count(2), 2)
        self.assertEqual([n for n, _ in rows].count(32), 2)
        self.assertEqual([n for n, _ in rows].count(16), 2)
        row = self.header([], raw, blacklist=['blocked@example.org'], own=['own@example.org'])
        self.assertEqual(row['id'], 'blacklist:' + hashlib.sha256(b'blocked@example.org').hexdigest())
        for raw in ['Subject: only\n\n', 'To: undisclosed recipients:;\n\n', '\nBinary body']:
            self.assertEqual(self.header([], raw)['id'], 'builtin:unsorted')

    def test_header_errors_are_all_or_nothing(self):
        cases = [(b'', 'E_MESSAGE_TRUNCATED'), (b'From: a@example.org', 'E_MESSAGE_TRUNCATED'),
                 (b'From: a@example.org\n', 'E_MESSAGE_TRUNCATED'),
                 (b' continuation\n\n', 'E_MESSAGE_HEADER'), (b'From a@example.org\n\n', 'E_MESSAGE_HEADER'),
                 (b'Bad Name: x\n\n', 'E_MESSAGE_HEADER'), (b': value\n\n', 'E_MESSAGE_HEADER'),
                 (b'From: x@example.org\0bad\n\n', 'E_MESSAGE_TEXT'),
                 (b'Subject: x\ry\n\n', 'E_MESSAGE_TEXT'), (b'Subject: x\x01y\n\n', 'E_MESSAGE_TEXT')]
        invalid_addresses = ['"unclosed@example.org', 'a@example.org (unclosed',
                             'a@example.org, broken <', 'a@example.org,', ',a@example.org',
                             'a@example.org,,b@example.org', 'group: a@example.org',
                             'outer: inner: a@example.org;;', 'a@example.org;', '(comment);',
                             'a@example.org b@example.org', 'Name <a@example.org> junk',
                             '<@route.example:a@example.org>', 'a@[127.0.0.1]',
                             'a@exämple.org', 'a@-bad.example', 'a@example..org', '', 'a@example.org >']
        cases += [('From: blocked@example.org\nTo: ' + address + '\n\n', 'E_MESSAGE_ADDRESS')
                  for address in invalid_addresses]
        for raw, code in cases:
            for memory in [False, True]:
                with self.subTest(raw=raw, memory=memory):
                    self.header([], raw, blacklist=['blocked@example.org'], memory=memory, error=code)

    def test_physical_unfolded_and_total_header_bounds_without_body_limits(self):
        self.assertEqual(self.header([], b'Subject:' + b'a' * (65536-8) + b'\r\n\r\n')['id'], 'builtin:unsorted')
        self.header([], b'Subject:' + b'a' * (65537-8) + b'\n\n', error='E_MESSAGE_LIMIT')
        self.header([], b'Subject:' + b'a' * 40000 + b'\n ' + b'b' * 30000 + b'\n\n', error='E_MESSAGE_LIMIT')
        line = b'X: ' + b'a' * 1020 + b'\n'
        self.header([], line * 1025 + b'\n', error='E_MESSAGE_LIMIT')
        near_limit = line * 1024
        self.assertEqual(len(near_limit), 1048576)
        self.header([], near_limit + b'\n', error='E_MESSAGE_LIMIT')
        exact_limit = line * 1023 + b'Y:' + b'a' * 1020 + b'\n\n'
        self.assertEqual(len(exact_limit), 1048576)
        self.assertEqual(self.header([], exact_limit)['id'], 'builtin:unsorted')
        self.assertEqual(self.header([], b'X:' + b'a' * 32768 + b'\n ' + b'b' * 32767 + b'\n\n')['id'], 'builtin:unsorted')
        row = self.header([], line * 1023 + b'\n' + b'\0\xffFrom: bad@example.org\n' * 100000)
        self.assertEqual(row['id'], 'builtin:unsorted')
        local = 'a' * 1000
        row = self.header(['from-address %s@example.org move long' % local],
                          'From: %s@example.org\n\n' % local)
        self.assertEqual(row['logical'], 'long')

    def test_sanitized_real_world_style_messages(self):
        cases = [('folded-list.eml', [], 'builtin:bounce', 'zz.bounces'),
                 ('groups.eml', ['known-domain *.example.org'],
                  explicit('known-domain', '*.example.org', 'known'), 'known'),
                 ('display-misdirection.eml', ['to-address team@example.org move team', 'known-domain example.org'],
                  'builtin:unsorted', 'unsorted')]
        for name, rules, id_, logical in cases:
            with self.subTest(message=name):
                row = self.header(rules, (FIXTURES / 'messages' / name).read_bytes(),
                                  known=['known@example.org'], blacklist=['blocked@example.org'])
                self.assertEqual((row['id'], row['logical']), (id_, logical))
        mailbox = 'Üser@XN--BCHER-KVA.EXAMPLE'
        row = self.header(['from-address Üser@xn--bcher-kva.example move utf8'],
                          'From: "UTF8 Person" <%s>\r\n\r\n' % mailbox)
        self.assertEqual(row['id'], explicit('from-address', 'Üser@xn--bcher-kva.example', 'utf8'))

    def test_near_limit_raw_senders_are_not_counted_twice(self):
        mailbox = 'a' * 50000 + '@example.org'
        raw = ('Sender: ' + mailbox + '\r\n') * 18 + '\r\n'
        self.assertLess(len(raw), 1048576)
        self.assertGreater(2 * len(mailbox) * 18, 1048576)
        self.assertEqual(self.header([], raw)['id'], 'builtin:unsorted')


if __name__ == '__main__':
    program = unittest.main(exit=False, verbosity=2)
    print('Native parser driver invocations: %d' % INVOCATIONS)
    print('Native classification policy loads: %d; decisions: %d; selftest invocations: %d' %
          (CLASSIFY_LOADS, DECISIONS, SELFTEST_INVOCATIONS))
    print('Native header reader loads: %d; classified raw messages: %d' % (HEADER_LOADS, HEADER_DECISIONS))
    raise SystemExit(not program.result.wasSuccessful())
