"""Characterize the installed JaroMail query consumers using synthetic mail."""

import argparse
from configparser import ConfigParser
from contextlib import closing, contextmanager, suppress
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import sqlite3
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

sys.dont_write_bytecode = True
from abook_terminal import Terminal
from filter_compiler_test import records


BOOK_HEADER = b'[format]\nprogram=abook\nversion=0.6.2\n'
BOOKS = {
    'whitelist': BOOK_HEADER + b'''
[0]
name=Pat Contact
email=pat@example.org
notes=Contact note

[1]
name=Mixed Zed
email=zed@example.org

[2]
name=Mixed Ada
email=ada@example.org

[3]
name=Quoted Person
email="a b"@example.org
notes=ghost@example.org

[4]
name=Membership Twin
email=first.member@example.org,Upper.Member@example.org,"a,b"@example.org
notes=ghost-member@example.org

[5]
name=Anne-Marie O'Neil
email=anne@example.org
''',
    'alternate': BOOK_HEADER + b'\n[0]\nname=Alternate Person\nemail=alternate@example.org\n',
    'blacklist': BOOK_HEADER + b'\n[0]\nname=Block Person\nemail=block@example.org\n'
                 b'notes=block-only@fixture.invalid\n'
                 b'\n[1]\nname=Block overlap\nemail=pat@example.org,alternate@example.org\n',
}
GROUP_ROSTER = b'roster@jaromail.group\tGroup roster (2 recipients)\n'
DOCTOR_PRIVATE = (b'doctor.marker@fixture.invalid', b'Doctor private notes marker',
                  b'Doctor private list marker')


