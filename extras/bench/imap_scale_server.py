"""Lazy, read-only loopback IMAP dataset for the installed terminal benchmark."""
from collections import Counter
import re
import socket
import threading


MIB = 1024 * 1024
GROUPS = LEAVES = 100
BUSY = "Archive.G000.B000"
SMALL, LARGE = "Exports.Small", "Exports.Large"
BLOCK = bytes(range(256)) * 256
HEADER = b"Date: Fri, 27 Nov 2026 00:00:00 +0000\r\nFrom: fixture@example.invalid\r\nSubject: scale\r\n\r\n"


def uid(position):
    return 17 + (position - 1) * 4093


def count(name):
    if name in (SMALL, LARGE):
        return 2
    if name == BUSY:
        return 1_000_000
    if re.fullmatch(r"Archive\.G\d{3}\.B\d{3}", name):
        return 100
    return 0


def size(name, position):
    if name in (SMALL, LARGE):
        return 4096 if position == 1 else (128 if name == LARGE else 1) * MIB
    return 1024 + int(name[-3:])


def total(name):
    if name in (SMALL, LARGE):
        return size(name, 1) + size(name, 2)
    return count(name) * size(name, 1)


def piece(name, position, offset, span):
    n = max(0, min(span, size(name, position) - offset))
    prefix = HEADER[offset:offset + n]
    start = max(0, offset - len(HEADER)) % len(BLOCK)
    rest = n - len(prefix)
    return prefix + (BLOCK * ((start + rest + len(BLOCK) - 1) // len(BLOCK)))[start:start + rest]


def children(pattern):
    if pattern == "%":
        return ["Archive", "Exports"]
    if pattern == "Archive.%":
        return [f"Archive.G{i:03}" for i in range(GROUPS)]
    if re.fullmatch(r"Archive\.G\d{3}\.%", pattern):
        return [pattern[:-1] + f"B{i:03}" for i in range(LEAVES)]
    if pattern == "Exports.%":
        return [SMALL, LARGE]
    if pattern.endswith(".%") and count(pattern[:-2]):
        return []
    raise AssertionError("Unexpected LIST scope: " + pattern)


class Server:
    def __init__(self, listener, *, fast=True, interrupt=False):
        self.fast, self.interrupt = fast, interrupt
        self.request_cancel = threading.Event()
        self.wire, self.commands, self.transfers = [], Counter(), []
        self.rx = self.tx = self.header_bytes = self.size_rows = self.inventory_rows = 0
        self.error = None
        self.listener = listener
        self.thread = threading.Thread(target=self.serve)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.thread.join(12)
        assert not self.thread.is_alive(), "fixture connection did not close"
        if self.error:
            raise self.error

    def serve(self):
        try:
            conn, _ = self.listener.accept()
            with conn, conn.makefile("rb") as source:
                conn.settimeout(10)
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                def send(data):
                    if isinstance(data, str):
                        data = data.encode("ascii")
                    conn.sendall(data)
                    self.tx += len(data)
                send("* OK generated fixture\r\n")
                while True:
                    line = source.readline(8192)
                    self.rx += len(line)
                    if not line:
                        return
                    tag, cmd, *rest = line.rstrip(b"\r\n").decode("ascii").split(" ", 2)
                    args = rest[0] if rest else ""
                    # Never retain the LOGIN operands or any literal contents.
                    self.wire.append([cmd, "<redacted>" if cmd == "LOGIN" else args])
                    self.commands[cmd] += 1
                    if cmd == "CAPABILITY":
                        send("* CAPABILITY IMAP4rev1" + (" LIST-STATUS STATUS=SIZE" if self.fast else "") + "\r\n")
                    elif cmd == "LOGIN":
                        pass
                    elif cmd == "LIST":
                        match = re.fullmatch(r'"" "([^"]+)"( RETURN \(STATUS \(SIZE\)\))?', args)
                        assert match, args
                        for name in children(match[1]):
                            flags = r"\HasNoChildren" if count(name) else r"\Noselect \HasChildren"
                            send(f'* LIST ({flags}) "." "{name}"\r\n')
                            if match[2] and count(name):
                                assert self.fast
                                send(f'* STATUS "{name}" (SIZE {total(name)})\r\n')
                    elif cmd == "STATUS":
                        match = re.fullmatch(r'"([^"]+)" \(SIZE\)', args)
                        assert self.fast and match
                        send(f'* STATUS "{match[1]}" (SIZE {total(match[1])})\r\n')
                    elif cmd == "EXAMINE":
                        self.selected = args.strip('"')
                        n = count(self.selected)
                        assert n
                        send(f"* {n} EXISTS\r\n* OK [UIDVALIDITY 7] fixed\r\n* OK [UIDNEXT {uid(n) + 1}] next\r\n")
                    elif cmd == "FETCH":
                        match = re.fullmatch(r"(\d+):(\d+) \(UID\)", args)
                        assert match
                        low, high = map(int, match.groups())
                        assert 1 <= low <= high <= count(self.selected) and high - low < 10
                        self.inventory_rows += high - low + 1
                        for pos in range(low, high + 1):
                            send(f"* {pos} FETCH (UID {uid(pos)})\r\n")
                    elif cmd == "UID":
                        if not self.fetch(args, send, source):
                            return
                    elif cmd == "LOGOUT":
                        send("* BYE goodbye\r\n" + tag + " OK logout\r\n")
                        return
                    else:
                        raise AssertionError("Unexpected command: " + cmd)
                    send(tag + " OK done\r\n")
        except BaseException as exc:
            self.error = exc

    def fetch(self, args, send, source):
        match = re.fullmatch(r"FETCH ([\d,]+) (\(.*\))", args)
        assert match
        requested = list(map(int, match[1].split(",")))
        assert 1 <= len(requested) <= 10
        for key in requested:
            pos = (key - 17) // 4093 + 1
            assert uid(pos) == key and 1 <= pos <= count(self.selected)
            n = size(self.selected, pos)
            attrs = match[2]
            if attrs == "(UID RFC822.SIZE)":
                self.size_rows += 1
                send(f"* {pos} FETCH (UID {key} RFC822.SIZE {n})\r\n")
            elif attrs == "(UID RFC822.SIZE INTERNALDATE BODY.PEEK[HEADER.FIELDS (DATE FROM SUBJECT)]<0.8192>)":
                self.header_bytes += len(HEADER)
                send(f'* {pos} FETCH (UID {key} RFC822.SIZE {n} INTERNALDATE "27-Nov-2026 00:00:00 +0000" '
                     f'BODY[HEADER.FIELDS (DATE FROM SUBJECT)]<0> {{{len(HEADER)}}}\r\n')
                send(HEADER + b")\r\n")
            else:
                body = re.fullmatch(r"\(UID RFC822.SIZE INTERNALDATE FLAGS BODY\.PEEK\[\]<(\d+)\.(\d+)>\)", attrs)
                assert body and len(requested) == 1, attrs
                offset, span = map(int, body.groups())
                assert span <= 256 * 1024
                data = piece(self.selected, pos, offset, span)
                send(f'* {pos} FETCH (UID {key} RFC822.SIZE {n} INTERNALDATE "27-Nov-2026 00:00:00 +0000" '
                     f'FLAGS (\\Flagged) BODY[]<{offset}> {{{len(data)}}}\r\n')
                if self.interrupt and pos == 2 and offset == 8 * MIB:
                    data = data[:1024]
                    send(data)
                    self.transfers.append([key, offset, len(data)])
                    self.request_cancel.set()
                    assert source.read(1) == b"", "client did not abandon held literal"
                    return False
                send(data)
                self.transfers.append([key, offset, len(data)])
                send(")\r\n")
        return True
