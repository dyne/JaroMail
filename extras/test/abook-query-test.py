"""Compare a real abook executable with the recorded 0.6.2 Mutt query protocol."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BOOK = Path(__file__).resolve().parent / 'fixtures/abook-query/contacts.abook'
PRIMARY = "primary@example.org\tZoë O'Neil\tWorkshop liaison\n".encode('utf-8')
SECONDARY = "second@example.org\tZoë O'Neil\tWorkshop liaison\n".encode('utf-8')
ADA = b'ada@example.org\tAda Rivera\t \n'
ZOE = PRIMARY + SECONDARY
ALL_ROWS = ZOE + ADA
FIRST_ONLY = b'set mutt_return_all_emails=false\n'
ALL_EMAILS = b'set mutt_return_all_emails=true\n'


class QueryProtocolTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='jaro-abook-query-')
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name)
        self.maildir = self.home / 'Maildir'
        for subdir in ('cur', 'new', 'tmp'):
            (self.maildir / subdir).mkdir(parents=True)
        self.books = {
            'contacts': BOOK.read_bytes(),
            'empty': b'',
            'header-only': b'[format]\nprogram=abook\nversion=0.6.2\n',
            'missing': None,
        }
        for name, data in self.books.items():
            if data is not None:
                (self.maildir / f'{name}.abook').write_bytes(data)
        self.config = self.home / 'abookrc'

    def check_query(self, locale, label, query, stdout, *, status=0, book='contacts', config=b''):
        with self.subTest(locale=locale, case=label):
            datafile = self.maildir / f'{book}.abook'
            self.config.write_bytes(config)
            result = subprocess.run(
                [self.binary, '--config', self.config, '--datafile', datafile,
                 '--mutt-query', query.encode('utf-8')],
                stdin=subprocess.DEVNULL, capture_output=True, timeout=10, cwd=self.maildir,
                env=dict(os.environ, HOME=str(self.home), MAILDIR=str(self.maildir),
                         LC_ALL=locale, LANGUAGE='C'),
            )
            original = self.books[book]
            if original is None:
                self.assertFalse(datafile.exists(), 'query created the missing address book')
            else:
                self.assertEqual(datafile.read_bytes(), original)
            self.assertEqual(self.config.read_bytes(), config)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (status, stdout, b''))

    def check_protocol(self, locale):
        # The first line is status, even when it contains only a newline.
        for label, query, rows in (
            ('name with spaces and apostrophe', "zoë o'neil", ZOE),
            ('secondary address', 'second@example.org', ZOE),
            ('nickname', 'stargazer', ZOE),
            ('notes', 'SHOP LIA', ZOE),
            ('file and address order', 'example.org', ALL_ROWS),
            ('empty query', '', ALL_ROWS),
        ):
            self.check_query(locale, label, query, b'\n' + rows)
        self.check_query(locale, 'literal all', 'all', b'All items\n' + ALL_ROWS)
        self.check_query(locale, 'no match', 'no-result', b'Not found\n', status=1)
        self.check_query(locale, 'unaccented query', 'Zoe', b'Not found\n', status=1)
        for book in ('empty', 'header-only', 'missing'):
            self.check_query(locale, book, 'all', b'Cannot open database\n', status=1, book=book)

        self.check_query(locale, 'first address on secondary match', 'second@example.org',
                         b'\n' + PRIMARY, config=FIRST_ONLY)
        self.check_query(locale, 'first address per contact for all', 'all',
                         b'All items\n' + PRIMARY + ADA, config=FIRST_ONLY)
        self.check_query(locale, 'all addresses on secondary match', 'second@example.org',
                         b'\n' + ZOE, config=ALL_EMAILS)
        self.check_query(locale, 'all addresses per contact for all', 'all',
                         b'All items\n' + ALL_ROWS, config=ALL_EMAILS)

    def test_c_locale(self):
        self.check_protocol('C')

    def test_utf8_locale(self):
        available = subprocess.run(['locale', '-a'], check=True, capture_output=True,
                                   text=True, timeout=10).stdout.splitlines()
        utf8 = next((name for name in available if name.lower().replace('-', '') == 'c.utf8'), None)
        if utf8 is None:
            utf8 = next((name for name in available if name.lower().replace('-', '').endswith('utf8')), None)
        self.assertIsNotNone(utf8, 'the query protocol test requires an installed UTF-8 locale')
        self.check_protocol(utf8)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    QueryProtocolTests.binary = parser.parse_args().binary.resolve(strict=True)
    unittest.main(argv=[__file__], verbosity=2)
