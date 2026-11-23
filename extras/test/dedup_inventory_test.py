"""Independent raw-name inventory oracle; fixtures never use real mail."""
import os
import itertools
from pathlib import Path
import resource
import select
import signal
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
DRIVER = Path(os.environ.get('JARO_DEDUP_TEST_BIN_DIR', REPO / 'build/gnu')) / 'dedup-inventory-driver'


def snapshot(root):
    """Snapshot names, modes and bytes without following symlinks or opening FIFOs."""
    result = {}
    for parent, directories, files in os.walk(os.fsencode(root)):
        for name in directories + files:
            path = os.path.join(parent, name)
            info = os.lstat(path)
            value = None
            if stat.S_ISREG(info.st_mode):
                with open(path, 'rb') as stream:
                    value = stream.read()
            elif stat.S_ISLNK(info.st_mode):
                value = os.readlink(path)
            result[os.path.relpath(path, os.fsencode(root))] = (info.st_mode, info.st_ino, value)
    return result


def oracle(roots):
    rows = []
    for rank, root in enumerate(roots):
        for subrank, sub in enumerate((b'cur', b'new')):
            parent = os.path.join(os.fsencode(root), sub)
            for name in os.listdir(parent):
                info = os.lstat(os.path.join(parent, name))
                if stat.S_ISREG(info.st_mode):
                    rows.append((rank, subrank, name, info.st_size, info.st_dev, info.st_ino,
                                 info.st_mtime_ns // 10**9, info.st_mtime_ns % 10**9,
                                 info.st_ctime_ns // 10**9, info.st_ctime_ns % 10**9))
    return sorted(rows)


def parse(data):
    if not data:
        return []
    fields = data.split(b'\0')
    assert fields.pop() == b''
    assert len(fields) % 10 == 0
    return sorted(tuple(value if index == 2 else int(value)
                        for index, value in enumerate(fields[offset:offset + 10]))
                  for offset in range(0, len(fields), 10))


class Inventory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dedup-inventory-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, JAROMAILDIR='')

    def folder(self, name):
        path = self.root / name
        for sub in ('cur', 'new', 'tmp'):
            (path / sub).mkdir(parents=True)
        return path

    def run_driver(self, *args, success=True, **kwargs):
        result = subprocess.run([os.fsencode(DRIVER), *map(os.fsencode, args)],
                                env=self.env, capture_output=True, timeout=20, **kwargs)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        if success:
            self.assertIn(b'inventory complete:', result.stderr)
        else:
            self.assertNotIn(b'inventory complete:', result.stderr)
        return result

    def test_precise_entries_and_unchanged_maildir(self):
        roots = [self.folder('Primary folder'), self.folder('Archive')]
        first = os.fsencode(roots[0])
        names = (b'space name:2,RS', b'new\nline\n', b'non-utf8-\xff', b'.hidden', b'-option')
        for index, name in enumerate(names):
            with open(first + b'/cur/' + name, 'wb') as stream:
                stream.write(b'raw\x00\xff\r\n' * index)
            os.chmod(first + b'/cur/' + name, 0o640)
        os.link(first + b'/cur/' + names[1], first + b'/new/hardlink')
        os.symlink(first + b'/cur/' + names[1], first + b'/new/symlink')
        os.symlink(b'absent', first + b'/new/broken')
        os.mkfifo(first + b'/new/fifo')
        (roots[0] / 'new/nested').mkdir()
        (roots[0] / 'new/nested/ignore').write_bytes(b'nested')
        (roots[0] / 'tmp/ignore').write_bytes(b'unfinished')
        (roots[1] / 'new/other').write_bytes(b'other')
        before = snapshot(self.root)
        self.assertEqual(parse(self.run_driver(*roots).stdout), oracle(roots))
        self.assertEqual(snapshot(self.root), before)

    def test_preflight_missing_second_root_emits_nothing(self):
        first = self.folder('valid')
        (first / 'cur/message').write_bytes(b'body')
        before = snapshot(self.root)
        result = self.run_driver(first, self.root / 'absent', success=False)
        self.assertEqual(result.stdout, b'')
        self.assertEqual(snapshot(self.root), before)

    def test_aliases_duplicate_paths_preserve_first_rank(self):
        first, second = self.folder('one'), self.folder('two')
        (first / 'new/a').write_bytes(b'a')
        (second / 'cur/b').write_bytes(b'b')
        alias = self.root / 'alias'
        alias.symlink_to(first, target_is_directory=True)
        rows = parse(self.run_driver(alias, first, alias, second).stdout)
        expected = oracle([first, second])
        expected = [(3 if row[0] == 1 else 0, *row[1:]) for row in expected]
        self.assertEqual(rows, expected)

    def test_full_child_aliases_collapse_partial_and_reversed_fail(self):
        first = self.folder('one')
        (first / 'cur/a').write_bytes(b'a')
        alias = self.root / 'other'
        alias.mkdir()
        (alias / 'cur').symlink_to(first / 'cur')
        (alias / 'new').symlink_to(first / 'new')
        self.assertEqual(parse(self.run_driver(first, alias).stdout), oracle([first]))
        (alias / 'new').unlink()
        (alias / 'new').mkdir()
        self.assertEqual(self.run_driver(first, alias, success=False).stdout, b'')
        (alias / 'new').rmdir()
        (alias / 'cur').unlink()
        (alias / 'cur').symlink_to(first / 'new')
        (alias / 'new').symlink_to(first / 'cur')
        self.assertEqual(self.run_driver(first, alias, success=False).stdout, b'')

    def test_same_cur_and_new_rejected(self):
        first = self.folder('one')
        (first / 'new').rmdir()
        (first / 'new').symlink_to(first / 'cur')
        self.assertEqual(self.run_driver(first, success=False).stdout, b'')

    def test_alias_root_overlap_rejected_in_every_input_order(self):
        first, alias, third = (self.folder(name) for name in ('one', 'alias', 'third'))
        for sub in ('cur', 'new'):
            (alias / sub).rmdir()
            (alias / sub).symlink_to(first / sub)
        (third / 'cur').rmdir()
        (third / 'cur').symlink_to(alias)
        for roots in itertools.permutations((first, alias, third)):
            with self.subTest(roots=roots):
                self.assertEqual(self.run_driver(*roots, success=False).stdout, b'')

    def test_empty_and_invalid_inputs(self):
        empty = self.folder('empty')
        self.assertEqual(self.run_driver(empty).stdout, b'')
        for args in ((), ('',), ('--bad-option',), ('--mail-root',), (self.root,)):
            with self.subTest(args=args):
                self.assertEqual(self.run_driver(*args, success=False).stdout, b'')

    def test_configured_root_and_explicit_path_precedence(self):
        configured = self.root / 'configured'
        configured.mkdir()
        selected = self.folder('configured/Inbox')
        local = self.folder('Inbox')
        (selected / 'cur/configured').write_bytes(b'configured')
        (local / 'cur/local').write_bytes(b'local')
        self.env['JAROMAILDIR'] = str(configured)
        self.assertEqual(parse(self.run_driver('Inbox', cwd=self.root).stdout), oracle([selected]))
        self.assertEqual(parse(self.run_driver('./Inbox', cwd=self.root).stdout), oracle([local]))
        self.assertEqual(parse(self.run_driver('--mail-root', self.root, 'Inbox', cwd=self.root).stdout), oracle([local]))
        self.assertEqual(self.run_driver('missing', cwd=self.root, success=False).stdout, b'')
        dash = self.folder('-folder')
        self.assertEqual(self.run_driver('--mail-root', self.root, '--', '-folder').stdout, b'')

    def test_descriptors_bounded_across_many_roots(self):
        roots = [self.folder(f'root-{index}') for index in range(128)]
        for index, root in enumerate(roots):
            (root / 'new' / str(index)).write_bytes(bytes([index]))

        def lower_limit():
            resource.setrlimit(resource.RLIMIT_NOFILE, (24, 24))

        self.assertEqual(parse(self.run_driver(*roots, preexec_fn=lower_limit).stdout), oracle(roots))

    def test_large_directory_and_long_binary_names_stream(self):
        root = self.folder('many')
        parent = os.fsencode(root / 'new')
        for index in range(8192):
            name = str(index).encode().zfill(6) + b'-' + b'\xff' * 230
            with open(os.path.join(parent, name), 'wb') as stream:
                stream.write(b'x' * (index % 7))
        self.assertEqual(parse(self.run_driver(root).stdout), oracle([root]))

    def test_directory_changed_after_preflight_is_incomplete(self):
        for replace_root in (False, True):
            with self.subTest(replace_root=replace_root):
                root = self.folder('root-' + str(replace_root))
                process = subprocess.Popen([str(DRIVER), '--pause', str(root)], env=self.env,
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    ready, _, _ = select.select([process.stderr], [], [], 5)
                    self.assertTrue(ready, 'bounded preflight readiness')
                    self.assertEqual(process.stderr.readline(), b'preflight complete\n')
                    target = root if replace_root else root / 'cur'
                    target.rename(str(target) + '.moved')
                    target.mkdir()
                    if replace_root:
                        (target / 'cur').mkdir()
                        (target / 'new').mkdir()
                    out, err = process.communicate(b'x', timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertEqual(out, b'')
                    self.assertIn(b'directory binding changed', err)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()

    def test_output_failure_is_incomplete(self):
        root = self.folder('mail')
        (root / 'cur/a').write_bytes(b'body')
        def fail_writes():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

        with open(self.root / 'full-output', 'wb') as destination:
            result = subprocess.run([str(DRIVER), str(root)], env=self.env,
                                    stdout=destination, stderr=subprocess.PIPE, timeout=5,
                                    preexec_fn=fail_writes)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(b'inventory complete:', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
