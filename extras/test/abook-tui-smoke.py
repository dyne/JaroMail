#!/usr/bin/env python3
"""Open and quit the real abook TUI in a private terminal/addressbook."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--emulator')
    parser.add_argument('--no-terminfo', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='jaro-abook-tui-') as directory:
        root = Path(directory)
        book = root/'contacts.abook'
        book.write_text('[format]\nprogram=abook\nversion=0.6.2\n\n[0]\nname=TUI Fixture\nemail=terminal@example.org\n')
        env = dict(os.environ,TERM='xterm-256color',LC_ALL='C.UTF-8')
        if args.no_terminfo:
            env.update(TERMINFO=str(root/'no-terminfo'),TERMINFO_DIRS=str(root/'no-terminfo'))
        master, slave = pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,80,0,0))
        command = ([args.emulator] if args.emulator else []) + [str(args.binary.resolve()),
                    '--config','/dev/null','--datafile',str(book)]
        process = subprocess.Popen(command,stdin=slave,stdout=slave,stderr=slave,env=env,cwd=root,
                                   start_new_session=True)
        os.close(slave)
        output, sent = bytearray(), False
        deadline = time.monotonic()+10
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master],[],[],0.1)
                if ready:
                    try:
                        chunk = os.read(master,65536)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        break
                    output.extend(chunk)
                    if not sent and b'TUI Fixture' in output:
                        os.write(master,b'q')
                        sent = True
                if process.poll() is not None:
                    break
            try:
                process.wait(timeout=max(0.01,deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                raise RuntimeError('abook TUI did not quit within its bounded wait')
            if process.returncode or not sent:
                raise RuntimeError(f'abook TUI failed: exit={process.returncode}, output={bytes(output[-500:])!r}')
            if 'terminal@example.org' not in book.read_text():
                raise RuntimeError('abook TUI lost the fixture address')
            print(json.dumps(dict(tui='passed',emulated=bool(args.emulator),terminfoFallback=args.no_terminfo)))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)


if __name__ == '__main__':
    main()
