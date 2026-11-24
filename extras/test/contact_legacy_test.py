"""Check the bounded native address-book reader and legacy query entry point."""

import argparse
import errno
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import unittest


LINE_LIMIT = 64 * 1024
SOURCE_LIMIT = 64 * 1024 * 1024


class LegacyBookTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='jaro-legacy-book-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def book(self, data, name='contacts.abook'):
        path = self.root / name
        path.write_bytes(data)
        return path

    def read(self, *args, **kwargs):
        return subprocess.run([self.driver, *args], capture_output=True, timeout=15, **kwargs)

    def test_order_custom_bytes_and_unterminated_final_field(self):
        custom = '漢字\t= Zoë 💌 '.encode()
        book = self.book(b' [format] \r\n program = abook\r\nversion=0.6.2\r\n'
                         b'[7]\r\nname=One\r\ncustom_field=' + custom + b'\r\n'
                         b'empty=\r\n[2]\nname=Two\nnotes=end\r')
        result = self.read('--dump', book)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            b'record 1 4 3', b'field 5 4 3 6e616d65 4f6e65',
            f'field 6 12 {len(custom)} 637573746f6d5f6669656c64 {custom.hex()}'.encode(),
            b'field 7 5 0 656d707479 ', b'record 2 8 2',
            b'field 9 4 3 6e616d65 54776f', b'field 10 5 4 6e6f746573 656e640d',
        ])

    def test_multiple_books_under_descriptor_limit(self):
        paths = [self.book(b'[0]\nname=One', f'{index}.abook') for index in range(64)]
        directory = self.root / 'directory.abook'
        directory.mkdir()
        fifo = self.root / 'fifo.abook'
        os.mkfifo(fifo)
        invalid = self.book(b'[0]\nsecret:bad', 'invalid.abook')

        def cap_descriptors():
            _, ceiling = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (16, ceiling))

        inputs = [arg for path in paths for arg in (directory, fifo, invalid, path)]
        result = self.read(*inputs, preexec_fn=cap_descriptors)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(result.stdout.count(b'record 1 1 1\n'), 64)
        self.assertEqual(result.stderr.count(b'expected a regular source file\n'), 128)
        self.assertEqual(result.stderr.count(b'expected a field assignment\n'), 64)

    def test_read_error_is_not_eof_or_a_complete_record(self):
        result = self.read('--read-failure')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b'record 1 1 1\nfield 2 4 5\n')
        self.assertEqual(result.stderr, f'1 5 2 2 {errno.EAGAIN} cannot read source\n'.encode())

    def test_line_and_field_limits_are_inclusive(self):
        prefix = b'[0]\nname=Name\nnotes='
        value = b'x' * (LINE_LIMIT - len(b'notes='))
        for ending in (b'', b'\n'):
            with self.subTest(ending=ending):
                result = self.read(self.book(prefix + value + ending))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.endswith(f'field 3 5 {len(value)}\n'.encode()))
        result = self.read(self.book(prefix + value + b'x'))
        self.assertEqual((result.returncode, result.stdout), (2, b''))
        self.assertEqual(result.stderr, b'2 3 1 2 0 source or line byte limit exceeded\n')
        fields = b''.join(f'f{i}=\n'.encode() for i in range(127))
        result = self.read(self.book(b'[0]\nname=Name\n' + fields))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'record 1 1 128\n'))
        result = self.read(self.book(b'[0]\nname=Name\n' + fields + b'extra=\n'))
        self.assertEqual((result.returncode, result.stdout), (2, b''))
        self.assertEqual(result.stderr, b'2 130 1 129 0 contact field count limit exceeded\n')

    def test_source_limit_is_inclusive(self):
        path = self.book(b'[0]\nname=Limit\n')
        with path.open('ab') as stream:
            remaining = SOURCE_LIMIT - stream.tell()
            comment = b'#' + b'x' * (LINE_LIMIT - 2) + b'\n'
            complete, tail = divmod(remaining, len(comment))
            for _ in range(complete):
                stream.write(comment)
            stream.write(comment[:tail])
        result = self.read(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith(b'record 1 1 1\n'))
        with path.open('ab') as stream:
            stream.write(b'x')
        result = self.read(path)
        self.assertEqual((result.returncode, result.stdout), (2, b''))

    def test_reports_unsupported_records_without_private_data(self):
        for value, location, reason in (
            (b'[private-section]\n', b'1 1 0', b'unsupported section'),
            (b'[0]\nnotes=private\n', b'1 1 0', b'missing name field'),
            (b'[0]\nname=\n', b'2 1 1', b'empty name field'),
            (b'[0]\nname=private\ncontinued', b'3 1 2', b'expected a field assignment'),
            (b'[0]\nname=private\nname=again', b'3 1 2', b'duplicate field'),
            (b'[0]\nname=private\x00tail', b'2 1 1', b'embedded NUL in field'),
        ):
            with self.subTest(reason=reason):
                result = self.read(self.book(value))
                self.assertEqual((result.returncode, result.stdout), (3, b''))
                self.assertEqual(result.stderr, b'3 ' + location + b' 0 ' + reason + b'\n')

    def abook_call(self, path, *args, **kwargs):
        return subprocess.run(
            [self.abook, '--config', self.book(b'', 'abookrc'), '--datafile', path, *args],
            capture_output=True, timeout=10, env=dict(os.environ, HOME=str(self.root), LC_ALL='C'),
            **kwargs)

    def test_legacy_query_reads_final_unterminated_line(self):
        original = b'[0]\nname=Last Contact\nemail=final@example.org'
        path = self.book(original)
        result = self.abook_call(path, '--mutt-query', 'Last')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'\nfinal@example.org\tLast Contact\t \n')
        self.assertEqual(path.read_bytes(), original)

    def test_legacy_add_cannot_replace_failed_read(self):
        original = b'[0]\nname=First\nemail=first@example.org\n[1]\nname=Bad\x00tail\n'
        path = self.book(original)
        result = self.abook_call(path, '--add-email-quiet',
                                 input=b'From: New Contact <new@example.org>\n\nSynthetic message.\n')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(path.read_bytes(), original)
        self.assertFalse(path.with_suffix('.abook~').exists())
        self.assertFalse(path.with_suffix('.abook.new').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    args = parser.parse_args()
    LegacyBookTests.driver = (args.bin_dir / 'contact-legacy-driver').resolve(strict=True)
    LegacyBookTests.abook = (args.bin_dir / 'abook').resolve(strict=True)
    unittest.main(argv=[__file__], verbosity=2)
