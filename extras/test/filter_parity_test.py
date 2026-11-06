"""Authored winners vs pure C, immutable batch C and real pinned Pigeonhole.

Expected IDs are hashes of the authored winning source record, never cache
records or another backend's answer. No matching/Sieve evaluator lives here.
"""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
from filter_batch_test import FIELDS as BATCH_FIELDS, PROFILE, tree
from filter_native_test import ascii_fold, explicit
from sieve_oracle import SieveOracle

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / 'extras/test/fixtures/filter-policy'
CORE = 'version 1\nprofile core-v1\ncapability fileinto\ncapability mailbox\n'


def tokens(line):
    """Decode authored record strings only, to resolve a named winner's ID."""
    result, pos = [], 0
    while pos < len(line):
        if line[pos] in ' \t':
            pos += 1
        elif line[pos] == '"':
            value, size = json.JSONDecoder().raw_decode(line[pos:])
            assert isinstance(value, str)
            result.append(value)
            pos += size
        else:
            end = pos
            while end < len(line) and line[end] not in ' \t':
                end += 1
            result.append(line[pos:end])
            pos = end
    return result


def expected(case, rules):
    alias = case['rule']
    kind, value = alias.split(':', 1)
    logical = 'spam' if alias == 'builtin:spam' else case['local']
    if kind == 'builtin':
        identity = alias
    elif kind in ('known', 'blacklist', 'own'):
        identity = kind + ':' + hashlib.sha256(ascii_fold(value).encode()).hexdigest()
    else:
        assert kind in ('filters', 'domain')
        position = int(value)
        record = tokens(rules[position-1])
        selector, predicate = record[:2]
        if selector not in ('from', 'to'):
            predicate = ascii_fold(predicate)
        logical = 'known' if selector == 'known-domain' else record[3]
        occurrence = 1
        for previous in rules[:position-1]:
            prior = tokens(previous)
            key = prior[1] if prior[0] in ('from', 'to') else ascii_fold(prior[1])
            destination = 'known' if prior[0] == 'known-domain' else prior[3]
            occurrence += (prior[0], key, destination) == (selector, predicate, logical)
        identity = explicit(selector, predicate, logical, occurrence=occurrence)
    return {'rule_id': identity, 'action': case.get('action', 'move'), 'logical': logical,
            'local': case['local'], 'sieve': case['sieve']}


class ParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get('JARO_FILTER_PARITY_WORK')
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-parity-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-compile', 'filter-policy-driver',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler, cls.batch, cls.pure = (cls.objects / x for x in
                                            ('jaro-filter-compile', 'jaro-filter', 'filter-policy-driver'))
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])
        cls.events, cls.decisions, cls.mutations = [], [], []

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        (cls.work / 'matrix.json').write_text(json.dumps({'decisions': cls.decisions, 'mutations': cls.mutations}, indent=2) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()

    def command(self, command, code=0, data=None):
        command = list(map(str, command))
        result = subprocess.run(command, input=data, cwd=REPO, capture_output=True, timeout=40)
        logs = {}
        for stream in ('stdout', 'stderr'):
            raw = getattr(result, stream)
            path = self.case / ('command-%05d.%s' % (len(self.events), stream))
            path.write_bytes(raw)
            logs[stream] = {'path': str(path), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
        self.events.append(dict(command=command, cwd=str(REPO), exit=result.returncode, **logs,
                                observedAt=datetime.now(timezone.utc).isoformat()))
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def build(self, name, rules, books=None, profile=PROFILE):
        root = self.case / name
        root.mkdir()
        (root / 'Filters.txt').write_text('\n'.join(rules) + '\n')
        (root / 'Aliases.txt').write_text('\n'.join(json.dumps(x) for x in (books or {}).get('own', [])))
        (root / 'FilterProfile.txt').write_text(profile)
        for key, filename in (('known', 'whitelist.abook'), ('blacklist', 'blacklist.abook')):
            values = (books or {}).get(key, [])
            raw = '[format]\nprogram=abook\nversion=0.6.0\n'
            raw += ''.join('[%d]\nname=Authored Fixture\nemail=%s\n' % (i, x) for i, x in enumerate(values))
            (root / filename).write_text(raw if values else '')
        self.command([self.compiler, '--root', root])
        return root

    def imported(self, root, name):
        target = self.case / name
        target.mkdir()
        (target / 'Filters.txt').write_bytes(b'')
        (target / 'FilterProfile.txt').write_bytes((root / 'FilterProfile.txt').read_bytes())
        self.command([self.compiler, '--root', target, '--import-sieve', root / 'Filters.sieve'])
        first = (target / 'Filters.sieve').read_bytes()
        self.command([self.compiler, '--root', target, '--import-sieve', root / 'Filters.sieve'])
        self.assertEqual((target / 'Filters.sieve').read_bytes(), first)
        return target

    def sources(self, root):
        args = ['filters', 'Filters.txt', str(root / 'Filters.txt'), str(root / 'Filters.txt')]
        aliases = root / 'Aliases.txt'
        if aliases.exists():
            args += ['aliases', 'Aliases.txt', str(aliases), str(aliases)]
        for key, name in (('known', 'whitelist.abook'), ('blacklist', 'blacklist.abook')):
            book = root / name
            exported = self.case / ('%s-%s.txt' % (root.name, key))
            raw = b''
            if book.exists() and book.stat().st_size:
                result = self.command(['abook', '--config', '/dev/null', '--datafile', book, '--mutt-query', ''])
                for line in result.stdout.splitlines():
                    if not line:
                        continue
                    self.assertIn(b'\t', line)
                    raw += line.split(b'\t')[0] + b'\n'
            exported.write_bytes(raw)
            args += [key, name, str(book), str(exported)]
        return args

    def evaluate(self, root, cases, rules, variant, extensions='+regex', create=True, script=None, mutation=False):
        sources = self.sources(root)
        incoming = root / 'incoming'
        incoming.mkdir(exist_ok=True)
        paths = []
        for i, case in enumerate(cases):
            path = incoming / ('%03d-%s.eml' % (i, case['id']))
            headers = case['headers']
            pairs = headers.items() if isinstance(headers, dict) else headers
            path.write_bytes((''.join('%s: %s\r\n' % tuple(x) for x in pairs) +
                              'Subject: Independent parity fixture\r\n\r\n').encode() +
                             b'Body immutable\x00\xff\nFrom: blocked@example.org\n')
            paths.append(path)
        before = tree(root)
        output = self.command([self.batch, '--root', root, '--files0-from', '-'],
                              data=b''.join(os.fsencode(p) + b'\0' for p in paths)).stdout.split(b'\0')
        self.assertEqual(output[:2], [b'JFD1', (root / 'cache/filter-engine/current').resolve().name.encode()])
        self.assertEqual(output[-1], b'')
        self.assertEqual(len(output[2:-1]), len(cases)*11)
        script = script or root / 'Filters.sieve'
        self.oracle.compile(script, extensions=extensions)
        results = []
        for i, (case, path) in enumerate(zip(cases, paths)):
            want = expected(case, rules)
            pure = self.command([self.pure, '--headers', path, *sources]).stdout.split(b'\0')
            self.assertEqual(len(pure), 17)
            native = dict(zip(('rule_id', 'action', 'logical', 'local'),
                              (pure[j].decode() for j in (0, 3, 4, 5))))
            row = dict(zip(BATCH_FIELDS, output[2+i*11:2+(i+1)*11]))
            self.assertEqual(row['path'], os.fsencode(path))
            self.assertEqual(row['index'], str(i+1).encode())
            self.assertEqual((row['status'], row['detail']), (b'classified', b''))
            batch = {k: row[k].decode() for k in ('rule_id', 'action', 'logical', 'sieve')}
            batch['local'] = os.path.relpath(os.fsdecode(row['local']), root)
            sieve = self.oracle.evaluate(script, path, extensions=extensions)
            failures = {}
            if native != {k: want[k] for k in native}:
                failures['pure'] = native
            if batch != want:
                failures['batch'] = batch
            if sieve != {'action': 'fileinto', 'mailbox': want['sieve'], 'create': create}:
                failures['sieve'] = sieve
            result = {'id': case['id'], 'variant': variant, 'expected': want, 'pure': native,
                      'batch': batch, 'sieve': sieve, 'mismatches': failures,
                      'message_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                      'trace': str(self.oracle.work / ('case-%03d/trace.txt' % self.oracle.sequence))}
            results.append(result)
            (self.mutations if mutation else self.decisions).append(result)
            if not mutation:
                self.assertEqual(failures, {}, json.dumps(result, sort_keys=True))
        self.assertEqual(tree(root), before, 'classification or Sieve simulation changed source configuration/mail')
        return results

    def test_authored_policy_matrix_before_and_after_import(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        casing = json.loads((FIXTURES / 'casing.json').read_text())
        roundtrip = json.loads((FIXTURES / 'roundtrip.json').read_text())
        groups = [(target['cases'], target['rules'], target, PROFILE),
                  (target['domain_cases'], target['domain_rules'], {}, CORE),
                  (casing['cases'], casing['rules'], casing, CORE),
                  (roundtrip['cases'], roundtrip['rules'], roundtrip, roundtrip['profile'])]
        groups += [([case], case['rules'], {}, CORE) for case in target['isolated_domain_cases']]
        empty = [
            {'id': 'empty-fallback', 'headers': {'From': 'stranger@elsewhere.example'}, 'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'},
            {'id': 'empty-bounce', 'headers': {'Sender': 'mailman-bounce@list.example'}, 'rule': 'builtin:bounce', 'local': 'zz.bounces', 'sieve': 'zz.bounces'},
            {'id': 'empty-spam', 'headers': {'X-Spam-Flag': 'yes'}, 'rule': 'builtin:spam', 'local': 'zz.spam', 'sieve': 'Spam'},
            {'id': 'empty-own-set', 'headers': {'To': 'own@example.org'}, 'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'}]
        groups.append((empty, [], {}, CORE))
        ids = [case['id'] for cases, _, _, _ in groups for case in cases]
        self.assertEqual(len(ids), 73)
        self.assertEqual(len(set(ids)), 73)
        for i, (cases, rules, books, profile) in enumerate(groups):
            root = self.build('group-%d' % i, rules, books, profile)
            clone = self.imported(root, 'imported-%d' % i)
            for label, actual in (('original', root), ('imported', clone)):
                self.evaluate(actual, cases, rules, 'policy-%d-%s' % (i, label))

    def test_qualified_regex_corpus_before_and_after_import(self):
        fixture = REPO / 'extras/test/fixtures/filter-contract/regex-subset.json'
        corpus = json.loads(fixture.read_text())
        catalogue = json.loads((REPO / 'doc/filter-regex-qualification.json').read_text())
        self.assertEqual(hashlib.sha256(fixture.read_bytes()).hexdigest(), catalogue['fixture_sha256'])
        self.assertEqual(len(corpus['cases']), 141)
        self.assertEqual(len({c['id'] for c in corpus['cases']}), 141)
        groups = {}
        for case in corpus['cases']:
            groups.setdefault(case['pattern'], []).append({
                'id': case['id'], 'headers': {case['field']: case['address']},
                'rule': 'filters:1' if case['expected'] == 'matched' else 'builtin:unsorted',
                'local': case['expected'], 'sieve': case['expected']})
        for i, (pattern, cases) in enumerate(groups.items()):
            rules = ['from ' + json.dumps(pattern) + ' move matched']
            root = self.build('regex-%d' % i, rules)
            clone = self.imported(root, 'imported-regex-%d' % i)
            for label, actual in (('original', root), ('imported', clone)):
                self.evaluate(actual, cases, rules, 'regex-%d-%s' % (i, label))

    def test_core_without_regex_or_mailbox_create(self):
        rules = ['from-address exact@example.org move exact']
        cases = [
            {'id': 'core-exact', 'headers': {'Sender': 'EXACT@EXAMPLE.ORG'}, 'rule': 'filters:1', 'local': 'exact', 'sieve': 'exact'},
            {'id': 'core-not-glob', 'headers': {'From': 'exact-other@example.org'}, 'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'},
            {'id': 'core-spam', 'headers': {'X-Spam-Flag': 'YES'}, 'rule': 'builtin:spam', 'local': 'zz.spam', 'sieve': 'Spam'},
            {'id': 'core-empty-fields', 'headers': {}, 'rule': 'builtin:unsorted', 'local': 'unsorted', 'sieve': 'unsorted'}]
        root = self.build('core', rules, profile='version 1\nprofile core-v1\ncapability fileinto\ncreate-mailboxes no\n')
        self.assertNotIn(b'"regex"', (root / 'Filters.sieve').read_bytes())
        self.assertNotIn(b':create', (root / 'Filters.sieve').read_bytes())
        self.evaluate(root, cases, rules, 'core-no-create', extensions=None, create=False)

    def test_mutations_catch_shared_wrong_destination_and_sieve_wrong_priority(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        case = next(c for c in target['cases'] if c['id'] == 'from-before-to')
        original = target['rules']
        wrong = list(original)
        wrong[0] = wrong[0].replace('frombucket', 'deliberately-wrong')
        root = self.build('shared-wrong', wrong, target)
        result = self.evaluate(root, [case], original, 'shared-destination-mutation', mutation=True)[0]
        self.assertEqual(set(result['mismatches']), {'pure', 'batch', 'sieve'})
        self.assertEqual(result['pure']['local'], 'deliberately-wrong')
        self.assertEqual(result['batch']['local'], 'deliberately-wrong')
        self.assertEqual(result['sieve']['mailbox'], 'deliberately-wrong')
        root = self.build('baseline', original, target)
        parts = (root / 'Filters.sieve').read_text().split('if true {\n')
        self.assertEqual(len(parts), 8)
        parts[3], parts[4] = parts[4], parts[3]
        script = self.case / 'deliberately-wrong-priority.sieve'
        script.write_text('if true {\n'.join(parts))
        result = self.evaluate(root, [case], original, 'sieve-priority-mutation', script=script, mutation=True)[0]
        self.assertEqual(set(result['mismatches']), {'sieve'})
        self.assertEqual(result['sieve']['mailbox'], 'tobucket')

    def test_empty_batch_has_complete_header_and_no_delivery(self):
        root = self.build('empty', [], profile=CORE)
        before = tree(root)
        output = self.command([self.batch, '--root', root, '--files0-from', '-'], data=b'').stdout
        self.assertEqual(output, b'JFD1\0' + (root / 'cache/filter-engine/current').resolve().name.encode() + b'\0')
        self.assertEqual(tree(root), before)

    def test_unsupported_regex_is_explicit_and_preserves_previous_policy(self):
        root = self.build('capability', ['from-address exact@example.org move exact'], profile=CORE)
        selected = os.readlink(root / 'cache/filter-engine/current')
        public = (root / 'Filters.sieve').read_bytes()
        (root / 'Filters.txt').write_text('from github.com move matched\n')
        self.assertIn(b'E_REGEX_EXPORT', self.command([self.compiler, '--root', root], code=4).stderr)
        (root / 'FilterProfile.txt').write_text(PROFILE)
        (root / 'Filters.txt').write_text('from "user+.*" move matched\n')
        self.assertIn(b'E_REGEX_EXPORT', self.command([self.compiler, '--root', root], code=4).stderr)
        self.assertEqual(os.readlink(root / 'cache/filter-engine/current'), selected)
        self.assertEqual((root / 'Filters.sieve').read_bytes(), public)


if __name__ == '__main__':
    program = unittest.main(verbosity=2, exit=False)
    result = program.result
    print('Independent parity: ' + json.dumps({
        'executed': result.testsRun, 'failed': len(result.failures) + len(result.errors),
        'skipped': len(result.skipped), 'positive_decisions_per_backend': len(getattr(ParityTests, 'decisions', [])),
        'mutation_controls': len(getattr(ParityTests, 'mutations', [])),
        'all_commands_waited': True}, sort_keys=True))
    if result.wasSuccessful():
        assert result.testsRun == 6 and len(ParityTests.decisions) == 432 and len(ParityTests.mutations) == 2
    raise SystemExit(not result.wasSuccessful())
