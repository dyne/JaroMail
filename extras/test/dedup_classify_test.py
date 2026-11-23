"""Read-only Maildir candidate reduction using disposable byte fixtures."""
from collections import defaultdict
from hashlib import sha256
import os
from pathlib import Path
import random
import select
import stat
import subprocess
import unittest

import dedup_inventory_test as inventory

BIN = Path(os.environ.get('JARO_DEDUP_TEST_BIN_DIR', inventory.REPO / 'build/gnu'))
HELPER = BIN / 'jaro-dedup-test'
WINDOW = 16 * 1024
BLOCK = 64 * 1024


def parse(data):
    fields = iter(data.split(b'\0'))
    pairs, buckets = [], defaultdict(list)

    def path():
        return int(next(fields)), int(next(fields)), next(fields)

    for kind in fields:
        if kind == b'P':
            pairs.append((path(), path()))
        elif kind == b'H':
            key = int(next(fields)), next(fields).decode('ascii')
            buckets[key].append(path())
        else:
            assert kind == b'' and next(fields, None) is None, kind
    return pairs, dict(buckets)


def counters(stderr):
    line, = (line for line in stderr.splitlines() if line.startswith(b'dedup candidates: '))
    return {key.decode(): int(value) for item in line.split()[2:]
            for key, value in (item.split(b'='),)}


def survivor_map(data):
    fields = data.split(b'\0')
    assert fields.pop() == b'' and len(fields) % 6 == 0

    def entry(offset):
        return int(fields[offset]), int(fields[offset + 1]), fields[offset + 2]

    return [(entry(offset), entry(offset + 3)) for offset in range(0, len(fields), 6)]


def exhaustive_survivors(roots):
    """Group complete raw bodies from the small fixtures in priority order."""
    winners, result = {}, []
    for rank, root in enumerate(roots):
        for subrank, sub in enumerate((b'cur', b'new')):
            parent = os.path.join(os.fsencode(root), sub)
            for name in sorted(os.listdir(parent)):
                path = os.path.join(parent, name)
                if not stat.S_ISREG(os.lstat(path).st_mode):
                    continue
                occurrence = rank, subrank, name
                with open(path, 'rb') as stream:
                    keeper = winners.setdefault(stream.read(), occurrence)
                result.append((occurrence, keeper))
    return result


