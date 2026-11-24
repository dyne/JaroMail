"""Exercise the C contact-store boundary and independently inspect SQLite data."""

import argparse
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest


FIXTURES = Path(__file__).resolve().parent / 'fixtures' / 'abook-query'
APPLICATION_ID = 0x4A434E54
ADDRESS_SQL = ('SELECT 1 FROM addresses INDEXED BY addresses_book_mailbox '
               'WHERE book_id=?1 AND mailbox_key=?2 LIMIT 1')


class ContactStoreTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='jaro-contact-store-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.db = self.root / 'contacts.sqlite'
        self.source = self.root / 'source.abook'

    def run_driver(self, *args, status=0):
        result = subprocess.run([self.driver, *args], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, status, result.stderr.decode(errors='replace'))
        return result

    def read_sqlite(self):
        return closing(sqlite3.connect(f'{self.db.as_uri()}?mode=ro', uri=True))

    def test_roundtrip_edit_conflicts_exact_lookup_and_mutt_display(self):
        before = datetime.now(timezone.utc).date().isoformat()
        result = self.run_driver('roundtrip', self.db)
        after = datetime.now(timezone.utc).date().isoformat()
        addresses = ['FIRST@Example.org', '"A,B O\'Neil"@Example.org', 'first@example.org']
        expected_rows = ''.join(f'{address}\tJose\u0301 O’Neil 물\tWorkshop Straße 照 [0m  last\n'
                                for address in addresses).encode()
        self.assertEqual(result.stdout, expected_rows)
        self.assertEqual([row.split(b'\t')[0].decode() for row in result.stdout.splitlines()], addresses)
        self.assertTrue(all(row.count(b'\t') == 2 for row in result.stdout.splitlines()))
        with self.read_sqlite() as db:
            self.assertEqual(db.execute('PRAGMA application_id').fetchone(), (APPLICATION_ID,))
            self.assertEqual(db.execute('PRAGMA user_version').fetchone(), (2,))
            self.assertEqual(db.execute('PRAGMA journal_mode').fetchone(), ('delete',))
            self.assertEqual(db.execute('PRAGMA integrity_check').fetchone(), ('ok',))
            self.assertEqual(db.execute('PRAGMA foreign_key_check').fetchall(), [])
            self.assertEqual(db.execute('SELECT name FROM books ORDER BY id').fetchall(),
                             [('allow',), ('block',), ("allow%'_;--漢字",)])
            data = db.execute('SELECT id,book_id,revision,name,nickname,notes,name_key,nickname_key,notes_key '
                              'FROM contacts ORDER BY book_id,ordinal').fetchall()
            self.assertEqual(len(data), 3)
            self.assertEqual([row[2] for row in data], [2, 1, 1])
            dates = db.execute('SELECT date FROM contacts').fetchall()
            self.assertEqual(len(dates), 3)
            for (value,) in dates:
                self.assertIn(value, (before, after))
            self.assertEqual(data[0][3:], ('Updated Name', 'Étoile', 'Workshop Straße\n照\x1b[0m\x9b\u2028last',
                                           'updatedname', 'etoile', 'workshopstrasse照0mlast'))
            self.assertEqual(db.execute('SELECT spelling,mailbox_key FROM addresses '
                                        'WHERE contact_id=? ORDER BY ordinal', (data[0][0],)).fetchall(),
                             list(zip(addresses, ['first@example.org', '"a,b o\'neil"@example.org',
                                                  'first@example.org'])))
            self.assertEqual(db.execute('SELECT key,value FROM fields WHERE contact_id=? ORDER BY ordinal',
                                        (data[0][0],)).fetchall(),
                             [('custom_漢字', "a'oe%_\t🧪\nline 2"), ('empty', '')])
            plan = db.execute('EXPLAIN QUERY PLAN ' + ADDRESS_SQL, (data[0][1], 'first@example.org')).fetchall()
            self.assertEqual(len(plan), 1, plan)
            self.assertIn('SEARCH addresses USING COVERING INDEX addresses_book_mailbox', plan[0][-1])
            self.assertIn('book_id=? AND mailbox_key=?', plan[0][-1])

    def test_imports_pinned_query_fixture_and_preserves_custom_text(self):
        self.source.write_bytes((FIXTURES / 'contacts.abook').read_bytes() +
                                "custom=⽇本語\tquotes ='\"%_ ☃".encode())
        before = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.run_driver('import', self.db, "allow%'_", self.source)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), before)
        with self.read_sqlite() as db:
            self.assertEqual(db.execute('SELECT name FROM books').fetchone(), ("allow%'_",))
            self.assertEqual(db.execute('SELECT name,nickname,notes FROM contacts ORDER BY ordinal').fetchall(),
                             [("Zoë O'Neil", 'stargazer', 'Workshop liaison'), ('Ada Rivera', 'river', '')])
            self.assertEqual(db.execute('SELECT spelling FROM addresses ORDER BY contact_id,ordinal').fetchall(),
                             [('primary@example.org',), ('second@example.org',), ('ada@example.org',)])
            self.assertEqual(db.execute('SELECT key,value FROM fields').fetchall(),
                             [('custom', "⽇本語\tquotes ='\"%_ ☃")])

    def test_import_preserves_quoted_secondary_and_duplicate_mailboxes(self):
        self.source.write_bytes(
            b'[0]\nname=Quoted\nemail= Cap@Example.org , "Q, \\"R\\""@Example.org,Cap@Example.org\n')
        self.run_driver('import', self.db, 'quoted', self.source)
        with self.read_sqlite() as db:
            self.assertEqual(db.execute('SELECT spelling FROM addresses ORDER BY ordinal').fetchall(),
                             [('Cap@Example.org',), ('"Q, \\"R\\""@Example.org',), ('Cap@Example.org',)])

    def test_import_rejects_fields_with_source_location_and_rolls_back(self):
        hidden = b'private-test-content'
        malformed = b'\xed\xa0\x80'
        cases = [
            (b'name=' + hidden + malformed, b'2 5 1 name'),
            (b'name=Second\nnick=' + hidden + malformed, b'2 6 2 nickname'),
            (b'name=Second\nnotes=' + hidden + malformed, b'2 6 2 notes'),
            (b'name=Second\n' + hidden + malformed + b'=x', b'2 6 2 custom key'),
            (b'name=Second\ncustom=' + hidden + malformed, b'2 6 2 custom value'),
            (b'name=Second\ncustom=' + hidden + b'\0value', b'2 6 2 embedded NUL in field'),
            (b'name=Second\nemail=first@example.org,"bad\tmail"@example.org', b'2 6 2 email'),
        ]
        stage = self.root / '.contacts-staging.sqlite'
        for data, message in cases:
            with self.subTest(message=message):
                original = b'[0]\nname=First\nemail=good@example.org\n[1]\n' + data
                self.source.write_bytes(original)
                result = self.run_driver('import', stage, 'allow', self.source, status=1)
                self.assertEqual(result.stderr, message + b'\n')
                self.assertNotIn(hidden, result.stderr)
                self.assertFalse(self.db.exists())
                self.assertFalse(stage.exists())
                self.assertFalse(stage.with_name(stage.name + '-journal').exists())
                self.assertEqual(self.source.read_bytes(), original)

    def test_permissions_readonly_and_invalid_database_shape(self):
        journal = self.db.with_name(self.db.name + '-journal')
        self.run_driver('permissions', self.db, journal)
        self.assertEqual(self.db.stat().st_mode & 0o777, 0o600)
        self.assertFalse(journal.exists())
        before = self.db.read_bytes()
        self.run_driver('probe', self.db)
        self.assertEqual(self.db.read_bytes(), before)
        self.db.chmod(0o640)
        self.assertIn(b'private regular file', self.run_driver('probe', self.db, status=5).stderr)
        self.db.chmod(0o600)
        for sql, diagnostic in (
            ('PRAGMA user_version=99', b'unsupported contact schema version'),
            ('PRAGMA user_version=2; DROP TRIGGER contact_date_fixed', b'invalid contact insertion date triggers'),
            ('DROP INDEX addresses_book_mailbox', b'invalid contact database schema'),
        ):
            with self.subTest(sql=sql), closing(sqlite3.connect(self.db)) as db:
                db.executescript(sql)
                self.assertIn(diagnostic, self.run_driver('probe', self.db, status=5).stderr)
        for label, data in [('empty', b''), ('text', b'already here')]:
            path = self.root / label
            path.write_bytes(data)
            path.chmod(0o600)
            self.run_driver('probe', path, status=5)
            self.assertEqual(path.read_bytes(), data)
        self.run_driver('probe', self.root / 'missing', status=2)
        self.assertFalse((self.root / 'missing').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    args = parser.parse_args()
    ContactStoreTests.driver = (args.bin_dir / 'contact-store-driver').resolve(strict=True)
    unittest.main(argv=[__file__], verbosity=2)
