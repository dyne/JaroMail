"""External Pigeonhole comparisons; local suites never construct an oracle."""
import json
import os
import sys
import unittest

sys.dont_write_bytecode = True
import filter_batch_test as fixtures
import filter_storage_test as storage
from sieve_oracle import SieveOracle


class OracleTests(storage.StorageTests):
    work_variable = 'JARO_FILTER_ORACLE_WORK'
    batch_paths = fixtures.BatchTests.batch_paths
    snapshot = fixtures.BatchTests.snapshot

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])

    def authored_comparisons(self, apply):
        count = 0
        for group, (cases, rules, contacts) in enumerate(fixtures.authored_groups()):
            books = {key: contacts.get(key, []) for key in ['known', 'blacklist', 'own']}
            product = self.compile(rules, **books)
            self.oracle.compile(product / 'Filters.sieve', extensions='+regex')
            paths = [self.message(('cur/' if i % 2 else 'new/') + '%d-%s:2,FS' % (group, case['id']), case['headers'])
                     for i, case in enumerate(cases)]
            for path, case in zip(paths, cases):
                self.oracle.expect(product / 'Filters.sieve', path, case['sieve'], True, extensions='+regex')
                self.counts['oracle_decisions'] += 1
                count += 1
            bodies = [path.read_bytes() for path in paths]
            rows = self.apply(paths) if apply else self.batch_paths(paths)
            self.assertEqual(len(rows), len(cases))
            for row, path, case, body in zip(rows, paths, cases, bodies):
                self.assertEqual(row['path'], os.fsencode(path))
                self.assertEqual(row['status'], b'moved' if apply else b'classified')
                self.assert_target(dict(row, status=b'classified'), case, rules, books)
                if apply:
                    self.assertFalse(path.exists())
                    self.assertEqual((self.root / case['local'] / 'cur' / path.name).read_bytes(), body)
        self.assertEqual(count, 52)

    def test_original_52_batch_sieve_comparisons(self):
        self.authored_comparisons(False)

    def test_original_52_apply_sieve_comparisons(self):
        self.authored_comparisons(True)

    def test_normalization_and_qualified_regex_sieve_comparisons(self):
        corpus = json.loads((fixtures.REPO / 'extras/test/fixtures/filter-contract/regex-subset.json').read_text())
        previous = None
        for i, case in enumerate(corpus['cases']):
            if case['pattern'] != previous:
                product = self.compile(['from ' + json.dumps(case['pattern']) + ' move matched'])
                previous = case['pattern']
            path = self.message('new/regex-%d' % i, {case['field']: case['address']})
            row = self.batch_paths([path])[0]
            expected = case['expected']
            self.assertEqual(row['sieve'], expected.encode())
            self.oracle.expect(product / 'Filters.sieve', path, expected, True, extensions='+regex')
            self.counts['oracle_decisions'] += 1
        normal = json.loads((fixtures.REPO / 'extras/test/fixtures/filter-contract/mailbox-normalization.json').read_text())
        for i, group in enumerate(normal['groups']):
            product = self.compile(['from-address %s move normalized' % json.dumps(group['canonical'])])
            paths = [self.message('new/normal-%d-%d' % (i, j), {'From': form}) for j, form in enumerate(group['forms'])]
            rows = self.batch_paths(paths)
            self.assertEqual(len(rows), len(paths))
            for row, path in zip(rows, paths):
                self.assertEqual(row['path'], os.fsencode(path))
                self.assertEqual(row['sieve'], b'normalized')
                self.oracle.expect(product / 'Filters.sieve', path, 'normalized', True, extensions='+regex')
                self.counts['oracle_decisions'] += 1


def load_tests(loader, tests, pattern):
    # Inherit fixture helpers, but do not rerun the unrelated local test methods.
    return unittest.TestSuite(OracleTests(name) for name in sorted(OracleTests.__dict__) if name.startswith('test_'))


if __name__ == '__main__':
    program = unittest.main(exit=False, verbosity=2)
    print('Oracle counts: ' + json.dumps(getattr(OracleTests, 'counts', {}), sort_keys=True))
    raise SystemExit(not program.result.wasSuccessful())
