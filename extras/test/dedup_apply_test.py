"""Exercise verified deletion only on new, private disposable Maildirs."""
from contextlib import contextmanager
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import unittest

import dedup_classify_test as classification
import dedup_inventory_test as inventory


def totals(stderr):
    line, = [line for line in stderr.splitlines() if line.startswith(b'dedup apply: examined=')]
    return {key.decode(): int(value) for item in line.split()[2:]
            for key, value in (item.split(b'='),)}


class Apply(unittest.TestCase):
    folder = inventory.Inventory.folder

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='dedup-apply-',
                                              dir=os.environ.get('JARO_DEDUP_TEST_TMP'))
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.env = dict(os.environ, JAROMAILDIR='')

    def command(self, *roots, options=(), production=False):
        binary = classification.BIN / ('jaro-dedup' if production else 'jaro-dedup-test')
        return [binary, '--apply', '--scratch-dir', self.root, *options, '--', *roots]

    def apply(self, *roots, success=True, **kwargs):
        completed = subprocess.run(self.command(*roots, **kwargs), env=self.env,
                                   capture_output=True, timeout=20)
        self.assertEqual(completed.returncode == 0, success, completed.stderr)
        return completed.stderr

    @contextmanager
    def held(self, stage, *roots):
        options = (('--test-hold-after-reduction',) if stage == 'candidates-ready'
                   else ('--test-apply-hold', stage))
        process = subprocess.Popen(self.command(*roots, options=options), env=self.env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stderr], [], [], 15)
            self.assertTrue(ready, f'never reached {stage}')
            self.assertEqual(process.stderr.readline().strip(), stage.encode())
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)

    def release(self, process, success=True):
        _, err = process.communicate(b'x', timeout=10)
        self.assertEqual(process.returncode == 0, success, err)
        return totals(err)

    def pair(self, root, data=b'A', first='a:2,FS', second='b:2,S'):
        keep, lose = root / 'cur' / first, root / 'new' / second
        keep.write_bytes(data)
        lose.write_bytes(data)
        return keep, lose

    def test_rank_exact_bytes_and_single_pair_pass(self):
        first, second = self.folder('Priority'), self.folder('Later')
        payload = b'non-text\x00\xff\r\n' * 7001
        keep = first / 'new' / 'newline\n\udcff:2,S'
        keep.write_bytes(payload)
        keep.chmod(0o640)
        keep_stat = keep.stat()
        loser = second / 'cur' / 'a'
        loser.write_bytes(payload)
        allocation = loser.stat().st_blocks * 512
        empty = self.pair(first, b'', 'z', 'earlier-name')
        unique = second / 'new' / 'unique'
        unique.write_bytes(b'u')
        err = self.apply(first, second)
        self.assertEqual(totals(err), {'examined': 5, 'groups': 2, 'removed': 2, 'conflicts': 0,
                                      'logical-bytes': len(payload), 'last-link-block-bytes': allocation})
        self.assertEqual(classification.counters(err)['pair-bytes'], 2 * len(payload))
        self.assertEqual((keep.stat().st_ino, keep.stat().st_mode, keep.read_bytes()),
                         (keep_stat.st_ino, keep_stat.st_mode, payload))
        self.assertEqual(empty[0].read_bytes(), b'')
        self.assertFalse(empty[1].exists())
        self.assertFalse(loser.exists())
        self.assertEqual(unique.read_bytes(), b'u')
        self.assertEqual(totals(self.apply(first, second))['removed'], 0)

    def test_multiple_hardlinks_keeper_other_inode_and_external_name(self):
        root = self.folder('links')
        winner = root / 'cur/a'
        winner.write_bytes(b'body')
        os.link(winner, root / 'cur/b')
        os.link(winner, root / 'new/c')
        other = root / 'cur/d'
        other.write_bytes(b'body')
        os.link(other, root / 'new/e')
        external = self.root / 'external'
        os.link(other, external)
        last = root / 'new/f'
        last.write_bytes(b'body')
        blocks = last.stat().st_blocks * 512
        counts = totals(self.apply(root))
        self.assertEqual((counts['removed'], counts['logical-bytes'], counts['last-link-block-bytes']),
                         (5, 20, blocks))
        self.assertEqual(list((root / 'cur').iterdir()), [winner])
        self.assertEqual(list((root / 'new').iterdir()), [])
        self.assertEqual((winner.stat().st_nlink, external.stat().st_nlink), (1, 1))
        self.assertEqual((winner.read_bytes(), external.read_bytes()), (b'body', b'body'))

    def test_collision_keeps_distinct_representatives(self):
        root = self.folder('collisions')
        for name, byte in zip('abcde', b'ABABB'):
            (root / 'cur' / name).write_bytes(bytes([byte]))
        counts = totals(self.apply(root, options=('--test-force-digest',)))
        self.assertEqual((counts['groups'], counts['removed']), (2, 3))
        self.assertEqual({p.name: p.read_bytes() for p in (root / 'cur').iterdir()},
                         {'a': b'A', 'b': b'B'})

    def test_nonblocking_overlap_root_and_physical_subdirectory(self):
        first, second = self.folder('first'), self.folder('second')
        keep, lose = self.pair(first)
        alias = self.root / 'alternate'
        alias.mkdir()
        (alias / 'cur').symlink_to(first / 'new', target_is_directory=True)
        (alias / 'new').mkdir()
        with self.held('apply-ready', first, second) as process:
            for roots in ((second, first), (alias,)):
                err = self.apply(*roots, success=False)
                self.assertIn(b'acquire apply lock', err)
                self.assertEqual(totals(err)['removed'], 0)
            self.assertEqual((keep.read_bytes(), lose.read_bytes()), (b'A', b'A'))
            self.assertEqual(self.release(process)['removed'], 1)
        self.assertEqual(totals(self.apply(alias))['removed'], 0)

    def test_change_before_hash_proof_aborts_before_any_removal(self):
        root = self.folder('proof')
        first = self.pair(root)
        second = self.pair(root, b'BB', 'c', 'd')
        (root / 'cur/e').write_bytes(b'BB')
        with self.held('candidates-ready', root) as process:
            second[1].rename(root / 'cur/moved:2,F')
            counts = self.release(process, success=False)
        self.assertEqual((counts['removed'], counts['conflicts']), (0, 1))
        self.assertTrue(all(p.exists() for p in first))
        self.assertEqual(totals(self.apply(root))['removed'], 3)

    def test_pinned_names_rechecked_for_move_edit_and_symlink(self):
        for side in ('keep', 'lose'):
            for change in ('move', 'edit', 'symlink'):
                with self.subTest(side=side, change=change):
                    root = self.folder(side + change)
                    keep, lose = self.pair(root)
                    path = keep if side == 'keep' else lose
                    other = lose if side == 'keep' else keep
                    with self.held('names-pinned', root) as process:
                        if change == 'edit':
                            path.write_bytes(b'B')
                        else:
                            path.rename(root / 'cur/moved:2,R')
                            if change == 'symlink':
                                path.symlink_to(root / 'cur/moved:2,R')
                        expected = inventory.snapshot(root)
                        _, err = process.communicate(b'x', timeout=10)
                        counts = totals(err)
                    # A same-tick edit can have identical POSIX stat fields.
                    # The one pinned byte comparison must still retain both.
                    if change == 'edit' and counts['conflicts'] == 0:
                        self.assertEqual(process.returncode, 0, err)
                        self.assertEqual(classification.counters(err)['matching-pairs'], 0)
                    else:
                        self.assertNotEqual(process.returncode, 0, err)
                        self.assertEqual(counts['conflicts'], 1)
                    self.assertEqual(counts['removed'], 0)
                    self.assertEqual(inventory.snapshot(root), expected)
                    self.assertEqual(other.read_bytes(), b'A')

    def test_permission_change_reports_conflict(self):
        root = self.folder('access')
        keep, lose = self.pair(root)
        with self.held('apply-ready', root) as process:
            lose.chmod(0)
            try:
                counts = self.release(process, success=False)
            finally:
                lose.chmod(0o600)
        self.assertEqual((counts['removed'], counts['conflicts']), (0, 1))
        self.assertEqual((keep.read_bytes(), lose.read_bytes()), (b'A', b'A'))

    def test_final_check_after_byte_proof_preserves_changed_names(self):
        for side in ('keeper', 'duplicate'):
            with self.subTest(side=side):
                root = self.folder(side)
                keep, lose = self.pair(root)
                path = keep if side == 'keeper' else lose
                with self.held('before-unlink', root) as process:
                    path.rename(root / 'cur/filed:2,S')
                    path.write_bytes(b'replacement')
                    before = inventory.snapshot(root)
                    counts = self.release(process, success=False)
                self.assertEqual((counts['removed'], counts['conflicts']), (0, 1))
                self.assertEqual(inventory.snapshot(root), before)

    def test_rebound_directories_skip_uncertain_mail_and_continue(self):
        for change in ('root', 'new'):
            with self.subTest(change=change):
                first, second = self.folder('a-' + change), self.folder('b-' + change)
                self.pair(first)
                keep, lose = self.pair(second, b'BB')
                with self.held('names-pinned', first, second) as process:
                    path = first if change == 'root' else first / 'new'
                    moved = self.root / ('original-' + change)
                    path.rename(moved)
                    path.mkdir()
                    (path / 'sentinel').write_bytes(b'new binding')
                    expected = inventory.snapshot(moved)
                    counts = self.release(process, success=False)
                self.assertEqual((counts['removed'], counts['conflicts']), (1, 1))
                self.assertEqual(inventory.snapshot(moved), expected)
                self.assertEqual((path / 'sentinel').read_bytes(), b'new binding')
                self.assertTrue(keep.exists())
                self.assertFalse(lose.exists())

    def test_rebound_input_symlink_preserves_original_and_replacement(self):
        original, replacement = self.folder('original'), self.folder('replacement')
        alias = self.root / 'alias'
        alias.symlink_to(original)
        self.pair(original)
        self.pair(replacement, b'other')
        before = inventory.snapshot(original), inventory.snapshot(replacement)
        with self.held('names-pinned', alias) as process:
            alias.unlink()
            alias.symlink_to(replacement)
            counts = self.release(process, success=False)
        self.assertEqual((counts['removed'], counts['conflicts']), (0, 1))
        self.assertEqual((inventory.snapshot(original), inventory.snapshot(replacement)), before)

    def test_external_link_change_after_own_unlink_is_a_conflict(self):
        root = self.folder('external-link')
        winner = root / 'cur/a'
        winner.write_bytes(b'message')
        for name in ('b', 'c'):
            os.link(winner, root / 'cur' / name)
        with self.held('name-removed', root) as process:
            self.assertFalse((root / 'cur/b').exists())
            os.link(winner, self.root / 'added-link')
            counts = self.release(process, success=False)
        self.assertEqual((counts['removed'], counts['conflicts']), (1, 1))
        self.assertTrue((root / 'cur/c').exists())
        self.assertEqual(totals(self.apply(root))['removed'], 1)

    def test_fresh_arrival_deferred(self):
        root = self.folder('delivery')
        keep, lose = self.pair(root)
        with self.held('apply-ready', root) as process:
            arrival = root / 'new/fresh'
            arrival.write_bytes(keep.read_bytes())
            self.assertEqual(self.release(process)['removed'], 1)
        self.assertFalse(lose.exists())
        self.assertTrue(arrival.exists())
        self.assertEqual(totals(self.apply(root))['removed'], 1)

    def test_scratch_failures_stop_and_keep_accurate_partial_count(self):
        root = self.folder('fault')
        winner = root / 'cur/a'
        winner.write_bytes(b'A')
        for name in ('b', 'c'):
            os.link(winner, root / 'cur' / name)
        before = inventory.snapshot(root)
        err = self.apply(root, options=('--test-allocation-failure',), success=False)
        self.assertIn(b'(7)', err)
        self.assertEqual(totals(err)['removed'], 0)
        self.assertEqual(inventory.snapshot(root), before)
        err = self.apply(root, options=('--test-fail-links',), success=False)
        self.assertIn(b'test link write failed', err)
        self.assertEqual((totals(err)['removed'], totals(err)['logical-bytes']), (1, 1))
        self.assertEqual(sorted(p.name for p in (root / 'cur').iterdir()), ['a', 'c'])
        self.assertEqual(list(self.root.glob('jaro-dedup-*')), [])
        self.assertEqual(totals(self.apply(root))['removed'], 1)
        self.assertEqual(winner.read_bytes(), b'A')

    def test_signal_termination_after_unlink_then_retry(self):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            with self.subTest(signal=sig):
                root = self.folder(str(sig))
                self.pair(root)
                self.pair(root, b'BB', 'c', 'd')
                with self.held('name-removed', root) as process:
                    self.assertFalse((root / 'new/b:2,S').exists())
                    process.send_signal(sig)
                    _, err = process.communicate(timeout=10)
                    if sig == signal.SIGTERM:
                        self.assertNotEqual(process.returncode, 0)
                        self.assertEqual(totals(err)['removed'], 1)
                    else:
                        self.assertEqual(process.returncode, -signal.SIGKILL)
                self.assertEqual(totals(self.apply(root))['removed'], 1)
                self.assertEqual(sorted(p.name for p in (root / 'cur').iterdir()), ['a:2,FS', 'c'])
                self.assertEqual(list((root / 'new').iterdir()), [])

    def test_real_distinct_devices(self):
        base = os.environ.get('JARO_DEDUP_CROSS_FS_DIR') or os.environ.get('JARO_FILTER_CROSS_FS_DIR')
        if not base:
            self.skipTest('real second source device required')
        with tempfile.TemporaryDirectory(prefix='dedup-cross-', dir=base) as remote:
            primary, archive = self.folder('primary'), self.folder(Path(remote) / 'archive')
            self.assertNotEqual(primary.stat().st_dev, archive.stat().st_dev)
            winner = primary / 'new/first:2,S'
            loser = archive / 'cur/second'
            winner.write_bytes(b'cross-device')
            loser.write_bytes(winner.read_bytes())
            counts = totals(self.apply(primary, archive, production=True))
            self.assertEqual((counts['removed'], counts['conflicts']), (1, 0))
            self.assertFalse(loser.exists())
            self.assertEqual(winner.read_bytes(), b'cross-device')


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    if not result.wasSuccessful():
        raise SystemExit(1)
    raise SystemExit(77 if result.skipped else 0)
