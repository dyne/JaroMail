"""Characterize the installed JaroMail query consumers using synthetic mail."""

import argparse
from contextlib import closing, suppress
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
                 b'\n[1]\nname=Block overlap\nemail=pat@example.org,alternate@example.org\n',
}
GROUP_ROSTER = b'roster@jaromail.group\tGroup roster (2 recipients)\n'


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
    def profile_tree(profile):
        tree = {}
        for path in [profile, *profile.rglob('*')]:
            info = path.lstat()
            data = (os.readlink(path) if stat.S_ISLNK(info.st_mode) else
                    path.read_bytes() if stat.S_ISREG(info.st_mode) else None)
            tree[path.relative_to(profile)] = (info.st_mode, info.st_ino, info.st_mtime_ns, data)
        return tree

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
                                                           (b'Remove selected item(s)', b'yq')])
        self.assertEqual(status, 0, repr(tty[-1000:]))
        self.assertEqual(self.capture([self.jaro, '-q', '-l', selected, 'addr', 'writer updated']).returncode, 1)
        self.assertEqual(self.capture([self.jaro, '-q', '-l', selected, 'addr', 'from vcard']).returncode, 0)

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
    unittest.main(argv=[__file__], verbosity=2)
