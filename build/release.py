#!/usr/bin/env python3
"""Prepare and publish one immutable, already-tested GitHub release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from version import release_tags, valid


def gh(*args, check=True):
    return subprocess.run(['gh', *args], text=True, capture_output=True, check=check)


def prepare(source, output):
    tags = release_tags(source)
    if not tags:
        raise ValueError('semantic releases require an existing release tag')
    tag = tags[-1][1]
    bootstrap = tags[-1][0][0] < 6
    with output.open('a') as stream:
        stream.write('tag='+tag+'\n')
        stream.write('additional='+('breaking: start the v6 release series' if bootstrap else '')+'\n')


def validate_next(current, next_version):
    current = valid(current.removeprefix('v'))
    next_version = valid(next_version)
    old, new = tuple(map(int, current.split('.'))), tuple(map(int, next_version.split('.')))
    if (old[0] < 6 and next_version != '6.0.0') or new < old:
        raise ValueError('first release must be 6.0.0; subsequent versions cannot go backwards')


def publish(version, directory, repository, commit):
    version = valid(version)
    if tuple(map(int, version.split('.'))) < (6, 0, 0):
        raise ValueError('refusing to publish a version before v6.0.0')
    assets = []
    for arch in ('x86_64', 'arm64'):
        archive = directory / f'jaromail-{version}-linux-{arch}.tar.gz'
        checksum = archive.with_name(archive.name+'.sha256')
        if not archive.is_file() or not checksum.is_file():
            raise ValueError(f'missing archive or checksum for {arch}')
        expected = checksum.read_text().split()[0]
        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f'archive checksum mismatch for {arch}')
        assets += [archive, checksum]
    tag = 'v'+version
    existing = gh('release', 'view', tag, '--repo', repository, '--json', 'isDraft,assets', check=False)
    if existing.returncode == 0:
        # Published versions are immutable. A completed workflow rerun is a no-op.
        target = gh('api', f'repos/{repository}/commits/{tag}', '--jq', '.sha').stdout.strip()
        if target != commit:
            raise ValueError('existing release tag points to another commit')
        release = json.loads(existing.stdout)
        if not release['isDraft']:
            names = {asset['name'] for asset in release['assets']}
            if not all(path.name in names for path in assets):
                raise ValueError('published release is missing required artifacts; refusing to modify it')
            print('Release already published: '+tag)
            return
        gh('release', 'upload', tag, '--repo', repository, '--clobber', *map(str, assets))
    else:
        # Do not let a manually created tag silently select a different commit.
        target = gh('api', f'repos/{repository}/commits/{tag}', '--jq', '.sha', check=False)
        if target.returncode == 0 and target.stdout.strip() != commit:
            raise ValueError('release tag already exists on another commit')
        gh('release', 'create', tag, '--repo', repository, '--target', commit, '--draft',
           '--title', 'JaroMail '+tag, '--generate-notes', *map(str, assets))
    gh('release', 'edit', tag, '--repo', repository, '--draft=false', '--latest')
    print('Published '+tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    prep.add_argument('--output', type=Path, required=True)
    validation = sub.add_parser('validate-version')
    validation.add_argument('--current', required=True)
    validation.add_argument('--next', dest='next_version', required=True)
    pub = sub.add_parser('publish')
    pub.add_argument('--version', required=True)
    pub.add_argument('--artifacts', type=Path, required=True)
    pub.add_argument('--repository', required=True)
    pub.add_argument('--commit', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'prepare':
            prepare(args.source, args.output)
        elif args.command == 'validate-version':
            validate_next(args.current, args.next_version)
        else:
            publish(args.version, args.artifacts, args.repository, args.commit)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'release: {error}\n')


if __name__ == '__main__':
    main()
