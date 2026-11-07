#!/usr/bin/env python3
"""Resolve release metadata without changing the source tree or contacting GitHub."""
import argparse
import json
from pathlib import Path
import re
import subprocess

SEMVER = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z')


def valid(value):
    match = SEMVER.fullmatch(value)
    if not match:
        raise ValueError('version must be a semantic version without the v prefix')
    if match[4] and any(p.isdecimal() and len(p) > 1 and p[0] == '0' for p in match[4].split('.')):
        raise ValueError('numeric prerelease identifiers cannot have leading zeros')
    return value


def git(source, *args):
    try:
        result = subprocess.run(['git', '-C', str(source), *args], capture_output=True, text=True, check=False)
        return result.stdout.strip() if result.returncode == 0 else ''
    except OSError:
        return ''


def release_tags(source):
    # A copied source fixture must never inherit its enclosing repository's tags.
    top = git(source, 'rev-parse', '--show-toplevel')
    if not top or Path(top).resolve() != source.resolve():
        return []
    result = []
    for tag in git(source, 'tag', '--merged', 'HEAD', '--list', 'v[0-9]*').splitlines():
        value = tag[1:]
        if SEMVER.fullmatch(value) and '-' not in value and '+' not in value:
            result.append((tuple(map(int, value.split('.'))), tag))
    return sorted(result)


def resolve(source, override=None):
    baseline = valid((source / 'VERSION').read_text().strip())
    if override is not None:
        return valid(override)
    tags = release_tags(source)
    if tags and tags[-1][0] >= tuple(map(int, baseline.split('.'))):
        return tags[-1][1][1:]
    archive = source / '.git_archival.txt'
    if archive.is_file():
        description = archive.read_text().strip().removeprefix('describe: ')
        match = re.fullmatch(r'v([0-9]+\.[0-9]+\.[0-9]+)(?:-[0-9]+-g[0-9a-f]+)?', description)
        if match and tuple(map(int, match[1].split('.'))) >= tuple(map(int, baseline.split('.'))):
            return valid(match[1])
    return baseline


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--version', help='release workflow override')
    parser.add_argument('--latest-tag', action='store_true', help='latest reachable stable SemVer tag, including pre-v6 history')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    try:
        if args.latest_tag:
            tags = release_tags(args.source)
            if not tags:
                raise ValueError('no reachable stable semantic version tag')
            print(tags[-1][1])
        else:
            version = resolve(args.source, args.version)
            print(json.dumps({'version': version}) if args.json else version)
    except (OSError, ValueError) as error:
        parser.exit(2, f'version: {error}\n')


if __name__ == '__main__':
    main()
