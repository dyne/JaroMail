"""Characterize the installed JaroMail query consumers using synthetic mail."""

import argparse
from contextlib import suppress
import errno
import fcntl
import hashlib
import os
from pathlib import Path
import pty
import select
import signal
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
''',
    'alternate': BOOK_HEADER + b'\n[0]\nname=Alternate Person\nemail=alternate@example.org\n',
    'blacklist': BOOK_HEADER + b'\n[0]\nname=Block Person\nemail=block@example.org\n',
}
GROUP_ROSTER = b'roster@jaromail.group\tGroup roster (2 recipients)\n'


def run_client(command, *, cwd, env):
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
        (self.mail / 'Groups').mkdir(parents=True)
        (self.home / '.abook').mkdir()
        (self.home / '.abook/abookrc').touch()
        (self.mail / 'Filters.txt').write_bytes(b'from-address fixture@example.org move Fixture\n')
        for book, data in BOOKS.items():
            (self.mail / f'{book}.abook').write_bytes(data)
        for group in ('roster', 'mixed'):
            (self.mail / 'Groups' / group).write_bytes(
                b'# members\n\nOne <one@example.org>\nTwo <two@example.org>\n')
        self.env = dict(os.environ, HOME=str(self.home), JAROMAILDIR=str(self.mail),
                        JAROACCOUNT=str(self.mail / 'Accounts'), TMPDIR=str(self.root),
                        LC_ALL='C', LANGUAGE='C', TERM='xterm-256color')
        self.env.pop('JAROWORKDIR', None)

    def capture(self, command, *, env=None):
        return subprocess.run(command, env=self.env if env is None else env, cwd=self.root,
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=15)

    def test_cli_query_records(self):
        cases = (
            ('contact', (), 0, b'\npat@example.org\tPat Contact\tContact note\n'),
            ('roster', (), 0, b'Not found\n' + GROUP_ROSTER),
            ('mixed', (), 0, b'\nzed@example.org\tMixed Zed\t \nada@example.org\tMixed Ada\t \n'
                              b'mixed@jaromail.group\tGroup mixed (2 recipients)\n'),
            ('miss', (), 1, b'Not found\n'),
            ('group/roster', (), 0, GROUP_ROSTER),
            ('quoted', (), 0, b'\n"a b"@example.org\tQuoted Person\tghost@example.org\n'),
            ('alternate', (), 1, b'Not found\n'),
            ('alternate', ('-l', 'alternate'), 0, b'\nalternate@example.org\tAlternate Person\t \n'),
        )
        for query, options, status, stdout in cases:
            with self.subTest(query=query, options=options):
                result = self.capture([self.jaro, '-q', *options, 'complete', query])
                self.assertEqual((result.returncode, result.stdout, result.stderr), (status, stdout, b''))

        for query, stdout in (
            ('contact', b'Pat Contact Contact note <pat@example.org>\n'),
            ('quoted', b'b"@example.org Quoted Person ghost@example.org <"a>\n'),
        ):
            with self.subTest(command='addr', query=query):
                result = self.capture([self.jaro, '-q', 'addr', query])
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, stdout, b''))

    def test_neomutt_tabs_select_recipients(self):
        launcher = self.root / 'launcher'
        launcher.mkdir()
        # Generate the rc without opening the full UI. The controlled client
        # below uses the installed Tab binding and the generated query command.
        mutt = launcher / 'mutt'
        mutt.write_bytes(b'#!/bin/sh\nexit 0\n')
        mutt.chmod(0o700)
        generated = self.capture([self.jaro, '-q', 'open'],
                                 env=dict(self.env, PATH=str(launcher) + os.pathsep + self.env['PATH']))
        self.assertEqual((generated.returncode, generated.stdout, generated.stderr), (0, b'', b''))
        lines = (self.mail / '.mutt/rc').read_text().splitlines()
        expected = f'set query_command = "{self.runtime / "bin/jaro"} -q complete \'%s\'"'
        self.assertEqual([line for line in lines if line.startswith('set query_command ')], [expected])
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
            ('contact', '', False, 'Pat Contact <pat@example.org>'),
            ('roster', '', False, '"Group roster (2 recipients)" <roster@jaromail.group>'),
            ('mixed', '<enter>', False, 'Mixed Zed <zed@example.org>'),
            ('mixed', 'j<enter>', False, 'Mixed Ada <ada@example.org>'),
            ('mixed', 'jj<enter>', False, '"Group mixed (2 recipients)" <mixed@jaromail.group>'),
            ('miss', '', False, 'miss@fixture.invalid'),
            ('group/roster', '', False, 'group/roster@fixture.invalid'),
            ('quoted', '', False, 'Quoted Person <ab@example.org>'),
            ('alternate', '', False, 'alternate@fixture.invalid'),
            ('alternate', '', True, 'Alternate Person <alternate@example.org>'),
        )
        for index, (query, choose, alternate, recipient) in enumerate(cases):
            with self.subTest(query=query, choose=choose, alternate=alternate):
                command = expected.replace(' -q complete ', ' -l alternate -q complete ') if alternate else expected
                config.write_text('\n'.join((command, *settings)) + '\n')
                draft = self.root / f'draft-{index}'
                keys = f'push "m{query}<tab>{choose}<enter>Fixture<enter>qq"'
                status, tty = run_client([self.client, '-n', '-F', config, '-f', mailbox, '-e', keys],
                                         cwd=self.root, env=dict(self.env, CAPTURED_DRAFT=str(draft)))
                self.assertEqual(status, 0, repr(tty[-600:]))
                self.assertTrue(draft.is_file(), repr(tty[-600:]))
                self.assertEqual([line for line in draft.read_text().splitlines() if line.startswith('To: ')],
                                 ['To: ' + recipient])

    def test_filter_snapshot_membership(self):
        products = self.root / 'products'
        products.mkdir()
        env = dict(self.env, PATH=str(self.runtime / 'bin') + os.pathsep + self.env['PATH'])
        result = self.capture([self.snapshot_driver, self.mail, products], env=env)
        self.assertEqual((result.returncode, result.stderr), (0, b''))
        self.assertRegex(result.stdout, rb'^[0-9a-f]{64}\x00$')
        deps = {row[0]: row[1:] for tag, row in records((products / 'dependencies').read_bytes()) if tag == b'D'}
        self.assertEqual(deps[b'config.exporter-locale'][-1], b'C')
        self.assertEqual(deps[b'config.exporter-flags'][-1],
                         b'--config\0/dev/null\0--datafile\0SNAPSHOT-FD\0--mutt-query\0\0')
        abook = self.runtime / 'bin/abook'
        self.assertEqual(deps[b'input.exporter'][:3],
                         [os.fsencode(abook), b'1', hashlib.sha256(abook.read_bytes()).digest()])
        self.assertEqual(deps[b'export.whitelist'][-1],
                         b'pat@example.org\nzed@example.org\nada@example.org\n"a b"@example.org\n')
        self.assertEqual(deps[b'export.blacklist'][-1], b'block@example.org\n')
        sieve = (products / 'Filters.sieve').read_bytes()
        self.assertIn(b'"From" "\\"a b\\"@example.org" { fileinto :create "INBOX"; stop; }', sieve)
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