class Candidates(unittest.TestCase):
    setUp = inventory.Inventory.setUp
    folder = inventory.Inventory.folder

    def command(self, root, *options, binary=HELPER):
        return [binary, '--inventory', '--test-candidates', '--scratch-dir', self.root,
                *options, '--', root]

    def reduced(self, root, *options):
        result = subprocess.run(self.command(root, *options), env=self.env,
                                capture_output=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.root.glob('jaro-dedup-*')), [])
        return (*parse(result.stdout), counters(result.stderr))

    def test_unique_sizes_never_open_payload(self):
        root = self.folder('unique')
        for index, size in enumerate((0, 1, WINDOW, 2**32 + 1)):
            with open(root / 'cur' / str(index), 'wb') as stream:
                stream.truncate(size)
        pairs, hashes, stats = self.reduced(root)
        self.assertEqual((pairs, hashes), ([], {}))
        self.assertTrue(all(value == 0 for value in stats.values()), stats)

    def test_windows_and_small_hash_route(self):
        root = self.folder('windows')
        sizes = (0, WINDOW - 1, WINDOW, WINDOW + 1, 2 * WINDOW - 1, 2 * WINDOW)
        expected = {}
        for size in sizes:
            data = b'\x00\xff\r\n' * (size // 4) + b'X' * (size % 4)
            name = str(size).encode()
            (root / 'new' / os.fsdecode(name + b'-c')).write_bytes(data)
            (root / 'cur' / os.fsdecode(name + b'-z')).write_bytes(data)
            different = data[:-1] + b'!' if data else b''
            (root / 'cur' / os.fsdecode(name + b'-a')).write_bytes(different)
            expected[(size, sha256(data).hexdigest())] = (
                [(0, 0, name + b'-a')] if not data else []
            ) + [(0, 0, name + b'-z'), (0, 1, name + b'-c')]
        pairs, hashes, stats = self.reduced(root)
        self.assertEqual(pairs, [])
        self.assertEqual(hashes, expected)
        self.assertEqual(stats['hashed-files'], 3 * len(sizes))
        self.assertEqual(stats['hash-opens'], 3 * len(sizes))
        self.assertEqual(stats['hash-bytes'], 3 * sum(sizes))
        self.assertEqual(stats['sample-opens'], 0)
        self.assertEqual(stats['pair-opens'], 0)

    def test_samples_reject_tails_and_hash_ambiguous_middles(self):
        root = self.folder('samples')
        data = b'\x00' * WINDOW + b'\xff' + b'\r' * WINDOW
        (root / 'new' / 'b:2,F').write_bytes(data)
        (root / 'cur' / '\udcff:2,S').write_bytes(data)
        (root / 'cur' / 'middle').write_bytes(data[:WINDOW] + b'\x00' + data[WINDOW+1:])
        (root / 'cur' / 'tail').write_bytes(data[:-1] + b'!')
        (root / 'new' / 'head').write_bytes(b'!' + data[1:])
        before = inventory.snapshot(root)
        pairs, hashes, stats = self.reduced(root)
        self.assertEqual(inventory.snapshot(root), before)
        self.assertEqual(pairs, [])
        self.assertEqual(hashes, {(len(data), sha256(data).hexdigest()):
                                  [(0, 0, b'\xff:2,S'), (0, 1, b'b:2,F')]})
        self.assertEqual(stats['repeated-sizes'], 1)
        self.assertEqual(stats['repeated-entries'], 5)
        self.assertEqual(stats['sampled-files'], 5)
        self.assertEqual(stats['sample-opens'], 5)
        self.assertEqual(stats['sample-bytes'], 5 * 2 * WINDOW)
        self.assertEqual(stats['sample-buckets'], 1)
        self.assertEqual(stats['hash-opens'], 3)
        self.assertEqual(stats['hash-bytes'], 3 * len(data))
        self.assertEqual(stats['pair-opens'], 0)

    def test_pairs_compare_directly_and_exit_on_first_different_block(self):
        root = self.folder('pairs')
        data = b'Content-Type: application/octet-stream\r\n\r\n' + bytes(range(256)) * 520
        (root / 'new' / 'pair-one').write_bytes(data)
        (root / 'new' / 'pair-two').write_bytes(b'!' + data[1:])
        (root / 'new' / 'first\ncopy').write_bytes(data + b'\x00')
        (root / 'cur' / 'second\ncopy').write_bytes(data + b'\x00')
        (root / 'cur' / 'unique-tail').write_bytes(data + b'\xff')
        pairs, hashes, stats = self.reduced(root)
        self.assertEqual(pairs, [((0, 0, b'second\ncopy'), (0, 1, b'first\ncopy'))])
        self.assertEqual(hashes, {})
        self.assertEqual(stats['compared-pairs'], 2)
        self.assertEqual(stats['matching-pairs'], 1)
        self.assertEqual(stats['pair-opens'], 4)
        self.assertEqual(stats['pair-bytes'], 2 * (BLOCK + len(data) + 1))
        self.assertEqual(stats['sample-opens'], 3)
        self.assertEqual(stats['sample-bytes'], 3 * 2 * WINDOW)
        self.assertEqual(stats['hash-opens'], 0)

    def test_sparse_tail_reads_use_offsets_above_2_and_4_gib(self):
        root = self.folder('wide')
        for size in (2**31 + 3 * WINDOW + 1, 2**32 + 3 * WINDOW + 1):
            for index in range(3):
                with open(root / 'cur' / f'{size}-{index}', 'wb') as stream:
                    stream.seek(size - 1)
                    stream.write(bytes([index]))
        pairs, hashes, stats = self.reduced(root)
        self.assertEqual((pairs, hashes), ([], {}))
        self.assertEqual(stats['sample-opens'], 6)
        self.assertEqual(stats['sample-bytes'], 6 * 2 * WINDOW)
        self.assertEqual(stats['sample-buckets'], 0)
        self.assertEqual(stats['hash-opens'], 0)
        self.assertEqual(stats['pair-opens'], 0)

    def test_many_candidates_with_small_sqlite_cache(self):
        root = self.folder('large-group')
        data, count = b'zero\x00\xff', 2048
        for index in reversed(range(count)):
            (root / 'cur' / f'message-{index:04d}').write_bytes(data)
        pairs, hashes, stats = self.reduced(root, '--test-cache-kib', '16',
                                            '--test-heap-kib', '8192')
        self.assertEqual(pairs, [])
        self.assertEqual(hashes, {(len(data), sha256(data).hexdigest()):
                                  [(0, 0, f'message-{i:04d}'.encode()) for i in range(count)]})
        self.assertEqual(stats['hash-opens'], count)
        self.assertEqual(stats['hash-bytes'], count * len(data))

    def test_rebound_payload_aborts_and_cleans_scratch(self):
        root = self.folder('rebound')
        first, other = root / 'cur' / 'one', root / 'new' / 'two'
        first.write_bytes(b'same size')
        other.write_bytes(b'same size')
        process = subprocess.Popen(self.command(root, '--test-hold'), env=self.env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stderr], [], [], 10)
            self.assertTrue(ready, 'inventory must complete before replacing a message')
            announcement = process.stderr.readline()
            self.assertTrue(announcement.startswith(b'scratch-ready: '), announcement)
            first.unlink()
            first.symlink_to(other)
            out, err = process.communicate(b'x', timeout=10)
            self.assertEqual(process.returncode, 1, err)
            self.assertEqual(out, b'')
            self.assertIn(b'dedup content: open message', err)
            self.assertNotIn(b'dedup candidates:', err)
            self.assertTrue(first.is_symlink())
            self.assertEqual(other.read_bytes(), b'same size')
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()
        self.assertEqual(list(self.root.glob('jaro-dedup-*')), [])

    def test_production_rejects_test_entry(self):
        root = self.folder('production')
        for option in ('--test-candidates', '--test-classify', '--test-force-digest',
                       '--test-hold-after-reduction'):
            with self.subTest(option=option):
                command = [BIN / 'jaro-dedup', '--inventory', option, '--scratch-dir',
                           self.root, '--', root]
                result = subprocess.run(command, env=self.env, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, b'')


