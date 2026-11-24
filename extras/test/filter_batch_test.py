"""Local decision-only batch contract and independently authored expectations."""
from datetime import datetime, timezone
from pathlib import Path
import errno
import fcntl
import hashlib
import json
import os
import select
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import contextmanager
sys.dont_write_bytecode = True
import filter_native_test as authored

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / 'extras/test/fixtures/filter-policy'
CATALOGUE = REPO / 'docs/filter-regex-qualification.json'
PROFILE = ('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\n'
           'capability mailbox\ncapability regex\nregex-qualification ' +
           hashlib.sha256(CATALOGUE.read_bytes()).hexdigest() + '\n')
FIELDS = 'index path rule_id action logical local sieve status source line detail'.split()


def authored_groups():
    target = json.loads((FIXTURES / 'target.json').read_text())
    casing = json.loads((FIXTURES / 'casing.json').read_text())
    return ([(target['cases'], target['rules'], target),
             (target['domain_cases'], target['domain_rules'], {}),
             (casing['cases'], casing['rules'], casing)] +
            [([case], case['rules'], {}) for case in target['isolated_domain_cases']])


def write_policy_fixture(root, rules, known=(), blacklist=(), own=(), profile=PROFILE):
    (root / 'Filters.txt').write_text('\n'.join(rules) + '\n')
    (root / 'Aliases.txt').write_text('\n'.join(json.dumps(v) for v in own))
    for filename, values in [('whitelist.abook', known), ('blacklist.abook', blacklist)]:
        text = '[format]\nprogram=abook\nversion=0.6.0\n'
        for i, address in enumerate(values):
            text += '\n[%d]\nname=Fixture\nemail=%s\n' % (i, address)
        (root / filename).write_text(text if values else '')
    (root / 'FilterProfile.txt').write_text(profile)


def tree(path):
    """Complete raw-path/mode/type/content snapshot; reads may update atime."""
    base = os.fsencode(path)
    st = os.lstat(base)
    result = {b'.': (stat.S_IFMT(st.st_mode),stat.S_IMODE(st.st_mode),b'')}
    for parent, directories, files in os.walk(base, followlinks=False):
        for name in directories + files:
            full = os.path.join(parent, name)
            st = os.lstat(full)
            payload = os.readlink(full) if stat.S_ISLNK(st.st_mode) else (
                hashlib.sha256(Path(os.fsdecode(full)).read_bytes()).digest() if stat.S_ISREG(st.st_mode) else b'')
            result[os.path.relpath(full, base)] = (stat.S_IFMT(st.st_mode), stat.S_IMODE(st.st_mode), payload)
    return result


def install_sieve(objects):
    prefix = objects / 'installed runtime'
    subprocess.run(['cmake', '--install', objects / 'cmake', '--component', 'sieve', '--prefix', prefix], check=True)
    return prefix / 'bin/jaro-sieve-filter'


def report_args(cli, root, source, generation, *options):
    return [cli, '--from', source, '--to', root, '--report0', generation.name, *options, generation / 'Filters.sieve']


