#!/usr/bin/env python3
"""Compatibility entry point: all compiler and install rules live in CMake."""
import argparse
import fcntl
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=Path('build/gnu'))
    parser.add_argument('--target', default='all')
    parser.add_argument('--testing', action='store_true')
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--clean', action='store_true')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    binary = output / 'cmake'
    output.mkdir(parents=True, exist_ok=True)
    with (output / '.cmake.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.clean:
            if (binary / 'CMakeCache.txt').exists():
                subprocess.run(['cmake', '--build', str(binary), '--target', 'clean'], check=True)
            return
        command = ['cmake', '-S', str(source), '-B', str(binary),
                   '-DCMAKE_BUILD_TYPE=', '-DCMAKE_RUNTIME_OUTPUT_DIRECTORY='+str(output),
                   '-DJARO_BUILD_TEST_DRIVERS='+('ON' if args.testing else 'OFF'),
                   '-DBUILD_TESTING=OFF', '-DCMAKE_INSTALL_PREFIX='+os.environ.get('PREFIX', '/usr/local'),
                   '-DCMAKE_C_FLAGS='+' '.join(v for v in (os.environ.get('CPPFLAGS',''), os.environ.get('CFLAGS','-O2')) if v),
                   '-DCMAKE_EXE_LINKER_FLAGS='+os.environ.get('LDFLAGS',''),
                   '-DCMAKE_C_STANDARD_LIBRARIES='+os.environ.get('LDLIBS','')]
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        already_wrapped = compiler and Path(compiler[0]).name in ('ccache', 'sccache', 'distcc')
        if 'CCACHE' in os.environ and not already_wrapped:
            command.append('-DCMAKE_C_COMPILER_LAUNCHER='+os.environ['CCACHE'])
        subprocess.run(command, check=True)
        subprocess.run(['cmake', '--build', str(binary), '--parallel', os.environ.get('CMAKE_BUILD_PARALLEL_LEVEL','2'), '--target', args.target], check=True)
        if args.install:
            subprocess.run(['cmake', '--install', str(binary)], check=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'cmake-build: {error}', file=sys.stderr)
        sys.exit(1)