def run_client(command, *, cwd, env, keys_after=()):
    """Run queued keystrokes in a terminal and settle the child on every path."""
    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 120, 0, 0))
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=slave, stdout=slave, stderr=slave,
                                   start_new_session=True)
    except BaseException:
        os.close(master)
        raise
    finally:
        os.close(slave)
    output = bytearray()
    steps = iter(keys_after)
    step = next(steps, None)
    offset = 0
    deadline = time.monotonic() + 10
    try:
        while time.monotonic() < deadline:
            if not select.select([master], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            try:
                block = os.read(master, 4096)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not block:
                break
            output.extend(block)
            while step and (index := output.find(step[0], offset)) >= 0:
                needle, keys = step
                offset = index + len(needle)
                if termios.tcgetattr(master)[3] & (termios.ICANON | termios.ECHO):
                    raise AssertionError(f'editor mode was not restored at {needle!r}')
                os.write(master, keys)
                step = next(steps, None)
        if step:
            raise AssertionError(f'did not observe {step[0]!r}: {bytes(output[-1200:])!r}')
        return process.wait(timeout=max(0, deadline - time.monotonic())), bytes(output)
    finally:
        if process.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(master)


class InstalledConsumers(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='jaro-abook-consumers-')
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.home = self.root / 'home'
        self.mail = self.home / 'Maildir'
        self.runtime = self.prefix / 'share/jaromail'
        self.jaro = self.prefix / 'bin/jaro'
        self.mail.mkdir(mode=0o700, parents=True)
        (self.mail / 'Groups').mkdir()
        (self.home / '.abook').mkdir()
        (self.home / '.abook/abookrc').touch()
        (self.mail / 'Filters.txt').write_bytes(b'from-address fixture@example.org move Fixture\n')
        for book, data in BOOKS.items():
            (self.mail / f'{book}.abook').write_bytes(data)
        (self.mail / 'empty.abook').touch()
        (self.mail / 'headerless.abook').write_bytes(
            b'[0]\nname=Legacy Contact\nemail=legacy@example.org\n')
        for group in ('roster', 'mixed'):
            (self.mail / 'Groups' / group).write_bytes(
                b'# members\n\nOne <one@example.org>\nTwo <two@example.org>\n')
        self.env = dict(os.environ, HOME=str(self.home), JAROMAILDIR=str(self.mail),
                        JAROACCOUNT=str(self.mail / 'Accounts'), TMPDIR=str(self.root),
                        LC_ALL='C', LANGUAGE='C', TERM='xterm-256color')
        self.env.pop('JAROWORKDIR', None)

    def capture(self, command, *, env=None, input=None):
        return subprocess.run(command, env=self.env if env is None else env, cwd=self.root,
                              stdin=subprocess.DEVNULL if input is None else None, input=input,
                              capture_output=True, timeout=15)

    @staticmethod
    def profile_tree(profile, omit_contents=()):
        tree = {}
        for path in [profile, *profile.rglob('*')]:
            info = path.lstat()
            data = (os.readlink(path) if stat.S_ISLNK(info.st_mode) else
                    path.read_bytes() if stat.S_ISREG(info.st_mode) and path not in omit_contents else None)
            tree[path.relative_to(profile)] = (info.st_mode, info.st_ino, info.st_mtime_ns, data)
        return tree

    def doctor_fixture(self, *, initialize=True):
        for path in self.mail.glob('*.abook'):
            path.unlink()
        (self.mail / 'Accounts').mkdir()
        (self.mail / 'Accounts/default.txt').write_text(
            'email fixture@example.org\nlogin fixture\nimap imap.example.org\ncert check\n')
        (self.mail / 'whitelist.abook').write_bytes(
            b'[0]\nname=Fixture Contact\nemail=' + DOCTOR_PRIVATE[0] +
            b'\nnotes=' + DOCTOR_PRIVATE[1] + b'\n')
        (self.mail / (DOCTOR_PRIVATE[2].decode() + '.abook')).write_bytes(
            b'[0]\nname=Custom One\nemail=one@example.org\n'
            b'[1]\nname=Custom Two\nemail=two@example.org\n')
        if initialize:
            initialized = self.capture([self.jaro, '-q', 'complete', 'fixture contact'])
            self.assertEqual(initialized.returncode, 0, initialized.stderr)
        return self.mail / 'contacts.sqlite'

    def doctor_output(self, finding, expected, *, structured=True, omit_contents=()):
        before = self.profile_tree(self.mail, omit_contents)
        before_home = {path for path in self.home.iterdir()}
        command = [self.jaro, 'doctor', *(['--json'] if structured else [])]
        result = self.capture(command)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        if finding == 'blocker':
            self.assertEqual(result.returncode, 1)
        if structured:
            report = json.loads(result.stdout)
            self.assertEqual(report['exit_status'], result.returncode)
            checks = [check for check in report['checks'] if check['id'] == 'contacts']
            self.assertEqual(len(checks), 1)
            self.assertEqual(checks[0]['status'], finding)
            self.assertIn(expected, checks[0]['explanation'])
            if finding == 'blocker':
                self.assertTrue(checks[0]['next_action'])
        else:
            self.assertIn(f'{finding} [contacts] {expected}'.encode(), result.stdout)
        for marker in DOCTOR_PRIVATE:
            self.assertNotIn(marker, result.stdout)
            self.assertNotIn(marker, result.stderr)
        self.assertEqual(result.stderr, b'')
        self.assertEqual(before, self.profile_tree(self.mail, omit_contents))
        self.assertEqual(before_home, set(self.home.iterdir()))

    def test_contact_doctor_absent_with_legacy_sources(self):
        database = self.doctor_fixture(initialize=False)
        self.doctor_output('ok', 'Contact store absent')
        self.assertFalse(database.exists())
        self.assertEqual(list(self.mail.glob('contacts.sqlite*')), [])

    def test_contact_doctor_initialized_counts_and_permissions(self):
        self.doctor_fixture()
        expected = 'Contact store initialized; schema=2 lists=2 contacts=3 mode=0600; integrity checks passed.'
        self.doctor_output('ok', expected)
        self.doctor_output('ok', expected, structured=False)

    def test_contact_doctor_locked(self):
        database = self.doctor_fixture()
        original = database.read_bytes()
        with closing(sqlite3.connect(database, isolation_level=None)) as locked:
            locked.execute('BEGIN EXCLUSIVE')
            try:
                self.assertTrue(locked.in_transaction)
                # Closing a second fd for this inode would release the process's lock.
                self.doctor_output('blocker', 'Contact store is locked (mode=0600)',
                                   omit_contents=(database,))
            finally:
                locked.rollback()
        self.assertEqual(database.read_bytes(), original)

    def test_contact_doctor_unreadable(self):
        database = self.doctor_fixture()
        original = database.read_bytes()
        database.chmod(0)
        try:
            self.doctor_output('blocker', 'Contact store is unreadable (mode=0000)',
                               omit_contents=(database,))
        finally:
            database.chmod(0o600)
        self.assertEqual(database.read_bytes(), original)

    def test_contact_doctor_unsafe_permissions(self):
        database = self.doctor_fixture()
        database.chmod(0o644)
        self.doctor_output('blocker', 'Contact store permissions or file type are unsafe (mode=0644)')

    def test_contact_doctor_corrupt_store(self):
        database = self.doctor_fixture()
        database.write_bytes(b'Not SQLite: ' + b' '.join(DOCTOR_PRIVATE))
        self.doctor_output('blocker', 'Contact store is corrupt or invalid (schema=unknown mode=0600)')

    def test_contact_doctor_runs_integrity_check(self):
        database = self.doctor_fixture()
        # Damage an ordinary table page outside the indexed membership path.
        with closing(sqlite3.connect(database)) as db:
            page_size, = db.execute('PRAGMA page_size').fetchone()
            root_page, = db.execute("SELECT rootpage FROM sqlite_schema WHERE name='fields'").fetchone()
        self.assertGreater(root_page, 1)
        with database.open('r+b') as file:
            file.seek((root_page - 1) * page_size)
            file.write(b'\0')
        membership = self.capture([self.runtime / 'bin/jaro-filter-compile', '--has-contact',
                                   self.mail, 'allow', DOCTOR_PRIVATE[0].decode()])
        self.assertEqual(membership.returncode, 0, membership.stderr)
        self.doctor_output('blocker', 'Contact store is corrupt or invalid (schema=2 mode=0600)')

    def test_contact_doctor_unsupported_schema(self):
        database = self.doctor_fixture()
        with closing(sqlite3.connect(database)) as db, db:
            db.execute('PRAGMA user_version=99')
        self.doctor_output('blocker', 'Unsupported contact schema (schema=99 mode=0600)')

    def test_contact_doctor_unsupported_wal_without_creating_sidecars(self):
        database = self.doctor_fixture()
        with closing(sqlite3.connect(database)) as db:
            self.assertEqual(db.execute('PRAGMA journal_mode=WAL').fetchone(), ('wal',))
        self.assertEqual(list(self.mail.glob('contacts.sqlite-*')), [])
        self.doctor_output('blocker', 'Contact store uses unsupported WAL journaling (mode=0600)')
        self.assertEqual(list(self.mail.glob('contacts.sqlite-*')), [])

    def test_contact_dry_runs_leave_fresh_and_existing_profiles_unchanged(self):
        fresh = self.root / 'fresh dry-run profile'
        fresh.mkdir(mode=0o700)
        header = b'From: Dry Run <dry@example.org>\n\nSynthetic body\n'
        addresses = b'Imported <imported@example.org>\n'
        commands = [
            (('-n', '-q', 'learn', 'sender'), header),
            (('-q', 'import', '--dry-run'), addresses),
        ]
        for profile in (fresh, self.mail):
            env = dict(self.env, JAROMAILDIR=str(profile), JAROACCOUNT=str(profile / 'Accounts'))
            before = self.profile_tree(profile)
            for args, data in commands:
                with self.subTest(profile=profile.name, args=args):
                    result = self.capture([self.jaro, *args], env=env, input=data)
                    self.assertEqual((result.returncode, result.stderr), (0, b''))
                    self.assertEqual(self.profile_tree(profile), before)

        real = self.capture([self.jaro, '-q', '-l', '-n', 'learn', 'sender'],
                            env=dict(self.env, JAROMAILDIR=str(fresh)), input=header)
        self.assertEqual((real.returncode, real.stderr), (0, b''))
        self.assertTrue((fresh / 'Filters.txt').is_file())
        with closing(sqlite3.connect(fresh / 'contacts.sqlite')) as db:
            self.assertEqual(db.execute('SELECT name FROM books').fetchall(), [('-n',)])

        for profile in (fresh, self.mail):
            env = dict(self.env, JAROMAILDIR=str(profile), JAROACCOUNT=str(profile / 'Accounts'))
            group = profile / 'Group file'
            group.write_bytes(b'existing@example.org\n')
            before = self.profile_tree(profile)
            rejected = (
                (('-n', '-q', 'abook'), None, b'editor requires a writable session'),
                (('-q', 'import', '--dry-run', '--', str(group)), addresses,
                 b'only supported for contact-list imports'),
            )
            for args, data, diagnostic in rejected:
                result = self.capture([self.jaro, *args], env=env, input=data)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertEqual(self.profile_tree(profile), before)

        before = self.profile_tree(fresh)
        for args, data in commands:
            result = self.capture([self.jaro, *args], env=dict(self.env, JAROMAILDIR=str(fresh)), input=data)
            self.assertEqual((result.returncode, result.stderr), (0, b''))
            self.assertEqual(self.profile_tree(fresh), before)
        committed = self.capture([self.jaro, '-q', 'import'],
                                 env=dict(self.env, JAROMAILDIR=str(fresh)), input=addresses)
        self.assertEqual((committed.returncode, committed.stdout), (0, addresses))
        database = fresh / 'contacts.sqlite'
        before = database.read_bytes()
        group = fresh / 'Group file'
        result = self.capture([self.jaro, '-q', 'import', '--', str(group)],
                              env=dict(self.env, JAROMAILDIR=str(fresh)), input=addresses)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(group.read_bytes(), addresses + b'existing@example.org\n')
        self.assertEqual(database.read_bytes(), before)

    def test_cli_query_records(self):
        cases = (
            ('contact', (), 0, b'\npat@example.org\tPat Contact\tContact note\n'
                               b'legacy@example.org\tLegacy Contact\t \n'),
            ('PAT CONTACT', (), 0, b'\npat@example.org\tPat Contact\tContact note\n'),
            ('roster', (), 0, b'\n' + GROUP_ROSTER),
            ('mixed', (), 0, b'\nzed@example.org\tMixed Zed\t \nada@example.org\tMixed Ada\t \n'
                              b'mixed@jaromail.group\tGroup mixed (2 recipients)\n'),
            ('miss', (), 1, b'Not found\n'),
            ('group/roster', (), 0, b'\n' + GROUP_ROSTER),
            ('group/absent', (), 1, b'Not found\n'),
            ('quoted', (), 0, b'\n"a b"@example.org\tQuoted Person\tghost@example.org\n'),
            ('ghost@example.org', (), 0, b'\n"a b"@example.org\tQuoted Person\tghost@example.org\n'),
            ('alternate', (), 0, b'\nalternate@example.org\tAlternate Person\t \n'),
            ('block', (), 1, b'Not found\n'),
            ('block', ('-l', 'block'), 1, b'Not found\n'),
            ('alternate', ('-l', 'alternate'), 0, b'\nalternate@example.org\tAlternate Person\t \n'),
            ('roster', ('-l', 'empty'), 0, b'\n' + GROUP_ROSTER),
            ('miss', ('-l', 'empty'), 1, b'Not found\n'),

        )
        for query, options, status, stdout in cases:
            with self.subTest(query=query, options=options):
                result = self.capture([self.jaro, '-q', *options, 'complete', query])
                self.assertEqual((result.returncode, result.stdout, result.stderr), (status, stdout, b''))

        for query, stdout in (
            ('contact', b'Pat Contact <pat@example.org>\n'),
            ('quoted', b'Quoted Person <"a b"@example.org>\n'),
        ):
            with self.subTest(command='addr', query=query):
                result = self.capture([self.jaro, '-q', 'addr', query])
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, stdout, b''))

        for book, status, stderr in (
            ('empty', 1, b''), ('missing-list', 1, b''),
        ):
            with self.subTest(command='addr', book=book):
                result = self.capture([self.jaro, '-q', '-l', book, 'addr'])
                self.assertEqual((result.returncode, result.stdout, result.stderr), (status, b'', stderr))

    def test_known_sender_uses_exact_mailbox(self):
        for mailbox, book, status in (
            (b'FIRST.MEMBER@EXAMPLE.ORG', 'allow', 0),
            (b'upper.member@example.org', 'allow', 0),
            (b'"a,b"@example.org', 'allow', 0),
            (b'"a b"@example.org', 'allow', 0),
            (b'ab@example.org', 'allow', 1),
            (b'ghost-member@example.org', 'allow', 1),
            (b'firstmember@example.org', 'allow', 1),
            (b'alternate@example.org', 'allow', 1),
            (b'alternate@example.org', 'alternate', 0),
            (b'legacy@example.org', 'headerless', 0),
            (b'first.member@example.org', 'empty', 1),
            (b'first.member@example.org', 'missing-list', 1),
        ):
            with self.subTest(mailbox=mailbox, book=book):
                result = self.capture([self.jaro, '-q', '-l', book, 'isknown'],
                                      input=b'From: Synthetic Sender <' + mailbox + b'>\n\nFixture body\n')
                self.assertEqual((result.returncode, result.stdout), (status, b''))
                if status == 3:
                    self.assertIn(b'E_IO', result.stderr)
                else:
                    self.assertEqual(result.stderr, b'')
        self.assertEqual((self.mail / 'whitelist.abook').read_bytes(), BOOKS['whitelist'])

    def test_import_duplicate_uses_exact_mailbox(self):
        book = self.mail / 'whitelist.abook'
        for mailbox in (b'upper.member@example.org', b'"a b"@example.org'):
            duplicate = self.capture([self.jaro, '-q', 'import'], input=b'Duplicate <' + mailbox + b'>\n')
            self.assertEqual((duplicate.returncode, duplicate.stdout, duplicate.stderr), (0, b'', b''))
            self.assertEqual(book.read_bytes(), BOOKS['whitelist'])

        item = b'Notes Mailbox <ghost-member@example.org>\n'
        imported = self.capture([self.jaro, '-q', 'import'], input=item)
        self.assertEqual((imported.returncode, imported.stdout, imported.stderr), (0, item, b''))
        self.assertEqual(book.read_bytes(), BOOKS['whitelist'])
        with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db, db:
            self.assertEqual(db.execute('SELECT spelling FROM addresses WHERE spelling=?',
                                        ('ghost-member@example.org',)).fetchone(), ('ghost-member@example.org',))

    def test_storage_failure_differs_from_miss_and_never_falls_back(self):
        self.assertEqual(self.capture([self.jaro, '-q', 'complete', 'miss']).returncode, 1)
        db = self.mail / 'contacts.sqlite'
        db.write_bytes(b'corrupt')
        complete = self.capture([self.jaro, '-q', 'complete', 'roster'])
        self.assertEqual((complete.returncode, complete.stdout), (3, b'Cannot query addressbook\n'))
        self.assertIn(b'E_CONTACT_STORAGE', complete.stderr)
        imported = self.capture([self.jaro, '-q', 'import'], input=b'Never <never@example.org>\n')
        self.assertEqual((imported.returncode, imported.stdout), (3, b''))
        self.assertIn(b'E_CONTACT_STORAGE', imported.stderr)
        self.assertEqual(db.read_bytes(), b'corrupt')
        self.assertEqual((self.mail / 'whitelist.abook').read_bytes(), BOOKS['whitelist'])

    def convert_book(self, data, inform, outform):
        source = self.root / 'conversion-input'
        source.write_bytes(data)
        result = self.capture([self.runtime / 'bin/abook', '--config', '/dev/null',
                               '--convert', '--informat', inform, '--infile', source,
                               '--outformat', outform],
                              env=dict(self.env, LC_ALL='C.UTF-8'))
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        self.assertEqual(result.stderr, b'')
        self.assertEqual(source.read_bytes(), data)
        return result.stdout

    @staticmethod
    def native_records(output):
        book = ConfigParser(interpolation=None, comment_prefixes=('#',), delimiters=('=',))
        book.read_string(output.decode('utf-8'))
        return [dict(book[section]) for section in book.sections() if section.isdecimal()]

    def test_installed_mutt_alias_round_trip(self):
        aliases = ('# Synthetic aliases\n'
                   'alias -group regular zoe Zoë One <First@Example.org>\n'
                   'alias renee Renée Two <Two@Example.org>').encode()
        book = self.convert_book(aliases, 'mutt', 'abook')
        expected = [
            {'name': 'Zoë One', 'email': 'First@Example.org', 'nick': 'zoe', 'groups': 'regular'},
            {'name': 'Renée Two', 'email': 'Two@Example.org', 'nick': 'renee'},
        ]
        self.assertEqual(self.native_records(book), expected)
        aliases = self.convert_book(book, 'abook', 'mutt')
        self.assertEqual(self.native_records(self.convert_book(aliases, 'mutt', 'abook')),
                         expected)

    def test_installed_vcard_round_trip(self):
        card = ('BEGIN:VCARD\r\nVERSION:3.0\r\nN:One;Zoë;;;\r\nFN:Zoë One\r\n'
                'EMAIL:First@Example.org\r\nEMAIL:Second@Example.org\r\n'
                'NICKNAME:Étoile\r\nNOTE:Older note\r\nNOTE:講師 ☃\r\n'
                'X-FIXTURE:Unsupported property\r\nEND:VCARD').encode()
        # Repeated mailboxes keep their order. A scalar keeps its last value.
        # The built-in mapping drops unknown vCard properties.
        expected = [{'name': 'Zoë One', 'email': 'First@Example.org,Second@Example.org',
                     'nick': 'Étoile', 'notes': '講師 ☃'}]
        book = self.convert_book(card, 'vcard', 'abook')
        self.assertEqual(self.native_records(book), expected)
        card = self.convert_book(book, 'abook', 'vcard')
        self.assertEqual(self.native_records(self.convert_book(card, 'vcard', 'abook')),
                         expected)
        # Mutt alias export splits a multi-address contact into separate aliases
        # and cannot represent its note.
        aliases = self.convert_book(book, 'abook', 'mutt')
        self.assertEqual(self.native_records(self.convert_book(aliases, 'mutt', 'abook')), [
            {'name': 'Zoë One', 'email': 'First@Example.org', 'nick': 'Étoile'},
            {'name': 'Zoë One', 'email': 'Second@Example.org',
             'nick': 'Étoile__Second@Example.org'},
        ])

    def test_installed_jaro_import_and_vcard_export(self):
        selected = 'Fixture list'
        env = dict(self.env, LC_ALL='C.UTF-8')
        payload = 'Zoë One <first@example.org>\nRenée Two <two@example.org>'.encode()
        imported = self.capture([self.jaro, '-q', '-l', selected, 'import'],
                                env=env, input=payload)
        self.assertEqual((imported.returncode, imported.stderr, imported.stdout),
                         (0, b'', payload + b'\n'))
        exported = self.capture([self.jaro, '-q', '-l', selected, 'export', 'vcard'],
                                env=env)
        self.assertEqual((exported.returncode, exported.stderr), (0, b''))
        self.assertEqual(self.native_records(self.convert_book(exported.stdout, 'vcard', 'abook')),
                         [{'name': 'Zoë One', 'email': 'first@example.org'},
                          {'name': 'Renée Two', 'email': 'two@example.org'}])

    def test_installed_editor_add_cancel_autosave_and_reopen(self):
        legacy = self.mail / 'whitelist.abook'
        fixture = ('[0]\nname=Zoë One\nemail=First@Example.org,Second@Example.org\n'
                   'nick=Étoile\nnotes=講師 ☃\nfixture=日本語\t= kept').encode()
        env = dict(self.env, LC_ALL='C.UTF-8')
        command = [self.jaro, '-q', 'abook']
        duplicate = fixture + b'\nname=Repeated'
        legacy.write_bytes(duplicate)
        rejected = self.capture([self.jaro, '-q', 'complete', 'zoe'], env=env)
        self.assertEqual(rejected.returncode, 4, rejected.stderr)
        self.assertIn(b'duplicate field', rejected.stderr)
        self.assertFalse((self.mail / 'contacts.sqlite').exists())
        self.assertEqual(legacy.read_bytes(), duplicate)
        legacy.write_bytes(fixture)

        def addresses(name):
            with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db:
                return db.execute('SELECT a.spelling FROM addresses a JOIN contacts c '
                                  'ON a.contact_id=c.id WHERE c.name=? ORDER BY a.ordinal',
                                  (name,)).fetchall()

        with Terminal(command, cwd=self.root, env=env, cols=80, lines=24) as terminal:
            terminal.wait('List: allow', 'Zoë One', 'First@Example.org')
            terminal.send('a')
            terminal.wait('Name:')
            terminal.send('Cancelled Name\x07')
            terminal.wait('List: allow', cursor=lambda screen: not screen.cursor_visible
                          and 'Name:' not in screen.text)
            self.assertNotIn('Cancelled Name', terminal.screen.text)
            terminal.send('a')
            terminal.wait('Name:')
            terminal.send('Renée New\r')
            terminal.wait('< CONTACT >', 'Renée New')
            terminal.send('2')
            terminal.wait('E-mail:')
            terminal.send('Added@Example.org\r')
            terminal.wait('Added@Example.org', 'Unsaved changes')
            self.assertEqual(addresses('Renée New'), [])
            terminal.send('q')
            terminal.wait('List: allow', 'Saved to this list', 'Renée New')
            self.assertEqual(addresses('Renée New'), [('Added@Example.org',)])
            terminal.quit()

        with Terminal(command, cwd=self.root, env=env, cols=80, lines=24) as terminal:
            terminal.wait('List: allow', 'Renée New', 'Zoë One')
            terminal.send('/renee')
            terminal.wait('/ renee', 'Added@Example.org')
            terminal.send('\r1')
            terminal.wait('Name: Renée New')
            terminal.send('\x15Renée Edited\rq')
            terminal.wait('List: allow', 'Renée Edited', 'Saved to this list')
            terminal.send('l')
            terminal.wait('/ renee', 'Renée Edited',
                          cursor=lambda screen: 'Saved to this list' not in screen.text)
            terminal.send('d')
            terminal.wait('Remove 1 contact from this list?')
            terminal.send('y')
            terminal.wait('No matching contacts')
            terminal.quit()

        self.assertEqual(addresses('Renée Edited'), [])
        self.assertEqual(addresses('Zoë One'), [('First@Example.org',), ('Second@Example.org',)])
        with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db:
            self.assertEqual(db.execute('SELECT nickname,notes FROM contacts WHERE name=?',
                                        ('Zoë One',)).fetchone(), ('Étoile', '講師 ☃'))
            self.assertEqual(db.execute('SELECT key,value FROM fields').fetchall(),
                             [('fixture', '日本語\t= kept')])
        self.assertEqual(legacy.read_bytes(), fixture)

    def test_installed_writer_editor_reader_use_profile_across_accounts(self):
        accounts = self.mail / 'Accounts'
        accounts.mkdir()
        for name in ('alice', 'bob'):
            (accounts / name).write_text(f'name {name}\nlogin {name}\nemail {name}@example.org\nhost example.org\n')
        selected = 'block-allow team'
        payload = b'From: Writer Fresh <new.writer@example.org>\n\nBody\n'
        learned = self.capture([self.jaro, '-q', '-a', 'alice', '-l', selected, 'learn'], input=payload)
        self.assertEqual((learned.returncode, learned.stderr), (0, b''))
        completed = self.capture([self.jaro, '-q', '-a', 'bob', 'complete', 'writer fresh'])
        self.assertEqual((completed.returncode, completed.stdout),
                         (0, b'\nnew.writer@example.org\tWriter Fresh\t \n'))
        status, tty = run_client([self.jaro, '-q', '-a', 'bob', '-l', selected, 'abook'], cwd=self.root,
                                 env=self.env, keys_after=[(b'Writer Fresh', b'\r1\x15Writer Updated\rqq')])
        self.assertEqual(status, 0, repr(tty[-1000:]))
        listed = self.capture([self.jaro, '-q', '-a', 'alice', '-l', selected, 'list'])
        self.assertEqual((listed.returncode, listed.stdout),
                         (0, b'Writer Updated <new.writer@example.org>\n'), tty[-2000:])
        exported = self.capture([self.jaro, '-q', '-a', 'alice', '-l', selected, 'export'])
        self.assertEqual(exported.returncode, 0, exported.stderr)
        self.assertIn(b'FN:Writer Updated\r\n', exported.stdout)
        self.assertIn(b'EMAIL:new.writer@example.org', exported.stdout)
        card = self.root / 'second.vcf'
        card.write_text('BEGIN:VCARD\nVERSION:3.0\nFN:From Vcard\n'
                        'EMAIL:imported.vcard@example.org\nEND:VCARD\n')
        status, tty = run_client([self.jaro, '-q', '-a', 'bob', '-l', selected, 'abook'], cwd=self.root,
                                 env=self.env, keys_after=[(b'Writer Updated', b'ihsecond.vcf\rq')])
        self.assertEqual(status, 0, repr(tty[-1000:]))
        imported = self.capture([self.jaro, '-q', '-a', 'alice', '-l', selected, 'addr', 'from vcard'])
        self.assertEqual((imported.returncode, imported.stdout),
                         (0, b'From Vcard <imported.vcard@example.org>\n'), tty[-2000:])
        for path in self.mail.glob('*.abook'):
            path.unlink()
        self.assertEqual(self.capture([self.jaro, '-q', '-a', 'bob', 'complete', 'writer updated']).returncode, 0)
        independent = self.root / 'Independent profile'
        independent.mkdir(mode=0o700)
        result = self.capture([self.jaro, '-q', 'complete', 'writer updated'],
                              env=dict(self.env, JAROMAILDIR=str(independent), JAROACCOUNT=str(independent / 'Accounts')))
        self.assertEqual((result.returncode, result.stdout), (1, b'Not found\n'))
        self.assertNotEqual((self.mail / 'contacts.sqlite').stat().st_ino,
                            (independent / 'contacts.sqlite').stat().st_ino)
        status, tty = run_client([self.jaro, '-q', '-a', 'bob', '-l', selected, 'abook'], cwd=self.root,
                                 env=self.env, keys_after=[(b'Writer Updated', b'd'),
                                                           (b'Remove 1 contact from this list?', b'yq')])
        self.assertEqual(status, 0, repr(tty[-1000:]))
        self.assertEqual(self.capture([self.jaro, '-q', '-l', selected, 'addr', 'writer updated']).returncode, 1)
        self.assertEqual(self.capture([self.jaro, '-q', '-l', selected, 'addr', 'from vcard']).returncode, 0)

    def browse_contacts(self, cols, lines):
        long_mailbox = 'third.' + 'a' * 50 + '@long.long.long.long.example.org'
        setup = (
            ('Unrelated', 'first@example.org'),
            ('José', 'Jose.One@example.org'),
            ('José', f'Jose.Two@example.org,Other.Box@example.org,{long_mailbox}'),
            ('木 Café', 'wide@example.org'),
        )
        (self.mail / 'whitelist.abook').write_bytes(BOOK_HEADER + ''.join(
            f'\n[{i}]\nname={name}\nemail={addresses}\n'
            for i, (name, addresses) in enumerate(setup)).encode())
        (self.mail / 'blacklist.abook').write_bytes(
            BOOK_HEADER + b'\n[0]\nname=Blocked Fixture\nemail=blocked@example.org\n')
        env = dict(self.env, LC_ALL='C.UTF-8')
        screens = os.environ.get('JARO_CONTACT_TUI_SCREENS')

        def snapshot(terminal, name):
            if screens:
                directory = Path(screens)
                directory.mkdir(parents=True, exist_ok=True)
                (directory / f'browse-{cols}x{lines}-{name}.txt').write_text(
                    terminal.screen.capture(), encoding='utf-8')

        def selected(mailbox):
            return lambda screen: any(row.startswith('>') and mailbox in row
                                      for row in screen.text.splitlines())

        def filtering_at_end(screen):
            return screen.cursor_visible and (screen.row, screen.col) == (screen.lines - 1, 9)

        with Terminal([self.jaro, '-q', 'abook'], cwd=self.root, env=env,
                      cols=cols, lines=lines) as terminal:
            terminal.wait('List: allow', '/ filter', 'Tab mailboxes', 'Name', 'Mailbox',
                          cursor=selected('first@example.org'))
            terminal.send('/jose')
            terminal.wait('/ jose', 'Up/Down', 'Jose.One@example.org', 'Jose.Two@example.org',
                          cursor=selected('Jose.One@example.org'))
            self.assertNotIn('first@example.org', terminal.screen.text)
            terminal.send('\x1bOB')
            terminal.wait('/ jose', cursor=lambda screen: selected('Jose.Two@example.org')(screen)
                          and screen.cursor_visible and (screen.row, screen.col) == (lines - 1, 6))
            snapshot(terminal, 'folded-query')
            terminal.send('\t')
            terminal.wait('Mailboxes for José', 'Jose.Two@example.org', 'Other.Box@example.org',
                          'Enter/Esc back')
            snapshot(terminal, 'mailboxes')
            terminal.send('jj')
            terminal.wait('Left/Right scroll')
            terminal.send('\x1bOC' * 8)
            terminal.wait('long.example.org', 'Enter/Esc back')
            terminal.send('\r\r')
            terminal.wait('List: allow', '< CONTACT >', 'Jose.Two@example.org', 'Other.Box@example.org',
                          'Left/Right views')
            terminal.send('q')
            terminal.wait('Esc clear', cursor=selected('Jose.Two@example.org'))
            with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db, db:
                second_id, = db.execute('SELECT contact_id FROM addresses WHERE spelling=?',
                                        ('Jose.Two@example.org',)).fetchone()
                db.execute('UPDATE contacts SET ordinal=99 WHERE id IN '
                           '(SELECT contact_id FROM addresses WHERE spelling=?)', ('Jose.One@example.org',))
            terminal.send('l')
            terminal.wait('/ jose', 'Esc clear', cursor=lambda screen: selected('Jose.Two@example.org')(screen)
                          and screen.text.index('Jose.Two@example.org') < screen.text.index('Jose.One@example.org'))
            snapshot(terminal, 'reloaded-selection')
            terminal.send('j')
            terminal.wait('/ jose', cursor=selected('Jose.One@example.org'))
            terminal.send('k')
            terminal.wait('/ jose', cursor=selected('Jose.Two@example.org'))
            terminal.send('/\x15木 Café')
            terminal.wait('/ 木 Café', 'wide@example.org', cursor=filtering_at_end)
            other = (48, 12) if cols == 80 else (80, 24)
            terminal.resize(*other)
            terminal.wait('List: allow', '/ 木 Café', 'wide@example.org', 'Up/Down', 'l load',
                          cursor=filtering_at_end)
            self.assertTrue(selected('wide@example.org')(terminal.screen))
            snapshot(terminal, 'resized-query')
            terminal.resize(cols, lines)
            terminal.wait('/ 木 Café', 'wide@example.org', 'Up/Down', cursor=filtering_at_end)
            terminal.send('\x15unfindable')
            terminal.wait('/ unfindable', 'No matching contacts', 'Esc clear')
            snapshot(terminal, 'no-match')
            terminal.send('\x1b')
            terminal.wait('/ filter', 'Tab mailboxes', 'first@example.org')
            terminal.send('/otherbox')
            terminal.wait('/ otherbox', 'Jose.Two@example.org', cursor=selected('Jose.Two@example.org'))
            self.assertNotIn('Jose.One@example.org', terminal.screen.text)
            terminal.send('\x1b')
            terminal.wait('/ filter', 'first@example.org')
            terminal.send('o')
            terminal.wait('Contact lists', '>* allow', 'block', 'alternate', 'Enter open')
            snapshot(terminal, 'list-picker')
            terminal.send('jj\r')
            terminal.wait('List: alternate', 'alternate@example.org',
                          cursor=selected('alternate@example.org'))
            terminal.send('o')
            terminal.wait('Contact lists', '>* alternate', 'Enter open')
            terminal.send('k\r')
            terminal.wait('List: block', 'blocked@example.org', 'Tab mailboxes',
                          cursor=selected('blocked@example.org'))
            terminal.send('/blocked\r')
            terminal.wait('List: block', '< CONTACT >', 'Blocked Fixture', 'blocked@example.org')
            terminal.send('1')
            terminal.wait('Name:')
            terminal.send('\x15Blocked Updated\r')
            terminal.wait('< CONTACT >', cursor=lambda screen: screen.text.count('Blocked Updated') == 2)
            snapshot(terminal, 'block-edit')
            terminal.send('qw')
            terminal.wait('List: block', 'Blocked Updated', '/ blocked', cursor=selected('blocked@example.org'))
            terminal.quit()

        with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db:
            self.assertEqual(db.execute('SELECT name FROM contacts WHERE id=?', (second_id,)).fetchone(),
                             ('José',))
        visible = self.capture([self.jaro, '-q', '-l', 'block', 'addr', 'blocked updated'], env=env)
        self.assertEqual((visible.returncode, visible.stdout), (0, b'Blocked Updated <blocked@example.org>\n'))
        completion = self.capture([self.jaro, '-q', 'complete', 'blocked updated'], env=env)
        self.assertEqual((completion.returncode, completion.stdout), (1, b'Not found\n'))

    def test_contact_browse_wide(self):
        self.browse_contacts(80, 24)

    def test_contact_browse_compact(self):
        self.browse_contacts(48, 12)

    def test_contact_browse_remapped_help(self):
        config = self.root / 'test.abookrc'
        config.write_text('remap f = /\nremap b = o\nremap h = ?\n')
        command = [self.runtime / 'bin/abook', '--config', config,
                   '--jaro-profile', self.mail, '--jaro-list', 'allow']
        with Terminal(command, cwd=self.root, env=self.env, cols=48, lines=12) as terminal:
            terminal.wait('List: allow', 'h help', 'f filter', 'b lists')
            terminal.send('h')
            terminal.wait('Contacts help', 'f: filter as you type', 'b: choose list')
            terminal.send('\rfpat')
            terminal.wait('f pat', 'Pat Contact', 'Esc clear')
            terminal.send('\x1b')
            terminal.wait('f filter', 'b lists')
            terminal.send('b')
            terminal.wait('Contact lists', 'Enter open')
            terminal.send('\x1b')
            terminal.wait('f filter', 'b lists')
            terminal.quit()

    def recover_contact_edits(self, cols, lines):
        (self.mail / 'whitelist.abook').write_bytes(BOOK_HEADER + b'''
[0]
name=Shared Fixture
email=shared@example.org

[1]
name=Other Contact
email=other@example.org
''')
        (self.mail / 'blacklist.abook').write_bytes(BOOK_HEADER + b'''
[0]
name=Blocked Copy
email=shared@example.org
''')
        accounts = self.mail / 'Accounts'
        accounts.mkdir()
        for account in ('alice', 'bob'):
            (accounts / account).write_text(
                f'name {account}\nlogin {account}\nemail {account}@example.org\nhost example.org\n')
        env = dict(self.env, LC_ALL='C.UTF-8')
        database = self.mail / 'contacts.sqlite'
        screens = os.environ.get('JARO_CONTACT_TUI_SCREENS')

        def snapshot(terminal, name):
            if screens:
                directory = Path(screens)
                directory.mkdir(parents=True, exist_ok=True)
                (directory / f'recovery-{cols}x{lines}-{name}.txt').write_text(
                    terminal.screen.capture(), encoding='utf-8')

        def session(account):
            return Terminal([self.jaro, '-q', '-a', account, 'abook'],
                            cwd=self.root, env=env, cols=cols, lines=lines)

        def rows():
            with closing(sqlite3.connect(database, timeout=0)) as db:
                return db.execute(
                    'SELECT b.name,c.id,c.revision,c.name,a.spelling FROM contacts c '
                    'JOIN books b ON b.id=c.book_id JOIN addresses a ON a.contact_id=c.id '
                    "WHERE b.name IN ('allow','block') ORDER BY b.name,c.ordinal,a.ordinal").fetchall()

        @contextmanager
        def hold_write_lock():
            with closing(sqlite3.connect(database, timeout=0, isolation_level=None)) as lock:
                lock.execute('BEGIN IMMEDIATE')
                self.assertTrue(lock.in_transaction)
                try:
                    yield
                finally:
                    lock.rollback()
                    self.assertFalse(lock.in_transaction)

        def change_name(terminal, name):
            terminal.send('1')
            terminal.wait('Name:')
            terminal.send('\x15' + name + '\r')
            terminal.wait('Unsaved changes', 'Ctrl+R reload/discard', name)

        def change_mailbox(terminal, old, new):
            terminal.send('2')
            terminal.wait('Mailbox: 1 or n for new')
            terminal.send('1')
            terminal.wait('E-mail: ' + old)
            terminal.send('\x15' + new + '\r')
            terminal.wait('Unsaved changes', 'Ctrl+R reload/discard', new)

        def select_block(terminal):
            terminal.send('o')
            terminal.wait('Contact lists', '>* allow', 'Enter open')
            terminal.send('j\r')

        with session('alice') as alice:
            alice.wait('List: allow', 'Shared Fixture', 'Tab mailboxes')
            base = rows()
            shared, other, blocked = base
            alice.send('\r')
            alice.wait('< CONTACT >', 'shared@example.org')
            change_name(alice, 'Alice Draft')
            with session('bob') as bob:
                bob.wait('List: allow', 'Shared Fixture', 'Tab mailboxes')
                bob.send('\r')
                bob.wait('< CONTACT >', 'shared@example.org')
                change_name(bob, 'Bob Committed')
                bob.send('w')
                bob.wait('Saved to this list', 'Bob Committed', '< CONTACT >')
                committed = [('allow', shared[1], shared[2] + 1, 'Bob Committed', shared[4]),
                             other, blocked]
                self.assertEqual(rows(), committed)

                alice.send('q')
                alice.wait(f'Conflict #{shared[1]}: changed elsewhere', 'Alice Draft',
                           'Unsaved changes', '< CONTACT >', 'Ctrl+R reload/discard')
                snapshot(alice, 'conflict')
                self.assertEqual(rows(), committed)
                alice.send('w\x12')
                alice.wait('Discard draft and reload this list?', 'y discard', 'Alice Draft')
                alice.send('\x1b')
                alice.wait('Conflict #', 'Alice Draft', 'Unsaved changes')
                self.assertEqual(rows(), committed)
                alice.send('\x12y')
                alice.wait('Reloaded this list', 'Bob Committed', '< CONTACT >')
                self.assertNotIn('Unsaved changes', alice.screen.text)
                self.assertEqual(rows(), committed)
                bob.quit('qq')

            change_name(alice, 'Busy Draft')
            # A reload after saving can introduce a custom field while the
            # form still has an undo record using the older field layout.
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('INSERT INTO fields(contact_id,ordinal,key,value) VALUES(?,0,?,?)',
                           (other[1], 'fixture-extra', 'kept'))
                db.execute('UPDATE contacts SET revision=revision+1 WHERE id=?', (other[1],))
            other = (other[0], other[1], other[2] + 1, other[3], other[4])
            committed[1] = other
            with hold_write_lock():
                alice.send('w')
                alice.wait('Contact store is busy. Retry save.', 'Busy Draft',
                           'Unsaved changes', '< CONTACT >')
                snapshot(alice, 'busy')
                self.assertEqual(rows(), committed)
            alice.send('w')
            alice.wait('Saved to this list', 'Busy Draft', '< CONTACT >')
            busy_saved = [('allow', shared[1], shared[2] + 2, 'Busy Draft', shared[4]), other, blocked]
            self.assertEqual(rows(), busy_saved)
            with closing(sqlite3.connect(database)) as db:
                self.assertEqual(db.execute('SELECT value FROM fields WHERE contact_id=? AND key=?',
                                            (other[1], 'fixture-extra')).fetchone(), ('kept',))

            alice.send('1')
            alice.wait('Name: Busy Draft')
            alice.send('\x15\rw')
            alice.wait('Invalid name: a name is required', 'Unsaved changes', 'shared@example.org')
            self.assertNotIn('Busy Draft', alice.screen.text)
            self.assertEqual(rows(), busy_saved)
            change_name(alice, 'Busy Draft')
            change_mailbox(alice, 'shared@example.org', 'broken mailbox')
            alice.send('w')
            alice.wait('Invalid email:', 'broken mailbox', 'Unsaved changes', 'Ctrl+R reload/discard')
            snapshot(alice, 'validation')
            self.assertEqual(rows(), busy_saved)
            change_mailbox(alice, 'broken mailbox', 'Shared@Example.org')
            alice.send('w')
            alice.wait('Saved to this list', 'Shared@Example.org', '< CONTACT >')
            corrected = [('allow', shared[1], shared[2] + 3, 'Busy Draft', 'Shared@Example.org'), other, blocked]
            self.assertEqual(rows(), corrected)
            alice.send('q')
            alice.wait('List: allow', 'Mailbox', 'Busy Draft')

            alice.send('C')
            alice.wait('Remove all 2 contacts from this list?', 'y confirm  Esc cancel', 'List: allow')
            alice.send('\x1b')
            alice.wait('/ filter', 'Busy Draft')
            self.assertEqual(rows(), corrected)
            alice.send('+r')
            alice.wait('Remove 2 contacts from this list?', 'y confirm  Esc cancel')
            snapshot(alice, 'bulk-confirmation')
            alice.send('n-r')
            alice.wait('Remove 1 contact from this list?', 'y confirm  Esc cancel')
            alice.send('\x1b')
            alice.wait('/ filter', 'Busy Draft')
            self.assertEqual(rows(), corrected)
            alice.send('ry')
            alice.wait('Removal pending; save to commit', 'Unsaved changes', 'Other Contact')
            self.assertNotIn('Busy Draft', alice.screen.text)
            self.assertEqual(rows(), corrected)
            with hold_write_lock():
                select_block(alice)
                alice.wait('Contact store is busy. Retry save.', 'List: allow', 'Unsaved changes')
                snapshot(alice, 'blocked-switch')
                self.assertEqual(rows(), corrected)
            select_block(alice)
            alice.wait('List: block', 'Blocked Copy', 'shared@example.org')
            self.assertEqual(rows(), [other, blocked])

            alice.send('ry')
            alice.wait('Removal pending; save to commit', 'Unsaved changes', 'No contacts in this list')
            with hold_write_lock():
                alice.send('q')
                alice.wait('Contact store is busy. Retry save.', 'List: block', 'Unsaved changes')
                self.assertEqual(rows(), [other, blocked])
            alice.send('Q')
            alice.wait('Discard unsaved changes and quit?', 'y discard  Esc keep editing')
            alice.send('\x1b')
            alice.wait('Contact store is busy. Retry save.', 'Unsaved changes')
            self.assertEqual(rows(), [other, blocked])
            alice.quit('Qy')
            self.assertEqual(rows(), [other, blocked])

    def test_contact_edit_recovery_wide(self):
        self.recover_contact_edits(80, 24)

    def test_contact_edit_recovery_compact(self):
        self.recover_contact_edits(48, 12)

    def test_contact_autosave_disabled(self):
        config = self.root / 'manual.abookrc'
        config.write_text('set autosave=false\n')
        command = [self.runtime / 'bin/abook', '--config', config,
                   '--jaro-profile', self.mail, '--jaro-list', 'allow']
        with Terminal(command, cwd=self.root, env=self.env, cols=48, lines=12) as terminal:
            terminal.wait('List: allow', 'Pat Contact', 'Tab mailboxes')
            terminal.send('\r1')
            terminal.wait('Name: Pat Contact')
            terminal.send('\x15Manual Draft\rq')
            terminal.wait('s save  d discard  Esc keep editing', 'Manual Draft', 'Unsaved changes')
            terminal.send('\r')
            terminal.wait('Manual Draft', 'Ctrl+R reload/discard', 'Unsaved changes')
            self.assertEqual(self.capture([self.jaro, '-q', 'complete', 'manual draft']).returncode, 1)
            terminal.send('qs')
            terminal.wait('Saved to this list', 'Manual Draft', '/ filter')
            committed = self.capture([self.jaro, '-q', 'complete', 'manual draft'])
            self.assertEqual((committed.returncode, committed.stdout),
                             (0, b'\npat@example.org\tManual Draft\tContact note\n'))
            terminal.send('\r1')
            terminal.wait('Name: Manual Draft')
            terminal.send('\x15Discarded Draft\rqd')
            terminal.wait('Reloaded this list', 'Manual Draft', '/ filter')
            self.assertNotIn('Unsaved changes', terminal.screen.text)
            terminal.quit()
        self.assertEqual(self.capture([self.jaro, '-q', 'complete', 'manual draft']).stdout,
                         committed.stdout)
        self.assertEqual(self.capture([self.jaro, '-q', 'complete', 'discarded draft']).returncode, 1)

    def test_contact_bulk_confirmation_counts(self):
        (self.mail / 'whitelist.abook').write_bytes(BOOK_HEADER + b'''
[0]
name=Duplicate
email="quoted,one"@example.org

[1]
name=Duplicate
email=second@example.org

[2]
name=Separate
email=third@example.org
''')
        with Terminal([self.jaro, '-q', 'abook'], cwd=self.root, env=self.env,
                      cols=48, lines=12) as terminal:
            terminal.wait('List: allow', 'Duplicate', 'Separate', 'Tab mailboxes')
            terminal.send('+M')
            terminal.wait('Merge 3 contacts here? Removes 2.', 'y confirm  Esc cancel')
            terminal.send('\x1b')
            terminal.wait('/ filter', 'Separate')
            terminal.send('U')
            terminal.wait('Remove 1 duplicate', 'y confirm  Esc cancel')
            terminal.send('yw')
            terminal.wait('Saved to this list', 'Duplicate', 'Separate')
            terminal.quit()
        with closing(sqlite3.connect(self.mail / 'contacts.sqlite')) as db:
            stored = db.execute('SELECT c.name,a.spelling FROM contacts c JOIN books b ON b.id=c.book_id '
                                'JOIN addresses a ON a.contact_id=c.id WHERE b.name=? ORDER BY a.spelling',
                                ('allow',)).fetchall()
            self.assertEqual(stored, [('Duplicate', '"quoted,one"@example.org'),
                                      ('Duplicate', 'second@example.org'), ('Separate', 'third@example.org')])
            self.assertEqual(db.execute('SELECT count(DISTINCT c.id) FROM contacts c '
                                        'JOIN books b ON b.id=c.book_id WHERE b.name=?', ('allow',)).fetchone(),
                             (2,))

    def test_neomutt_tabs_select_recipients(self):
        launcher = self.root / 'launcher'
        launcher.mkdir()
        # Generate the rc without opening the full UI. The controlled client
        # below uses the installed Tab binding and the generated query command.
        mutt = launcher / 'mutt'
        mutt.write_bytes(b'#!/bin/sh\nexit 0\n')
        mutt.chmod(0o700)
        # The generated command is parsed by the client's shell even when
        # the user's Zsh configuration enables a different quote syntax.
        (self.home / '.zshenv').write_text('setopt rc_quotes\n')
        generated = self.capture([self.jaro, '-q', 'open'],
                                 env=dict(self.env, ZDOTDIR=str(self.home),
                                          PATH=str(launcher) + os.pathsep + self.env['PATH']))
        self.assertEqual((generated.returncode, generated.stdout, generated.stderr), (0, b'', b''))
        lines = (self.mail / '.mutt/rc').read_text().splitlines()
        (query_command,) = [line for line in lines if line.startswith('set query_command ')]
        self.assertTrue(query_command.endswith(' -q complete -- %s"'), query_command)
        bindings = (self.runtime / 'mutt/keybindings').read_text().splitlines()
        tab = next(line for line in bindings if line.split() == ['bind', 'editor', '<tab>', 'complete-query'])

        editor = self.root / 'capture-editor'
        editor.write_bytes(b'#!/bin/sh\ncp "$1" "$CAPTURED_DRAFT"\n')
        editor.chmod(0o700)
        config = self.root / 'client.rc'
        mailbox = self.mail / 'known'
        (mailbox / 'new/1.fixture').write_bytes(
            b'From: sender@example.org\nTo: fixture@example.org\nSubject: Fixture message\n\nExample body\n')
        settings = (
            tab, f'set folder="{self.mail}"', f'set spoolfile="{mailbox}"', f'set editor="{editor}"',
            'set from=fixture@example.org', 'set realname=Fixture', 'set hostname=fixture.invalid',
            'set edit_headers', 'set abort_unmodified=no', 'set postpone=no', 'set quit=yes',
            'set copy=no', 'set sendmail=/bin/false', 'set sort_alias=unsorted',
        )
        cases = (
            ('contact', '<enter>', False, 'Pat Contact <pat@example.org>'),
            ('roster', '', False, '"Group roster (2 recipients)" <roster@jaromail.group>'),
            ('mixed', '<enter>', False, 'Mixed Zed <zed@example.org>'),
            ('mixed', 'j<enter>', False, 'Mixed Ada <ada@example.org>'),
            ('mixed', 'jj<enter>', False, '"Group mixed (2 recipients)" <mixed@jaromail.group>'),
            ('miss', '', False, 'miss@fixture.invalid'),
            ('block-only@fixture.invalid', '', False, 'block-only@fixture.invalid'),
            ('group/roster', '', False, '"Group roster (2 recipients)" <roster@jaromail.group>'),
            ('quoted', '', False, 'Quoted Person <ab@example.org>'),
            ('alternate', '', False, 'Alternate Person <alternate@example.org>'),
            ('alternate', '', True, 'Alternate Person <alternate@example.org>'),
            ("O'Neil", '', False, "Anne-Marie O'Neil <anne@example.org>"),
            ('Anne-Marie', '', False, "Anne-Marie O'Neil <anne@example.org>"),
        )
        for index, (query, choose, alternate, recipient) in enumerate(cases):
            with self.subTest(query=query, choose=choose, alternate=alternate):
                command = query_command.replace(' -q complete ', ' -l alternate -q complete ') if alternate else query_command
                config.write_text('\n'.join((command, *settings)) + '\n')
                draft = self.root / f'draft-{index}'
                keys = f'push "m{query}<tab>{choose}<enter>Fixture<enter>qq"'
                status, tty = run_client([self.client, '-n', '-F', config, '-f', mailbox, '-e', keys],
                                         cwd=self.root, env=dict(self.env, CAPTURED_DRAFT=str(draft)))
                self.assertEqual(status, 0, repr(tty[-600:]))
                self.assertTrue(draft.is_file(), repr(tty[-600:]))
                self.assertEqual([line for line in draft.read_text().splitlines() if line.startswith('To: ')],
                                 ['To: ' + recipient])

    def run_contact_actions(self, mutt_command, url, keys_after):
        book = self.root / 'actions.abook'
        book.write_text(
            "[0]\nname=Anne O'Neil\nemail=\"anne,one\"@example.org,other@example.org\n"
            f"url={url}\n\n[1]\nname=Pat \"Junior\" Contact\nemail=pat@example.org\n")
        recorder = self.root / 'record contact action'
        recorder.write_text(
            '#!/usr/bin/env python3\n'
            'import json, os, signal, sys, termios\n'
            'mode = termios.tcgetattr(0)\n'
            'record = [sys.argv[1:], bool(mode[3] & termios.ICANON), bool(mode[3] & termios.ECHO)]\n'
            'with open(os.environ["CONTACT_ACTION_LOG"], "a") as log:\n'
            '    log.write(json.dumps(record) + "\\n")\n'
            'if sys.argv[1] == "fail":\n'
            '    mode[3] &= ~(termios.ICANON | termios.ECHO)\n'
            '    termios.tcsetattr(0, termios.TCSANOW, mode)\n'
            '    sys.exit(9)\n'
            'if sys.argv[1] == "signal":\n'
            '    os.kill(os.getpid(), signal.SIGINT)\n')
        recorder.chmod(0o700)
        (self.root / 'jaro').symlink_to(recorder)
        config = self.root / 'action.abookrc'
        config.write_text(f"set mutt_command={mutt_command}\n"
                          f"set www_command='{recorder}' browser \"new window\"\n")
        log = self.root / 'action.log'
        abook = self.runtime / 'bin/abook'
        status, tty = run_client(
            [abook, '--config', config, '--datafile', book], cwd=self.root,
            env=dict(self.env, PATH=str(self.root) + os.pathsep + self.env['PATH'],
                     CONTACT_ACTION_LOG=str(log)), keys_after=keys_after)
        self.assertEqual(status, 0, repr(tty[-600:]))
        return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

    def test_external_actions_pass_argv_and_restore_terminal(self):
        recorder = self.root / 'record contact action'
        url = "HTTPS://example.org/O'Neil?tag=a%20b&active=yes"
        records = self.run_contact_actions(
            f"'{recorder}' compose --label \"two words\" '' ordinary\\ flag", url,
            [(b"Anne O'Neil", b'+mvQy')])
        self.assertEqual(records, [
            [['compose', '--label', 'two words', '', 'ordinary flag',
              '"Anne O\'Neil" <"anne,one"@example.org>,"Pat \\"Junior\\" Contact" <pat@example.org>'], True, True],
            [['browser', 'new window', url], True, True],
        ])

    def test_jaro_compose_and_browser_policy_in_contact_editor(self):
        records = self.run_contact_actions(
            'jaro compose', 'ftp://example.org/contact',
            [(b"Anne O'Neil", b'\rmv'),
             (b'Contact URL must be an absolute HTTP or HTTPS URL', b'xqQy')])
        self.assertEqual(records, [[['compose', '"Anne O\'Neil" <"anne,one"@example.org>'], True, True]])

    def test_external_action_nonzero_exit_restores_terminal(self):
        recorder = self.root / 'record contact action'
        url = 'http://example.org/after-failure'
        records = self.run_contact_actions(
            f"'{recorder}' fail", url,
            [(b"Anne O'Neil", b'm'), (b'External command exited with status 9', b'xvQy')])
        self.assertEqual(records, [
            [['fail', '"Anne O\'Neil" <"anne,one"@example.org>'], True, True],
            [['browser', 'new window', url], True, True],
        ])

    def test_external_action_signaled_child_restores_terminal(self):
        recorder = self.root / 'record contact action'
        url = 'http://example.org/after-interrupt'
        message = f'External command terminated by signal {signal.SIGINT}'.encode()
        records = self.run_contact_actions(
            f"'{recorder}' signal", url, [(b"Anne O'Neil", b'm'), (message, b'xvQy')])
        self.assertEqual(records, [
            [['signal', '"Anne O\'Neil" <"anne,one"@example.org>'], True, True],
            [['browser', 'new window', url], True, True],
        ])

    def test_external_action_launch_failure_restores_terminal(self):
        url = 'http://example.org/after-missing-command'
        records = self.run_contact_actions(
            str(self.root / 'missing-program'), url,
            [(b"Anne O'Neil", b'm'), (b'Cannot run external command:', b'xvQy')])
        self.assertEqual(records, [[['browser', 'new window', url], True, True]])

    def test_external_action_rejects_incomplete_command_quote(self):
        url = 'http://example.org/after-invalid-command'
        records = self.run_contact_actions(
            "jaro 'compose", url,
            [(b"Anne O'Neil", b'm'), (b'Invalid external command setting', b'xvQy')])
        self.assertEqual(records, [[['browser', 'new window', url], True, True]])

    def test_filter_snapshot_membership(self):
        products = self.root / 'products'
        products.mkdir()
        env = dict(self.env, PATH=str(self.runtime / 'bin') + os.pathsep + self.env['PATH'])
        result = self.capture([self.snapshot_driver, self.mail, products], env=env)
        self.assertEqual((result.returncode, result.stderr), (0, b''))
        self.assertRegex(result.stdout, rb'^[0-9a-f]{64}\x00$')
        deps = {row[0]: row[1:] for tag, row in records((products / 'dependencies').read_bytes()) if tag == b'D'}
        self.assertEqual(deps[b'config.contact-membership'][-1], b'sqlite-sorted-mailbox-keys-v1')
        self.assertNotIn(b'input.exporter', deps)
        self.assertEqual(deps[b'contacts.allow'][-1],
                         b'"a b"@example.org\n"a,b"@example.org\nada@example.org\nanne@example.org\n'
                         b'first.member@example.org\npat@example.org\nupper.member@example.org\nzed@example.org\n')
        self.assertEqual(deps[b'contacts.block'][-1], b'alternate@example.org\nblock@example.org\npat@example.org\n')
        sieve = (products / 'Filters.sieve').read_bytes()
        self.assertIn(b'"From" "\\"a b\\"@example.org" { fileinto :create "INBOX"; stop; }', sieve)
        self.assertIn(b'"From" "pat@example.org" { fileinto :create "INBOX"; stop; }', sieve)
        self.assertIn(b'"From" "alternate@example.org" { fileinto :create "zz.blacklist"; stop; }', sieve)
        self.assertNotIn(b'ghost@example.org', sieve)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('prefix', 'snapshot-driver', 'client'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--test', action='append', default=[], help='unittest test identity to run')
    args = parser.parse_args()
    InstalledConsumers.prefix = args.prefix.resolve(strict=True)
    InstalledConsumers.snapshot_driver = args.snapshot_driver.resolve(strict=True)
    InstalledConsumers.client = args.client.resolve(strict=True)
    runtime = InstalledConsumers.prefix / 'share/jaromail/bin'
    for label, binary in (
        ('installed jaro launcher', InstalledConsumers.prefix / 'bin/jaro'),
        ('installed jaro runtime', runtime / 'jaro'),
        ('bundled abook', runtime / 'abook'),
        ('filter compiler driver', InstalledConsumers.snapshot_driver),
        ('NeoMutt', InstalledConsumers.client),
    ):
        print(f'{label} sha256 {hashlib.sha256(binary.read_bytes()).hexdigest()}', flush=True)
    version = subprocess.run([InstalledConsumers.client, '-v'], check=True, capture_output=True, timeout=5)
    print(version.stdout.decode().splitlines()[0], flush=True)
    unittest.main(argv=[__file__, *args.test], verbosity=2)
