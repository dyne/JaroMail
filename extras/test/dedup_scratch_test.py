"""Bounded SQLite scratch storage and failure contracts on disposable mail."""
from contextlib import contextmanager
import os
from pathlib import Path
import resource
import re
import select
import signal
import sqlite3
import stat
import subprocess
import unittest

import dedup_inventory_test as inventory

REPO = inventory.REPO
oracle, parse, snapshot = inventory.oracle, inventory.parse, inventory.snapshot

BIN = Path(os.environ.get('JARO_DEDUP_TEST_BIN_DIR', REPO / 'build/gnu'))
HELPER = BIN / 'jaro-dedup-test'
PRODUCTION = BIN / 'jaro-dedup'


class Scratch(unittest.TestCase):
    setUp = inventory.Inventory.setUp
    folder = inventory.Inventory.folder

    def run_driver(self, *args, success=True, production=False, **kwargs):
        binary = PRODUCTION if production else HELPER
        command = [os.fsencode(binary), b'--inventory', b'--scratch-dir', os.fsencode(self.root)]
        if not production:
            command.append(b'--test-dump')
        result = subprocess.run(command + list(map(os.fsencode, args)), env=self.env,
                                capture_output=True, timeout=20, **kwargs)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        if success:
            self.assertIn(b'inventory complete:', result.stderr)
        else:
            self.assertNotIn(b'inventory complete:', result.stderr)
        self.assertEqual(list(self.root.glob('jaro-dedup-*')), [], 'normal failure/success cleans owned scratch')
        return result

    test_precise_entries_and_unchanged_maildir = inventory.Inventory.test_precise_entries_and_unchanged_maildir
    test_aliases_duplicate_paths_preserve_first_rank = inventory.Inventory.test_aliases_duplicate_paths_preserve_first_rank
    test_long_binary_names_and_many_groups = inventory.Inventory.test_large_directory_and_long_binary_names_stream

    @contextmanager
    def held(self, root, *options):
        command = [str(HELPER), '--inventory', '--scratch-dir', str(self.root), '--test-hold', *options, '--', str(root)]
        process = subprocess.Popen(command, env=self.env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready, _, _ = select.select([process.stderr], [], [], 15)
            self.assertTrue(ready, 'bounded scratch readiness')
            line = process.stderr.readline()
            self.assertTrue(line.startswith(b'scratch-ready: '), line)
            scratch = Path(os.fsdecode(line[len(b'scratch-ready: '):].rstrip(b'\n')))
            yield process, scratch
            out, err = process.communicate(b'x', timeout=10)
            self.assertEqual(process.returncode, 0, err)
            self.assertFalse(scratch.exists())
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def many(self, count=8192):
        root = self.folder('many')
        parent = os.fsencode(root / 'new')
        for index in range(count):
            name = str(index).encode().zfill(6) + b'-' + b'\xff' * 230
            with open(os.path.join(parent, name), 'wb') as stream:
                stream.write(b'x' * (index % 31))
        return root

    def test_small_cache_private_disk_database_and_cursor_index(self):
        root = self.many()
        before = snapshot(root)
        with self.held(root, '--test-cache-kib', '16', '--test-heap-kib', '8192', '--test-temp-probe') as (process, scratch):
            self.assertEqual(stat.S_IMODE(scratch.stat().st_mode), 0o700)
            database = scratch / 'index.sqlite'
            self.assertEqual(stat.S_IMODE(database.stat().st_mode), 0o600)
            self.assertGreater(database.stat().st_size, 16 * 1024)
            with sqlite3.connect(database) as db:
                self.assertEqual(db.execute('SELECT complete FROM status').fetchone(), (1,))
                self.assertEqual(db.execute('SELECT count(*) FROM occurrences').fetchone(), (8192,))
                self.assertEqual(db.execute('SELECT count(*) FROM occurrences WHERE typeof(name)="blob"').fetchone(), (8192,))
                rows = db.execute('SELECT rank,subdir,name,size,device,inode,mtime_sec,mtime_nsec,ctime_sec,ctime_nsec FROM occurrences').fetchall()
                self.assertEqual(sorted(tuple(int(v) if i in (4, 5) else v for i, v in enumerate(row)) for row in rows), oracle([root]))
                plan = db.execute('EXPLAIN QUERY PLAN SELECT name FROM occurrences ORDER BY size,rank,subdir,name').fetchall()
                self.assertTrue(any('occurrence_size' in row[3] for row in plan), plan)
                self.assertFalse(any('TEMP B-TREE' in row[3] for row in plan), plan)
            # Linux exposes unlinked SQLite temp files; other POSIX hosts still
            # check configuration in C and all visible side files here.
            fd_root = Path(f'/proc/{process.pid}/fd')
            if fd_root.exists():
                temporary = [os.readlink(fd) for fd in fd_root.iterdir()]
                temporary = [name for name in temporary if name.startswith(str(scratch) + '/') and name.endswith(' (deleted)')]
                self.assertTrue(temporary, 'disk temp tables must create an observable temp file')
                self.assertTrue(all(name.startswith(str(scratch) + '/') for name in temporary), temporary)
            self.assertTrue(all(stat.S_IMODE(path.stat().st_mode) == 0o600 for path in scratch.iterdir()))
        self.assertEqual(snapshot(root), before)

    def test_real_sqlite_allocation_failure_leaves_mail_unchanged(self):
        root = self.many(4)
        before = snapshot(root)
        result = self.run_driver('--test-allocation-failure', root, success=False)
        self.assertIn(b'(7)', result.stderr, 'SQLITE_NOMEM')
        self.assertEqual(snapshot(root), before)

    def test_bounded_batches_and_cursor_size_priority_order(self):
        root = self.many(2200)
        result = self.run_driver('--test-cache-kib', '16', '--test-heap-kib', '8192', root)
        self.assertIn(b'rows=2200 batches=3 ', result.stderr)
        peak = int(re.search(rb'sqlite-peak-bytes=(\d+)', result.stderr)[1])
        self.assertLessEqual(peak, 8192 * 1024)
        fields = result.stdout.split(b'\0')[:-1]
        actual_order = [(int(fields[i + 3]), int(fields[i]), int(fields[i + 1]), fields[i + 2])
                        for i in range(0, len(fields), 10)]
        self.assertEqual(actual_order, sorted(actual_order))
        self.assertEqual(parse(result.stdout), oracle([root]))

    def test_sqlite_full_at_small_page_limit_leaves_mail_unchanged(self):
        root = self.many(2048)
        before = snapshot(root)
        result = self.run_driver('--test-max-pages', '8', root, success=False)
        self.assertIn(b'(13)', result.stderr, 'SQLITE_FULL')
        self.assertEqual(snapshot(root), before)

    def test_os_scratch_write_failure_is_incomplete_and_clean(self):
        root = self.many(2048)
        before = snapshot(root)

        def bound_file_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (65536, 65536))

        result = self.run_driver(root, success=False, preexec_fn=bound_file_size)
        self.assertIn(b'dedup scratch:', result.stderr)
        self.assertEqual(snapshot(root), before)

    def test_empty_inventory_and_production_entry(self):
        root = self.folder('empty')
        self.assertEqual(self.run_driver(root).stdout, b'')
        self.assertEqual(self.run_driver(root, production=True).stdout, b'')
        self.run_driver('--test-dump', root, production=True, success=False)
        result = subprocess.run([str(PRODUCTION), str(root)], capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)

    def test_scratch_parent_errors_and_invalid_limits(self):
        root = self.folder('mail')
        for parent in (self.root / 'missing', root / 'cur/file'):
            (root / 'cur/file').write_bytes(b'body')
            self.run_driver('--scratch-dir', parent, root, success=False)
        for options in (('--test-cache-kib', '0'), ('--test-cache-kib', '900000'),
                        ('--test-cache-kib', '128', '--test-heap-kib', '64')):
            self.run_driver(*options, root, success=False)

    def test_large_sparse_size_without_payload_reads(self):
        root = self.folder('sparse')
        with (root / 'cur/over-4-GiB').open('wb') as stream:
            stream.truncate(2**32 + 17)
        self.assertEqual(parse(self.run_driver(root).stdout), oracle([root]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