class BatchTests(unittest.TestCase):
    work_variable = 'JARO_FILTER_BATCH_WORK'

    @classmethod
    def setUpClass(cls):
        retained = os.environ.get(cls.work_variable)
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            assert cls.work.is_absolute()
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-batch-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-compile',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler, cls.batch = cls.objects / 'jaro-filter-compile', install_sieve(cls.objects)
        cls.events, cls.counts = [], dict(invocations=0, target_decisions=0, oracle_decisions=0,
                                         messages=0, snapshots=0, controlled_children=0, unsettled_children=0)

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        (cls.work / 'counts.json').write_text(json.dumps(cls.counts, sort_keys=True) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()
        self.root = self.case / 'mail root'
        self.root.mkdir(mode=0o700)
        self.incoming = self.root / 'incoming'
        self.maildir(self.incoming)

    def maildir(self, path):
        for name in ['cur', 'new', 'tmp']:
            (path / name).mkdir(parents=True)

    def command(self, args, code=0, data=None, cwd=None):
        args = list(map(os.fspath, args))
        result = subprocess.run(args, input=data, cwd=cwd or REPO, capture_output=True, timeout=60)
        self.capture(args,result,cwd)
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def capture(self,args,result,cwd=None,extra=None):
        number = len(self.events)
        logs = {}
        for stream in ['stdout', 'stderr']:
            raw = getattr(result, stream)
            log = self.case / ('command-%05d.%s' % (number, stream))
            log.write_bytes(raw)
            logs[stream] = dict(path=str(log), sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw))
        self.events.append(dict(command=args, cwd=str(cwd or REPO), exit=result.returncode,
                                observedAt=datetime.now(timezone.utc).isoformat(), **logs, **(extra or {})))
        self.counts['invocations'] += 1

    def snapshot(self,before,after):
        self.assertEqual(after,before)
        self.counts['snapshots'] += 1
        rows = [dict(path_hex=path.hex(),type=values[0],permissions=values[1],payload_hex=values[2].hex())
                for path,values in sorted(before.items())]
        artifact = self.case/('snapshot-%05d.json' % self.counts['snapshots'])
        artifact.write_text(json.dumps(dict(root=str(self.root),rows=rows,after_equal=True),sort_keys=True)+'\n')

    def compile(self, rules=(), known=(), blacklist=(), own=(), profile=PROFILE):
        write_policy_fixture(self.root, rules, known, blacklist, own, profile)
        output = self.command([self.compiler, '--root', self.root, '--report0']).stdout.split(b'\0')
        self.generation = output[1]
        self.product = Path(os.fsdecode(output[3]))
        return self.product

    def decode(self, result, generation=None):
        values = result.stdout.split(b'\0')
        self.assertEqual(values[-1], b'')
        self.assertEqual(values[:2], [b'JFD1', generation or self.generation])
        values = values[2:-1]
        self.assertEqual(len(values) % 11, 0)
        rows = [dict(zip(FIELDS, values[i:i+11])) for i in range(0, len(values), 11)]
        self.assertEqual([row['index'] for row in rows], [str(i+1).encode() for i in range(len(rows))])
        self.counts['messages'] += len(rows)
        return rows

    def batch_paths(self, paths):
        before = tree(self.root)
        rows = []
        for message in paths:
            args = report_args(self.batch, self.root, self.incoming, self.product, '--dry-run', '--message', message)
            rows.extend(self.decode(self.command(args)))
        self.snapshot(before,tree(self.root))
        return rows

    def apply(self, paths):
        rows = self.decode(self.command(report_args(self.batch, self.root, self.incoming, self.product)))
        by_path = {row['path']: row for row in rows}
        self.assertEqual(set(by_path), {os.fsencode(path) for path in paths})
        return [by_path[os.fsencode(path)] for path in paths]

    def message(self, name, headers=None, raw=None, folder=None):
        path = (folder or self.incoming) / name
        if raw is None:
            pairs = (headers or {}).items() if isinstance(headers or {}, dict) else headers
            raw = (''.join('%s: %s\r\n' % tuple(p) for p in pairs) + 'Subject: Batch fixture\r\n\r\n').encode()
            raw += b'Body untouched\x00\xff\r\nFrom: blocked@example.org\n'
        path.write_bytes(raw)
        return path

    def assert_target(self, row, case, rules, books):
        expected = authored.PolicyTests().expected_decision(case, rules, **books)
        self.assertEqual(row['rule_id'], expected['id'].encode(), case['id'])
        self.assertEqual(row['logical'], expected['sieve'].encode(), case['id'])
        self.assertEqual(row['local'], os.fsencode(self.root / expected['sieve']), case['id'])
        self.assertEqual(row['sieve'], expected['sieve'].encode(), case['id'])
        source = expected['source'].replace('/mail/', str(self.root) + '/')
        if expected['selector'] in ('allow', 'block'):
            source = self.root / 'contacts.sqlite'
            roster = books['known' if expected['selector'] == 'allow' else 'blacklist']
            keys = sorted(set(authored.ascii_fold(mailbox) for mailbox in roster))
            expected['line'] = str(keys.index(expected['predicate']) + 1)
        self.assertEqual(row['source'], os.fsencode(source), case['id'])
        self.assertEqual(row['line'], expected['line'].encode(), case['id'])
        self.assertEqual((row['action'], row['status'], row['detail']), (b'move', b'classified', b''))

    def test_original_52_authored_targets(self):
        count = 0
        for group, (cases, rules, contacts) in enumerate(authored_groups()):
            if group:
                self.root = self.case / f'mail root {group}'
                self.root.mkdir(mode=0o700)
                self.incoming = self.root / 'incoming'
                self.maildir(self.incoming)
            books = {key: contacts.get(key, []) for key in ['known', 'blacklist', 'own']}
            self.compile(rules, **books)
            paths = [self.message(('cur/' if i % 2 else 'new/') + '%d-%s:2,FS' % (group, case['id']), case['headers'])
                     for i, case in enumerate(cases)]
            rows = self.batch_paths(paths)
            self.assertEqual(len(rows), len(cases))
            for row, path, case in zip(rows, paths, cases):
                self.assertEqual(row['path'], os.fsencode(path))
                self.assert_target(row, case, rules, books)
                count += 1
        self.assertEqual(count, 52)
        self.counts['target_decisions'] += count
        print('Independent batch target identities: 52/52')

    def test_normalization_and_qualified_regex_corpus(self):
        corpus = json.loads((REPO / 'extras/test/fixtures/filter-contract/regex-subset.json').read_text())
        previous = None
        for i, case in enumerate(corpus['cases']):
            if case['pattern'] != previous:
                self.compile(['from '+json.dumps(case['pattern'])+' move matched'])
                previous = case['pattern']
            path = self.message('new/regex-%d' % i, {case['field']: case['address']})
            row = self.batch_paths([path])[0]
            expected = case['expected']
            self.assertEqual((row['local'], row['sieve']), (os.fsencode(self.root/expected), expected.encode()))
            expected_id = authored.explicit('from', case['pattern'], 'matched') if expected=='matched' else 'builtin:unsorted'
            self.assertEqual(row['rule_id'], expected_id.encode())
        normal = json.loads((REPO/'extras/test/fixtures/filter-contract/mailbox-normalization.json').read_text())
        for i, group in enumerate(normal['groups']):
            self.compile(['from-address %s move normalized' % json.dumps(group['canonical'])])
            paths = [self.message('new/normal-%d-%d' % (i, j), {'From': form}) for j, form in enumerate(group['forms'])]
            for row, path in zip(self.batch_paths(paths), paths):
                expected = authored.explicit('from-address', authored.ascii_fold(group['canonical']), 'normalized')
                self.assertEqual(row['rule_id'], expected.encode())

if __name__ == '__main__':
    unittest.main(verbosity=2)
