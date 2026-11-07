#!/usr/bin/env python3
"""Reject a release unless every native helper is the requested static Linux ELF."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

HELPERS = ('fetchaddr', 'parsedate', 'dotlock', 'gpgewrap', 'jaro-filter',
           'jaro-filter-compile', 'jaro-filter-legacy', 'abook')
MACHINES = {'x86_64': 62, 'arm64': 183}


def inspect(directory, arch):
    records = []
    for name in HELPERS:
        path = directory / name
        header = path.read_bytes()[:64]
        if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01':
            raise ValueError(f'{name}: expected a little-endian ELF64 executable')
        if struct.unpack_from('<H', header, 18)[0] != MACHINES[arch]:
            raise ValueError(f'{name}: wrong machine for {arch}')
        result = subprocess.run(['readelf', '-lW', '-dW', str(path)],
                                env=dict(os.environ, LC_ALL='C'), text=True, capture_output=True, check=True)
        if 'INTERP' in result.stdout or '(NEEDED)' in result.stdout:
            raise ValueError(f'{name}: dynamic interpreter or shared-library dependency present')
        records.append({'name': name, 'arch': arch, 'static': True, 'bytes': path.stat().st_size})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--arch', choices=MACHINES, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps({'helpers': inspect(args.bin_dir, args.arch)}, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'static release check: {error}\n')


if __name__ == '__main__':
    main()
