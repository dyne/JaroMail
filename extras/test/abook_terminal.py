"""Bounded xterm PTY and screen capture for the real contact editor."""

import codecs
from contextlib import suppress
import errno
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import termios
import time
import unicodedata


class Screen:
    # ncurses sends relative cursor moves and erases on incremental updates.
    control = re.compile(r'\x1b(?:\[[0-?]*[ -/]*[@-~]|[()][A-Z0-9]|[=>78M])')

    def __init__(self, cols, lines):
        self.pending = ''
        self.decode = codecs.getincrementaldecoder('utf-8')()
        self.resize(cols, lines)

    def resize(self, cols, lines):
        self.cols = cols
        self.lines = lines
        self.cells = [[' '] * cols for _ in range(lines)]
        self.row, self.col = 0, 0
        self.saved_cursor = (0, 0)
        self.line_drawing = False
        self.top, self.bottom = 0, lines - 1
        self.cursor_visible = True
        self.last_char = ' '

    def csi(self, parameters, op):
        if parameters.startswith('?'):
            if parameters == '?25' and op in 'hl':
                self.cursor_visible = op == 'h'
            return
        values = [int(arg or '0') for arg in parameters.split(';')]
        value = values[0] or 1
        if op in 'Hf':
            self.row = value - 1
            self.col = (values[1] or 1) - 1 if len(values) > 1 else 0
        elif op == 'd':
            self.row = value - 1
        elif op == 'G':
            self.col = value - 1
        elif op == 'A':
            self.row -= value
        elif op == 'B':
            self.row += value
        elif op == 'C':
            self.col += value
        elif op == 'D':
            self.col -= value
        elif op == 'J':
            if values[0] == 2:
                self.cells = [[' '] * self.cols for _ in range(self.lines)]
            elif values[0] == 0:
                self.erase(self.row, self.col, self.cols)
                for row in range(self.row + 1, self.lines):
                    self.erase(row, 0, self.cols)
        elif op == 'K':
            start = 0 if values[0] else self.col
            end = self.col + 1 if values[0] == 1 else self.cols
            self.erase(self.row, start, end)
        elif op == 'X':
            self.erase(self.row, self.col, min(self.cols, self.col + value))
        elif op == 'P':
            count = min(value, self.cols - self.col)
            row = self.cells[self.row]
            self.cells[self.row] = row[:self.col] + row[self.col + count:] + [' '] * count
        elif op == 'b':
            for _ in range(value):
                self.put(self.last_char)
        elif op == 'r':
            self.top = value - 1
            self.bottom = (values[1] or self.lines) - 1 if len(values) > 1 else self.lines - 1
            self.row, self.col = 0, 0
        elif op in 'ST':
            for _ in range(min(value, self.bottom - self.top + 1)):
                self.scroll(down=op == 'T')
        elif op not in 'hlmt':
            raise AssertionError(f'unhandled terminal sequence {parameters}{op}')
        self.row = min(max(self.row, 0), self.lines - 1)
        self.col = min(max(self.col, 0), self.cols)

    def erase(self, row, start, end):
        self.cells[row][start:end] = [' '] * (end - start)

    def put(self, char):
        if unicodedata.combining(char):
            previous = self.col - 1
            while previous >= 0 and not self.cells[self.row][previous]:
                previous -= 1
            if previous >= 0:
                self.cells[self.row][previous] += char
            return
        if self.col == self.cols:
            self.col = 0
            self.linefeed()
        self.cells[self.row][self.col] = char
        self.col += 1
        if unicodedata.east_asian_width(char) in ('F', 'W') and self.col < self.cols:
            self.cells[self.row][self.col] = ''
            self.col += 1
        self.last_char = char

    def linefeed(self):
        if self.row == self.bottom:
            self.scroll(down=False)
        else:
            self.row = min(self.row + 1, self.lines - 1)

    def scroll(self, *, down):
        del self.cells[self.bottom if down else self.top]
        self.cells.insert(self.top if down else self.bottom, [' '] * self.cols)

    def feed(self, block):
        text = self.pending + self.decode.decode(block)
        offset = 0
        while offset < len(text):
            char = text[offset]
            if char == '\x1b':
                match = self.control.match(text, offset)
                if not match:
                    break
                seq = match.group()
                if seq.startswith('\x1b['):
                    self.csi(seq[2:-1], seq[-1])
                elif seq == '\x1b7':
                    self.saved_cursor = self.row, self.col
                elif seq == '\x1b8':
                    self.row, self.col = self.saved_cursor
                elif seq == '\x1bM':
                    if self.row == self.top:
                        self.scroll(down=True)
                    else:
                        self.row = max(self.row - 1, 0)
                elif seq.startswith('\x1b('):
                    self.line_drawing = seq[-1] == '0'
                offset = match.end()
                continue
            if char == '\r':
                self.col = 0
            elif char == '\n':
                self.linefeed()
            elif char == '\b':
                self.col = max(0, self.col - 1)
            elif char == '\t':
                self.col = min((self.col // 8 + 1) * 8, self.cols - 1)
            elif char.isprintable():
                if self.line_drawing:
                    char = {'q': '─', 'x': '│', 'l': '┌', 'k': '┐', 'm': '└', 'j': '┘',
                            'v': '┴'}.get(char, char)
                self.put(char)
            offset += 1
        self.pending = text[offset:]
        if len(self.pending) > 64:
            raise AssertionError(f'unknown terminal sequence {self.pending[:64]!r}')

    @property
    def text(self):
        return '\n'.join(''.join(row).rstrip() for row in self.cells)

    def capture(self):
        return f'{self.cols}x{self.lines} cursor={self.row + 1},{self.col + 1} visible={self.cursor_visible}\n{self.text}\n'


class Terminal:
    def __init__(self, command, *, cwd, env, cols, lines):
        self.screen = Screen(cols, lines)
        self.output = bytearray()
        self.master, slave = pty.openpty()
        try:
            self.set_size(slave, cols, lines)
            self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=slave, stdout=slave,
                                            stderr=slave, start_new_session=True)
        except BaseException:
            os.close(self.master)
            raise
        finally:
            os.close(slave)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait()
        os.close(self.master)

    @staticmethod
    def set_size(fd, cols, lines):
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', lines, cols, 0, 0))

    def resize(self, cols, lines):
        self.screen.resize(cols, lines)
        self.set_size(self.master, cols, lines)
        os.killpg(self.process.pid, signal.SIGWINCH)

    def send(self, keys):
        os.write(self.master, keys.encode('utf-8') if isinstance(keys, str) else keys)

    def wait(self, *strings, cursor=None):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if all(text in self.screen.text for text in strings) and (
                cursor is None or cursor(self.screen)
            ):
                return
            ready, _, _ = select.select([self.master], [], [], 0.05)
            if ready:
                try:
                    block = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    break
                if not block:
                    break
                self.output.extend(block)
                self.screen.feed(block)
        raise AssertionError(f'expected {strings!r} on screen, exit={self.process.poll()}\n'
                             + self.screen.capture() + repr(bytes(self.output[-1200:])))

    def quit(self, keys='q'):
        self.send(keys)
        result = self.process.wait(timeout=5)
        if result:
            raise AssertionError(f'editor exit {result}\n' + self.screen.capture())
