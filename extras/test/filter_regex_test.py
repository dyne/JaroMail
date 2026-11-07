"""Qualification of an explicit grammar against authored winners, never an evaluator."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import struct
import unittest
sys.dont_write_bytecode = True
from sieve_oracle import SieveOracle
from filter_compiler_test import records

REPO = Path(__file__).resolve().parents[2]
CATALOGUE = REPO / 'docs/filter-regex-qualification.json'
FIXTURE = REPO / 'extras/test/fixtures/filter-contract/regex-subset.json'
DIGEST = hashlib.sha256(CATALOGUE.read_bytes()).hexdigest()
PROFILE = ('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\n'
           'capability mailbox\ncapability regex\nregex-qualification ' + DIGEST + '\n')


class RegexTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = None
        retained = os.environ.get('JARO_FILTER_REGEX_WORK')
        if not retained and os.environ.get('JARO_FILTER_TEST_TMP'):
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-regex-',dir=os.environ['JARO_FILTER_TEST_TMP'])
        cls.work = Path(retained or (cls.temp.name if cls.temp else tempfile.mkdtemp(prefix='jaro-regex-')))
        cls.work.mkdir(mode=0o700, parents=True, exist_ok=True)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-compile', 'filter-policy-driver', 'filter-compiler-driver',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler = cls.objects / 'jaro-filter-compile'
        cls.native = cls.objects / 'filter-policy-driver'
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])
        cls.events, cls.decisions = [], 0

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2)+'\n')

    def setUp(self):
        self.root = self.work / self._testMethodName
        self.root.mkdir()
        self.rules = self.root / 'Filters.txt'
        self.profile = self.root / 'FilterProfile.txt'
        self.profile.write_text(PROFILE)
        self.sources = ['filters', 'Filters.txt', str(self.rules), str(self.rules)]

    def run_tool(self, args, code=0):
        result = subprocess.run(list(map(str, args)), capture_output=True, timeout=30)
        self.events.append({'command': list(map(str, args)), 'cwd': str(REPO), 'exit': result.returncode,
                            'stdout_hex': result.stdout.hex(), 'stderr': result.stderr.decode('ascii'),
                            'observedAt': datetime.now(timezone.utc).isoformat()})
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def compile(self, rules):
        self.rules.write_text('\n'.join(rules)+'\n')
        result = self.run_tool([self.compiler, '--root', self.root, '--report0'])
        path = Path(os.fsdecode(result.stdout.split(b'\0')[3]))
        self.oracle.compile(path / 'Filters.sieve', extensions='+regex')
        return path

    def decide(self, generation, case):
        message = self.root / 'message.eml'
        pairs = case['headers'].items() if isinstance(case['headers'], dict) else case['headers']
        message.write_text('\n'.join('%s: %s' % tuple(p) for p in pairs)+'\nSubject: Synthetic qualification\n\nBody unchanged.\n')
        actual = self.run_tool([self.native, '--headers', message, *self.sources]).stdout.split(b'\0')
        self.assertEqual(len(actual), 17)
        self.assertEqual(actual[5].decode(), case['local'], case['id'])
        if 'parsed' in case:
            parsed = self.root / 'parsed.bin'
            parsed.write_bytes(b''.join(str({'From': 1, 'Sender': 2, 'To': 4, 'Cc': 8}[field]).encode()+b'\0'+
                                       value.encode()+b'\0' for field, value in case['parsed'])+b'\0')
            direct = self.run_tool([self.native, '--classify', parsed, *self.sources]).stdout.split(b'\0')
            self.assertEqual(direct[0], actual[0], case['id'])
            self.assertEqual(direct[5], actual[5], case['id'])
        if 'rule' in case:
            expected = case['rule']
            if expected.startswith(('filters:', 'domain:')):
                line = expected.split(':')[1].encode()
                rows = [row for tag, row in records((generation / 'policy.bin').read_bytes())
                        if tag == b'R' and row[10] == line and row[9] == os.fsencode(self.rules)]
                self.assertEqual(len(rows), 1)
                expected = rows[0][4].decode()
            elif not expected.startswith('builtin:'):
                kind, mailbox = expected.split(':', 1)
                expected = kind + ':' + hashlib.sha256(mailbox.encode()).hexdigest()
            self.assertEqual(actual[0].decode(), expected, case['id'])
        self.oracle.expect(generation / 'Filters.sieve', message, case['sieve'], True, extensions='+regex')
        type(self).decisions += 1

    def books(self, corpus):
        for key, filename, role in [('known', 'whitelist.abook', 'known'),
                                     ('blacklist', 'blacklist.abook', 'blacklist')]:
            values = corpus.get(key, [])
            text = '[format]\nprogram=abook\nversion=0.6.0\n'
            for i, address in enumerate(values):
                text += '[%d]\nname=Fixture\nemail=%s\n' % (i, address)
            (self.root / filename).write_text(text if values else '')
            source = self.root / (role+'.txt')
            source.write_text('\n'.join(values))
            self.sources += [role, source.name, str(source), str(source)]
        aliases = self.root / 'Aliases.txt'
        aliases.write_text('\n'.join(json.dumps(v) for v in corpus.get('own', [])))
        self.sources += ['aliases', aliases.name, str(aliases), str(aliases)]

    def test_catalogue_is_embedded_and_compiler_bound(self):
        catalogue = json.loads(CATALOGUE.read_text())
        fixture = json.loads(FIXTURE.read_text())
        self.assertEqual(catalogue['fixture_sha256'], hashlib.sha256(FIXTURE.read_bytes()).hexdigest())
        self.assertEqual(catalogue['independent_cases'], len(fixture['cases']))
        normalization = REPO / 'extras/test/fixtures/filter-contract/mailbox-normalization.json'
        self.assertEqual(catalogue['normalization_fixture_sha256'], hashlib.sha256(normalization.read_bytes()).hexdigest())
        configuration = Path(os.environ['SIEVE_DEPS_PREFIX']) / 'include/dovecot/config.h'
        self.assertRegex(configuration.read_text(), r'(?m)^#define EXPERIMENTAL_MAIL_UTF8\b')
        self.assertEqual(catalogue['compiler_id'].encode()+b'\n', self.run_tool([self.compiler, '--version']).stdout)
        header = (REPO / 'src/filter/regex_qualification.h').read_text()
        self.assertIn('#define JF_REGEX_QUALIFICATION "'+DIGEST+'"', header)
        encoded = header.split('#define JF_REGEX_CATALOGUE ', 1)[1].strip()
        self.assertEqual(json.loads(encoded).encode(), CATALOGUE.read_bytes())

    def test_old_and_noncanonical_cache_metadata_is_incompatible(self):
        generation = self.compile(['from-address user@example.org move matched'])
        original = records((generation / 'policy.bin').read_bytes())
        for change in ('compiler', 'normalization', 'address'):
            changed = [(tag, list(fields)) for tag, fields in original]
            for tag, fields in changed:
                if change == 'compiler' and tag == b'M':
                    fields[0] = b'jaromail-filter-compiler-v1'
                if change == 'normalization' and tag == b'D' and fields[0] == b'config.address-normalization':
                    fields[4] = b'quoted-spelling-v1'
                    fields[3] = hashlib.sha256(fields[4]).digest()
                if change == 'address' and tag == b'R' and fields[1] == b'3':
                    fields[5] = b'"user"@example.org'
            def encode(tag, fields):
                return tag + struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(f))+f for f in fields)
            payload = struct.pack('<I', len(changed))+b''.join(encode(t, f) for t, f in changed)
            fingerprint = hashlib.sha256(b'JFFP1\0'+b''.join(encode(t, f) for t, f in changed if t == b'D')).digest()
            header = b'JFNCACHE'+struct.pack('<IIQ', 1, 1, len(payload))+fingerprint+hashlib.sha256(payload).digest()
            corrupt = self.root / 'incompatible.bin'
            corrupt.write_bytes(header+payload)
            self.run_tool([self.objects / 'filter-compiler-driver', '--validate', corrupt,
                           generation / 'filters.zsh', generation / 'Filters.sieve'], 5)

    def test_independent_subset_cases(self):
        previous, generation = None, None
        for case in json.loads(FIXTURE.read_text())['cases']:
            if case['pattern'] != previous:
                generation = self.compile(['from '+json.dumps(case['pattern'])+' move matched'])
                previous = case['pattern']
            self.decide(generation, {'id': case['id'], 'headers': {case['field']: case['address']},
                                     'parsed': [(case['field'], case['address'])],
                                     'local': case['expected'], 'sieve': case['expected']})

    def test_default_profile_qualification_and_partial_save_move(self):
        self.profile.unlink()
        # Run the independently authored ERE corpus through normal defaults.
        self.test_independent_subset_cases()
        for selector, matching in [('from', ('From', 'Sender')), ('to', ('To', 'Cc'))]:
            exports = []
            for action in ('save', 'move'):
                generation = self.compile([selector+' mute@metamute '+action+' matched'])
                exports.append((generation / 'Filters.sieve').read_bytes())
                for field in ('From', 'Sender', 'To', 'Cc'):
                    destination = 'matched' if field in matching else 'unsorted'
                    self.decide(generation, {'id': selector+'-'+action+'-'+field,
                                             'headers': {field: 'MUTE@METAMUTE.example.org'},
                                             'local': destination, 'sieve': destination})
                self.decide(generation, {'id': selector+'-'+action+'-miss',
                                         'headers': {matching[0]: 'other@metamute.org'},
                                         'local': 'unsorted', 'sieve': 'unsorted'})
            self.assertEqual(exports[0], exports[1])
        self.assertFalse(self.profile.exists())

    def test_equivalent_mailbox_spellings_all_sources(self):
        corpus = json.loads((REPO / 'extras/test/fixtures/filter-contract/mailbox-normalization.json').read_text())
        for index, group in enumerate(corpus['groups']):
            for policy in group['forms']:
                generation = self.compile(['from-address '+json.dumps(policy)+' move matched'])
                rows = [r for t, r in records((generation / 'policy.bin').read_bytes()) if t == b'R' and r[1] == b'3']
                canonical = ''.join(chr(ord(c)+32) if 'A' <= c <= 'Z' else c for c in group['canonical'])
                self.assertEqual(rows[0][5], canonical.encode())
                for form in group['forms']:
                    self.decide(generation, {'id': 'normalization-%d' % index, 'headers': {'From': form},
                                             'parsed': [('From', form)], 'local': 'matched', 'sieve': 'matched'})
        # Each contact/own source normalizes before duplicate collapse and IDs.
        self.books({'known': ['"us\\er"@example.org', 'user@example.org'],
                    'blacklist': ['"block\\ed"@example.org', 'blocked@example.org'],
                    'own': ['"ow\\n"@example.org', 'own@example.org']})
        generation = self.compile([])
        rows = [r for t, r in records((generation / 'policy.bin').read_bytes()) if t == b'R']
        self.assertEqual(len(rows), 6)
        for field, form, local, sieve in [('From', '"us\\er"@example.org', 'known', 'INBOX'),
                                         ('From', '"block\\ed"@example.org', 'zz.blacklist', 'zz.blacklist'),
                                         ('To', '"ow\\n"@example.org', 'priv', 'priv')]:
            self.decide(generation, {'id': local, 'headers': {field: form}, 'parsed': [(field, form)],
                                     'local': local, 'sieve': sieve})
        # ASCII casemap never folds a Unicode character, even with SMTPUTF8.
        generation = self.compile(['from-address Üser@example.org move matched'])
        for form, expected in [('ÜSER@example.org', 'matched'), ('üser@example.org', 'unsorted')]:
            self.decide(generation, {'id': 'utf8-case-'+form, 'headers': {'From': form},
                                     'parsed': [('From', form)], 'local': expected, 'sieve': expected})

    def test_original_target_winners_with_legacy_regex(self):
        corpus = json.loads((REPO / 'extras/test/fixtures/filter-policy/target.json').read_text())
        self.books(corpus)
        generation = self.compile(corpus['rules'])
        for case in corpus['cases']:
            self.decide(generation, case)

    def test_domain_and_casing_winners(self):
        target = json.loads((REPO / 'extras/test/fixtures/filter-policy/target.json').read_text())
        generation = self.compile(target['domain_rules'])
        for case in target['domain_cases']:
            self.decide(generation, case)
        for case in target['isolated_domain_cases']:
            self.decide(self.compile(case['rules']), case)
        corpus = json.loads((REPO / 'extras/test/fixtures/filter-policy/casing.json').read_text())
        self.books(corpus)
        generation = self.compile(corpus['rules'])
        for case in corpus['cases']:
            self.decide(generation, case)

    def test_shipped_defaults_unchanged(self):
        generation = self.compile((REPO / 'docs/Filters.txt').read_text().splitlines())
        for case in json.loads(FIXTURE.read_text())['cases']:
            if case['id'].startswith('default-') and case['expected'] == 'matched':
                self.decide(generation, {'id': case['id'], 'headers': {'From': case['address']},
                                         'local': 'zz.social', 'sieve': 'zz.social'})

    def test_agreed_benchmark_thousand_patterns(self):
        rules = ['from ^from%04d@example[.]org$ move from-routes' % i for i in range(500)]
        rules += ['to ^to%04d@example[.]org$ move route-%d' % (i, i % 2) for i in range(500)]
        self.books({'known': ['known%d@example.org' % i for i in range(1000)]})
        generation = self.compile(rules)
        for i in [0, 1, 499]:
            for suffix, headers, local, sieve in [
                ('from', {'From': 'from%04d@example.org' % i, 'To': 'to%04d@example.org' % i}, 'from-routes', 'from-routes'),
                ('to', {'To': 'to%04d@example.org' % i}, 'route-%d' % (i % 2), 'route-%d' % (i % 2)),
                ('known', {'From': 'known%d@example.org' % i}, 'known', 'INBOX'),
                ('miss', {'From': 'xfrom%04d@example.org' % i, 'To': 'to%04d@exampleXorg' % i}, 'unsorted', 'unsorted')]:
                self.decide(generation, {'id': 'benchmark-%d-%s' % (i, suffix), 'headers': headers, 'local': local, 'sieve': sieve})

    def test_unsupported_patterns_retain_previous_generation(self):
        generation = self.compile(['from github.com move matched'])
        before = os.readlink(self.root / 'cache/filter-engine/current'), (self.root / 'Filters.sieve').read_bytes()
        for pattern in json.loads(FIXTURE.read_text())['unsupported']:
            self.rules.write_text('from '+json.dumps(pattern)+' move matched\n')
            result = self.run_tool([self.compiler, '--root', self.root], 4)
            self.assertIn(b'Filters.txt":1', result.stderr)
            self.assertRegex(result.stderr, b'E_REGEX(?:_EXPORT)? ')
            self.assertEqual((os.readlink(self.root / 'cache/filter-engine/current'),
                              (self.root / 'Filters.sieve').read_bytes()), before)
        self.assertTrue(generation.is_dir())

    def test_profile_admission_and_no_unused_regex(self):
        generation = self.compile(['from github.com move matched'])
        before = os.readlink(self.root / 'cache/filter-engine/current'), (self.root / 'Filters.sieve').read_bytes()
        for profile in [PROFILE.replace(DIGEST, '0'*64), PROFILE.replace('capability regex\n', ''),
                        PROFILE.replace('regex-qualification '+DIGEST+'\n', ''),
                        PROFILE.replace('pigeonhole-2.4.5-posix-ere-v1', 'core-v1')]:
            self.profile.write_text(profile)
            self.run_tool([self.compiler, '--root', self.root], 4)
            self.assertEqual((os.readlink(self.root / 'cache/filter-engine/current'),
                              (self.root / 'Filters.sieve').read_bytes()), before)
        self.profile.unlink()
        automatic = self.compile(['from github.com move matched'])
        self.assertEqual((automatic / 'Filters.sieve').read_bytes(), before[1])
        self.assertFalse(self.profile.exists())
        self.profile.write_text(PROFILE)
        generation = self.compile(['from-address person@example.org move matched'])
        self.assertNotIn('"regex"', (generation / 'Filters.sieve').read_text())


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    summary = {'executed': result.testsRun, 'failed': len(result.errors)+len(result.failures),
               'skipped': len(result.skipped), 'total': result.testsRun,
               'independent_native_and_sieve_decisions': RegexTests.decisions,
               'qualification': DIGEST, 'observedAt': datetime.now(timezone.utc).isoformat()}
    (RegexTests.work / 'summary.json').write_text(json.dumps(summary, sort_keys=True)+'\n')
    print('Regex qualification: '+json.dumps(summary, sort_keys=True))
    if RegexTests.temp:
        RegexTests.temp.cleanup()
    sys.exit(0 if result.wasSuccessful() else 1)