class Proof(unittest.TestCase):
    setUp = inventory.Inventory.setUp
    folder = inventory.Inventory.folder

    def command(self, roots, *options, mode='--test-classify'):
        return [HELPER, '--inventory', mode, '--scratch-dir', self.root, *options, '--', *roots]

    def classified(self, roots, *options):
        result = subprocess.run(self.command(roots, *options), env=self.env,
                                capture_output=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.root.glob('jaro-dedup-*')), [])
        return survivor_map(result.stdout), counters(result.stderr)

    def test_hash_collision_keeps_distinct_representatives(self):
        root = self.folder('collision')
        length = 2 * BLOCK + 1
        a = b'X' * length
        b = a[:BLOCK + 1] + b'Y' + a[BLOCK + 2:]
        (root / 'cur' / 'first').write_bytes(a)
        (root / 'cur' / 'second').write_bytes(b)
        (root / 'new' / 'third').write_bytes(b)
        command = self.command([root], '--test-force-digest', mode='--test-candidates')
        candidates = subprocess.run(command, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(candidates.returncode, 0, candidates.stderr)
        self.assertEqual(parse(candidates.stdout), ([], {(length, '0' * 64):
                         [(0, 0, b'first'), (0, 0, b'second'), (0, 1, b'third')]}))
        before = inventory.snapshot(root)
        mapping, stats = self.classified([root], '--test-force-digest')
        self.assertEqual(mapping, exhaustive_survivors([root]))
        self.assertEqual(mapping[-1], ((0, 1, b'third'), (0, 0, b'second')))
        self.assertEqual(stats['sampled-files'], 3)
        self.assertEqual(stats['hashed-files'], 3)
        self.assertEqual(stats['proof-buckets'], 1)
        self.assertEqual(stats['proof-comparisons'], 3)
        self.assertEqual(stats['proof-matches'], 1)
        self.assertEqual(stats['proof-bytes'], 8 * BLOCK + 2 * length)
        self.assertEqual(inventory.snapshot(root), before)

    def test_priority_and_complete_map_match_independent_oracle(self):
        without_id = b'Content-Type: application/octet-stream\r\n\r\n\0\xff\r\n'
        second_group = b'other body without an id'
        id_body = b'Message-ID: <reused@example.test>\n\n'
        specifications = [
            (0, b'new', b'a-before-cur:2,T', without_id),
            (0, b'cur', b'\xff:2,R', without_id),
            (0, b'cur', b'z\n:2,S', without_id),
            (1, b'cur', b'a:2,F', without_id),
            (0, b'new', b'only-new-first', second_group),
            (1, b'cur', b'another-cur', second_group),
            (0, b'new', b'reused-id-a', id_body + b'a'),
            (0, b'cur', b'reused-id-b', id_body + b'b'),
            (1, b'cur', b'reused-id-a', id_body + b'a'),
            (0, b'cur', b'.empty', b''),
            (1, b'new', b'-empty', b''),
            (1, b'new', b'unique', b'a uniquely sized message with its own trailing nul\0'),
        ]
        shuffled_outputs = []
        for seed in (1, 8):
            roots = [self.folder(f'primary {seed}'), self.folder(f'archive {seed}')]
            entries = specifications.copy()
            random.Random(seed).shuffle(entries)
            for rank, subdir, name, body in entries:
                path = os.path.join(os.fsencode(roots[rank]), subdir, name)
                with open(path, 'wb') as stream:
                    stream.write(body)
                os.chmod(path, 0o640 if subdir == b'cur' else 0o600)
            before = [inventory.snapshot(root) for root in roots]
            orders = []
            for operands in (roots, roots[::-1]):
                mapping, _ = self.classified(operands, '--test-force-digest')
                self.assertEqual(mapping, exhaustive_survivors(operands))
                orders.append(mapping)
            self.assertEqual([inventory.snapshot(root) for root in roots], before)
            shuffled_outputs.append(orders)
        first, swapped = shuffled_outputs[0]
        self.assertEqual(shuffled_outputs[0], shuffled_outputs[1])
        self.assertEqual(dict(first)[(1, 0, b'a:2,F')], (0, 0, b'z\n:2,S'))
        self.assertEqual(dict(first)[(1, 0, b'another-cur')], (0, 1, b'only-new-first'))
        self.assertEqual(dict(swapped)[(1, 1, b'only-new-first')], (0, 0, b'another-cur'))
        self.assertEqual(dict(swapped)[(1, 0, b'z\n:2,S')], (0, 0, b'a:2,F'))

    def test_same_inode_group_never_reads_sparse_body(self):
        roots = [self.folder('linked-first'), self.folder('linked-second')]
        original = roots[0] / 'cur' / 'b'
        with original.open('wb') as stream:
            stream.truncate(2**32 + 19)
        os.link(original, roots[0] / 'new' / 'a')
        os.link(original, roots[1] / 'cur' / 'a')
        mapping, stats = self.classified(roots)
        self.assertEqual(mapping, [
            ((0, 0, b'b'), (0, 0, b'b')),
            ((0, 1, b'a'), (0, 0, b'b')),
            ((1, 0, b'a'), (0, 0, b'b')),
        ])
        self.assertEqual(stats['matching-pairs'], 2)
        self.assertEqual(stats['pair-inode-matches'], 2)
        self.assertEqual(stats['pair-opens'], 4)
        for key in ('sample-opens', 'hash-opens', 'proof-opens', 'pair-bytes'):
            self.assertEqual(stats[key], 0)
        self.assertEqual(original.stat().st_nlink, 3)

    def test_same_inode_verification_inside_hash_bucket(self):
        root = self.folder('mixed-inodes')
        body = b'\xff\x00\r\nmixed'
        original = root / 'cur' / 'a'
        original.write_bytes(body)
        os.link(original, root / 'cur' / 'b')
        (root / 'new' / 'c').write_bytes(body)
        mapping, stats = self.classified([root])
        self.assertEqual(mapping, exhaustive_survivors([root]))
        self.assertEqual(stats['proof-comparisons'], 2)
        self.assertEqual(stats['proof-matches'], 2)
        self.assertEqual(stats['proof-inode-matches'], 1)
        self.assertEqual(stats['proof-bytes'], 2 * len(body))

    def test_large_proof_bucket_with_small_sqlite_cache(self):
        root = self.folder('proof-spill')
        data, count = b'raw\0\xff\n', 2048
        for index in reversed(range(count)):
            (root / 'cur' / f'{index:04d}').write_bytes(data)
        mapping, stats = self.classified([root], '--test-cache-kib', '16',
                                        '--test-heap-kib', '8192')
        self.assertEqual(mapping, exhaustive_survivors([root]))
        self.assertEqual(stats['proof-buckets'], 1)
        self.assertEqual(stats['proof-comparisons'], count - 1)
        self.assertEqual(stats['proof-matches'], count - 1)
        self.assertEqual(stats['proof-bytes'], 2 * (count - 1) * len(data))

    def test_stale_keeper_after_reduction_cannot_prove_either_route(self):
        for copies in (2, 3):
            with self.subTest(copies=copies):
                root = self.folder(f'stale-{copies}')
                files = [root / 'cur' / str(index) for index in range(copies)]
                for path in files:
                    path.write_bytes(b'unchanged bytes')
                process = subprocess.Popen(self.command([root], '--test-hold-after-reduction'),
                                           env=self.env, stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    ready, _, _ = select.select([process.stderr], [], [], 10)
                    self.assertTrue(ready, 'candidate reduction must complete before replacement')
                    self.assertEqual(process.stderr.readline(), b'candidates-ready\n')
                    files[0].rename(root / 'tmp' / 'old-keeper')
                    files[0].write_bytes(b'unchanged bytes')
                    out, err = process.communicate(b'x', timeout=10)
                    self.assertEqual(process.returncode, 1, err)
                    self.assertEqual(out, b'')
                    self.assertIn(b'message changed since inventory', err)
                    self.assertNotIn(b'dedup candidates:', err)
                    self.assertTrue(all(path.read_bytes() == b'unchanged bytes' for path in files))
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()
                self.assertEqual(list(self.root.glob('jaro-dedup-*')), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
