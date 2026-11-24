#!/usr/bin/env python3
"""Exercise abook and SQLite contacts in an installed or unpacked package."""

import argparse
from configparser import ConfigParser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ALIASES = ('alias -group test fixture Zoë Fixture <first.static@fixture.invalid>\n'
           'alias second Renée Second <second.static@fixture.invalid>').encode()
BOOK = [
    {'name': 'Zoë Fixture', 'email': 'first.static@fixture.invalid', 'nick': 'fixture', 'groups': 'test'},
    {'name': 'Renée Second', 'email': 'second.static@fixture.invalid', 'nick': 'second'},
]
ROW = '\nsecond.static@fixture.invalid\tRenée Second\t \n'.encode()


def records(path):
    book = ConfigParser(interpolation=None)
    book.read_string(path.read_text(encoding='utf-8'))
    return [dict(book[section]) for section in book.sections() if section.isdecimal()]


def packaged(root, name):
    binary = root / name
    if not binary.is_file() or not binary.resolve().is_relative_to(root) or not os.access(binary, os.X_OK):
        raise RuntimeError(f'expected an executable inside the package: {binary}')
    return binary


def smoke(prefix, version, emulator, cli):
    runtime = prefix / 'share/jaromail'
    abook = packaged(prefix, 'share/jaromail/bin/abook')
    compiler = packaged(prefix, 'share/jaromail/bin/jaro-filter-compile')
    jaro = packaged(prefix, 'bin/jaro')
    manifest = Path(__file__).resolve().parents[2] / 'vendor/abook/sources.json'
    fork = json.loads(manifest.read_text())['fork']['version']
    if (runtime / 'VERSION').read_text().strip() != version:
        raise RuntimeError('unexpected packaged version metadata')
    checks = ['version-metadata']
    with tempfile.TemporaryDirectory(prefix='jaro-abook-package-') as scratch:
        home = Path(scratch)
        profile, poison = home / 'Maildir', home / 'no-host-helpers'
        profile.mkdir(mode=0o700)
        poison.mkdir()
        (home / '.abook').mkdir()
        (profile / 'Groups').mkdir()
        for name in ('abook', 'jaro-filter-compile'):
            stub = poison / name
            stub.write_text('#!/bin/sh\necho unexpected host helper >&2\nexit 99\n')
            stub.chmod(0o700)
        env = dict(os.environ, HOME=str(home), JAROMAILDIR=str(profile),
                   JAROACCOUNT=str(profile / 'Accounts'), LC_ALL='C', LANGUAGE='C',
                   XDG_CONFIG_HOME=str(home / 'config'), XDG_DATA_HOME=str(home / 'data'),
                   XDG_CACHE_HOME=str(home / 'cache'), PATH=f"{poison}:{os.environ['PATH']}")
        env.pop('JAROWORKDIR', None)

        def run(label, argv, status=0, stdout=b''):
            result = subprocess.run(argv, cwd=home, env=env, stdin=subprocess.DEVNULL,
                                    capture_output=True, timeout=20)
            if result.returncode != status or result.stderr or (
                    stdout is not None and result.stdout != stdout):
                raise RuntimeError(f'{label}: {argv}: exit {result.returncode}, '
                                   f'{result.stdout[:400]!r}, {result.stderr[:400]!r}')
            return result.stdout

        native = [emulator] if emulator else []
        abook_command = [*native, abook, '--config', '/dev/null']
        help_text = run('private abook version', [*native, abook, '--help'], stdout=None)
        if f'JaroMail abook fork {fork}'.encode() not in help_text.splitlines():
            raise RuntimeError('private abook fork identity was absent')
        checks.append('private-fork-version')
        aliases, book = home / 'aliases', profile / 'whitelist.abook'
        aliases.write_bytes(ALIASES)
        run('Mutt conversion', [*abook_command, '--convert', '--informat', 'mutt',
                               '--infile', aliases, '--outformat', 'abook', '--outfile', book])
        if records(book) != BOOK or aliases.read_bytes() != ALIASES:
            raise RuntimeError('Mutt conversion changed fields, order, or its source')
        checks.append('mutt-conversion')
        run('direct query hit', [*abook_command, '--datafile', book, '--mutt-query', 'renee'],
            stdout=ROW)
        run('direct query miss', [*abook_command, '--datafile', book, '--mutt-query', 'missing'],
            status=1, stdout=b'Not found\n')
        checks.extend(('abook-query-hit', 'abook-query-miss'))
        card, converted = home / 'contacts.vcf', home / 'vcard.abook'
        run('vCard export', [*abook_command, '--convert', '--informat', 'abook',
                             '--infile', book, '--outformat', 'vcard', '--outfile', card])
        run('vCard import', [*abook_command, '--convert', '--informat', 'vcard',
                             '--infile', card, '--outformat', 'abook', '--outfile', converted])
        # vCard has no built-in mapping for the Mutt alias group.
        expected = [{key: value for key, value in row.items() if key != 'groups'} for row in BOOK]
        if records(converted) != expected:
            raise RuntimeError('vCard round trip changed a supported contact field')
        checks.append('vcard-round-trip')
        run('SQLite completion', [*native, compiler, '--recipient-contacts', profile, 'renee'],
            stdout=ROW)
        if not (profile / 'contacts.sqlite').is_file():
            raise RuntimeError('packaged helper did not initialize SQLite')
        checks.append('native-sqlite-completion')
        book.unlink()
        if cli:
            run('packaged jaro version', [jaro, '--version'],
                stdout=f'Jaro Mail v{version}\n'.encode())
            run('packaged jaro completion', [jaro, '-q', 'complete', 'renee'], stdout=ROW)
            run('packaged jaro completion miss', [jaro, '-q', 'complete', 'missing'],
                status=1, stdout=b'Not found\n')
            checks.extend(('jaro-version', 'jaro-completion-hit', 'jaro-completion-miss'))
    return {
        'prefix': str(prefix), 'version': version, 'fork_version': fork, 'emulator': emulator,
        'shell_runtime': 'executed' if cli else 'not selected',
        'checks': checks,
        'sha256': {str(path.relative_to(prefix)): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in (abook, compiler, jaro)},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--version', help='assigned package version')
    execution = parser.add_mutually_exclusive_group(required=True)
    execution.add_argument('--cli', action='store_true', help='run the installed shell and native helpers')
    execution.add_argument('--emulator', help='run packaged helpers under a user-mode emulator')
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)
    version = args.version or (prefix / 'share/jaromail/VERSION').read_text().strip()
    emulator = shutil.which(args.emulator) if args.emulator else None
    if args.emulator and not emulator:
        parser.error(f'emulator cannot be found: {args.emulator}')
    print(json.dumps(smoke(prefix, version, emulator, args.cli), sort_keys=True))


if __name__ == '__main__':
    main()
