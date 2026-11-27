"""Exercise the public source/installed shell with disposable loopback accounts."""
import json
import contextlib
import fcntl
import os
from pathlib import Path
import signal
import runpy
import pty
import re
import selectors
import socket
import ssl
import subprocess
import sys
import threading
import termios
import time

repo, work, prefix = map(Path, sys.argv[1:])
sys.path.insert(0, str(repo / "src/zlibs/imap-shell"))
sys.dont_write_bytecode = True
import folders as folderlib
import rename_preview as previewlib
helper = runpy.run_path(str(repo / "src/zlibs/imap-shell/session.py"))
assert helper["server_host"]("[::1]") == "::1"
assert helper["server_host"]("localhost") == "localhost"
try:
    with socket.socket(socket.AF_INET6) as probe:
        probe.bind(("::1", 0))
    ipv6_available = True
except OSError:
    ipv6_available = False
print(f"PASS: bracketed IPv6 normalization; loopback IPv6 available={ipv6_available}")

# Validate the indexed lookup against the straightforward pairwise definition,
# including flat names, trailing namespace delimiters and duplicate roots.
index_roots = dict(enumerate(((b"A", b"/"), (b"A/B", None), (b"A.", b"."),
                              (b"A.B", b"."), (b"AX", b"."), (b"X/", b"/"),
                              (b"X/Y", b"/"), (b"Y", None), (b"Y", b"."))))
index_queries = list(index_roots.values()) + [(b"X/Z", b"/"), (b"Y.Z", b"."), (b"AX/N", None)]
subtree_index = previewlib.Subtrees(index_roots.items())


def subtree_contains(root, delimiter, value):
    return value == root or (delimiter is not None and value.startswith(
        root if root.endswith(delimiter) else root + delimiter))


index_checks = 0
for raw, delimiter in index_queries:
    for excluded in [None, *index_roots]:
        expected = any(owner != excluded and (subtree_contains(root, sep, raw)
                       or subtree_contains(raw, delimiter, root))
                       for owner, (root, sep) in index_roots.items())
        assert subtree_index.related(raw, delimiter, exclude=excluded) == expected
        index_checks += 1
print(f"PASS: {index_checks} indexed subtree overlap/dependency checks")
tls = work / "shell-tls"
tls.mkdir()


def openssl(*args):
    subprocess.run(["openssl", *args], cwd=tls, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=10)


openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=Fixture CA", "-keyout", "ca.key", "-out", "ca.pem",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
for name, host in (("trusted", "localhost"), ("wrong", "wrong.invalid")):
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={host}",
            "-keyout", f"{name}.key", "-out", f"{name}.csr")
    (tls / f"{name}.ext").write_text(f"subjectAltName=DNS:{host}\n")
    openssl("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
            "-CAcreateserial", "-days", "1", "-extfile", f"{name}.ext", "-out", f"{name}.pem")


class Server:
    def __init__(self, mode="implicit", cert="trusted", version="IMAP4rev1", denied=False,
                 login_disabled=False, preauth=False, ipv6=False, folders=None, utf8=False,
                 enable_reply="confirmed", messages=None, header_mode=None, events=None, sizes=None,
                 write_state=None, write_replies=None, write_namespace=b".", write_interrupt=False,
                 write_disconnect=None, subscriptions=None, subscriptions_after=None):
        self.mode, self.version, self.denied = mode, version, denied
        self.login_disabled, self.preauth = login_disabled, preauth
        self.folders, self.utf8 = folders, utf8
        self.enable_reply = enable_reply
        self.messages, self.header_mode = messages or [], header_mode
        self.literal_started = threading.Event()
        self.connections = 0
        self.fetch_ranges = []
        self.header_fetches = []
        self.events = events or {}
        self.sizes = sizes or {}
        self.write_state = None if write_state is None else dict(write_state)
        self.write_replies = write_replies or {}
        self.write_namespace, self.write_interrupt = write_namespace, write_interrupt
        self.write_disconnect = write_disconnect
        self.subscriptions = set(subscriptions or ())
        self.subscriptions_after = subscriptions_after
        self.interrupted_write_closed = threading.Event()
        self.writes = []
        self.current_size = self.sizes
        self.size_fetches = []
        self.validity = 44
        self.header_bytes = 0
        self.flags = {uid: {"\\Flagged"} for uid, _ in self.messages}
        self.patterns = []
        self.wire = []
        self.commands = []
        self.failure = None
        self.conn = None
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_INET6 if ipv6 else socket.AF_INET)
        self.listener.bind(("::1" if ipv6 else "127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(tls / f"{cert}.pem", tls / f"{cert}.key")
        self.thread = threading.Thread(target=self.serve)

    def serve(self):
        for _ in range(2 if self.header_mode in ("cancel", "oversized", "inventory-oversized", "encoding") or self.sizes.get("cancel") or self.write_interrupt or self.write_disconnect else 1):
            self.serve_once()
            if self.failure or self.stop.is_set():
                return

    def serve_once(self):
        try:
            conn, _ = self.listener.accept()
            self.connections += 1
            self.conn = conn
            conn.settimeout(5)
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.mode == "timeout":
                self.stop.wait(5)
                return
            secure = self.mode == "implicit"
            if secure:
                conn = self.context.wrap_socket(conn, server_side=True)
                self.conn = conn
            conn.sendall(b"* PREAUTH fixture ready\r\n" if self.preauth else b"* OK fixture ready\r\n")
            while True:
                line = bytearray()
                while not line.endswith(b"\n"):
                    char = conn.recv(1)
                    if not char:
                        return
                    line.extend(char)
                    assert len(line) < 8192
                tag, command, *args = bytes(line).strip().split(b" ", 2)
                command = command.decode("ascii").upper()
                self.commands.append(command)
                safe_line = tag + b" LOGIN <credentials redacted>\r\n" if command == "LOGIN" else bytes(line)
                self.wire.append({"direction": "client", "command": command, "tls": secure,
                                  "connection": self.connections,
                                  "line": safe_line.decode("utf-8")})

                def send(response):
                    safe = response.replace(b"private-marker-password", b"<redacted>")
                    self.wire.append({"direction": "server", "command": command, "tls": secure,
                                      "connection": self.connections,
                                      "line": safe.decode("utf-8")})
                    conn.sendall(response)

                def send_status(raw, settings):
                    reply = settings.get("status")
                    attributes = b"MESSAGES 0" if reply is None else f"SIZE {reply}".encode()
                    identity = settings.get("reply_raw", raw)
                    if settings.get("literal"):
                        send(b"* STATUS {" + str(len(identity)).encode() + b"}\r\n" + identity
                             + b" (" + attributes + b")\r\n")
                    else:
                        send(b"* STATUS " + folderlib.quoted(identity) + b" (" + attributes + b")\r\n")

                event = self.events.get((command, self.commands.count(command)))
                if event:
                    event(self, send)
                if command == "CAPABILITY":
                    caps = self.version + " UIDPLUS"
                    if self.sizes.get("capability"):
                        caps += " STATUS=SIZE"
                    if self.sizes.get("list_status"):
                        caps += " LIST-STATUS"
                    if self.header_mode == "encoding" and self.connections == 2:
                        caps = "IMAP4rev2 UIDPLUS"
                    if self.mode == "starttls" and not secure:
                        caps += " STARTTLS LOGINDISABLED"
                    elif self.login_disabled:
                        caps += " LOGINDISABLED"
                    if self.utf8:
                        caps += " ENABLE UTF8=ACCEPT"
                    send(f"* CAPABILITY {caps}\r\n".encode() + tag + b" OK capability\r\n")
                elif command == "STARTTLS":
                    send(tag + b" OK begin TLS\r\n")
                    conn = self.context.wrap_socket(conn, server_side=True)
                    self.conn, secure = conn, True
                elif command == "LOGIN":
                    assert args == [b'"fixture user" "private-marker-password"'], "LOGIN quoting differs"
                    if self.denied:
                        send(tag + b" NO private-marker-password rejected\r\n")
                    else:
                        send(tag + b" OK authenticated\r\n")
                        self.ready.set()
                elif command == "LOGOUT":
                    send(b"* BYE goodbye\r\n" + tag + b" OK logout\r\n")
                    return
                elif command in ("CREATE", "RENAME", "DELETE"):
                    assert self.write_state is not None, "unexpected mailbox write"
                    remaining = args[0]
                    operands = []
                    for _ in range(2 if command == "RENAME" else 1):
                        if remaining.startswith(b'"'):
                            raw, end = folderlib.read_quoted(remaining, 0)
                            remaining = remaining[end:]
                        else:
                            assert remaining.startswith(b"{") and remaining.endswith(b"}")
                            length = int(remaining[1:-1])
                            if self.write_interrupt:
                                self.literal_started.set()
                                trailing = conn.recv(8192)
                                assert trailing == b"", "bytes sent after interrupted literal marker: " + repr(trailing)
                                self.interrupted_write_closed.set()
                                return
                            send(b"+ mailbox literal\r\n")
                            raw = bytearray()
                            while len(raw) < length:
                                raw.extend(conn.recv(length - len(raw)))
                            raw = bytes(raw)
                            remaining = bytearray()
                            while not remaining.endswith(b"\r\n"):
                                remaining.extend(conn.recv(1))
                            self.wire.append({"direction": "client", "command": "WRITE_LITERAL", "tls": secure,
                                              "line": (raw + remaining).decode("utf-8")})
                            remaining = bytes(remaining[:-2])
                        operands.append(raw)
                        if remaining.startswith(b" "):
                            remaining = remaining[1:]
                    assert not remaining, "extra write operands"
                    self.writes.append((command, *operands))
                    if self.write_disconnect == "before":
                        return
                    if self.write_disconnect == "hold-before":
                        self.literal_started.set()
                        assert conn.recv(1) == b"", "command sent before interrupted write reconciled"
                        return
                    reply = self.write_replies.get((command, len(self.writes)), self.write_replies.get(command, "OK"))
                    if reply != "OK":
                        send(tag + b" " + reply.encode() + b" [NOPERM] private-marker-password denied\r\n")
                        continue
                    if command == "CREATE":
                        assert operands[0] not in self.write_state
                        self.write_state[operands[0]] = ()
                    elif command == "DELETE":
                        assert operands[0] in self.write_state
                        del self.write_state[operands[0]]
                        for container in sorted(self.sizes.get("auto_containers", ()), key=len, reverse=True):
                            if not any(raw != container and subtree_contains(container, self.write_namespace, raw)
                                       for raw in self.write_state):
                                self.write_state.pop(container, None)
                    else:
                        old, new = operands
                        assert old in self.write_state and new not in self.write_state
                        separator = self.write_namespace
                        boundary = old if separator is not None and old.endswith(separator) else old + (separator or b"")
                        moving = {raw: value for raw, value in self.write_state.items()
                                  if raw == old or (separator is not None and raw.startswith(boundary))}
                        for raw in moving:
                            del self.write_state[raw]
                        self.write_state.update({new + (separator if raw != old and old.endswith(separator) else b"") + raw[len(old):]: value
                                                 for raw, value in moving.items()})
                    if self.subscriptions_after is not None:
                        self.subscriptions = set(self.subscriptions_after)
                    if self.write_disconnect in ("after", "encoding"):
                        if self.write_disconnect == "encoding":
                            self.version = "IMAP4rev2"
                        return
                    if self.write_disconnect == "hold-after":
                        self.literal_started.set()
                        assert conn.recv(1) == b"", "command sent before interrupted write reconciled"
                        return
                    send(tag + b" OK mailbox changed\r\n")
                elif command == "EXAMINE":
                    assert args
                    if args[0].startswith(b"{"):
                        length = int(args[0][1:-1])
                        send(b"+ examine literal\r\n")
                        value = bytearray()
                        while len(value) < length + 2:
                            value.extend(conn.recv(length + 2 - len(value)))
                        assert value[-2:] == b"\r\n"
                        raw = bytes(value[:-2])
                    else:
                        raw, end = folderlib.read_quoted(args[0], 0)
                        assert end == len(args[0])
                    if self.write_state is not None:
                        assert raw in self.write_state
                        self.messages = [(uid, b"") for uid in self.write_state[raw]]
                    if "mailboxes" in self.sizes:
                        self.current_size = self.sizes["mailboxes"][raw]
                        self.messages = list(self.current_size.get("messages", self.messages if self.write_state is not None else []))
                    if "raw" in self.sizes:
                        assert args[0] == folderlib.quoted(self.sizes["raw"])
                    if self.current_size.get("denied"):
                        send(tag + b" NO inaccessible\r\n")
                        continue
                    if self.header_mode == "encoding":
                        assert self.connections == 1, "cached raw name reused after encoding changed"
                    uidnext = max((uid for uid, _ in self.messages), default=0) + 1
                    send(f"* {len(self.messages)} EXISTS\r\n* OK [UIDVALIDITY {self.validity}] generation\r\n* OK [UIDNEXT {uidnext}] next\r\n".encode()
                         + tag + b" OK [READ-ONLY] examined\r\n")
                elif command == "STATUS":
                    raw, end = folderlib.read_quoted(args[0], 0)
                    assert args[0][end:] == b" (SIZE)"
                    settings = self.sizes.get("mailboxes", {}).get(raw, self.sizes)
                    if "mailboxes" not in self.sizes:
                        assert raw == self.sizes.get("raw", b"Inbox")
                    if self.sizes.get("cancel") and self.connections == 1 and self.commands.count("STATUS") == 2:
                        send(b'* STATUS "unfinished" (SIZE ')
                        self.literal_started.set()
                        assert conn.recv(1) == b"", "command sent on interrupted size response"
                        return
                    if self.sizes.get("scan_failure") and self.commands.count("STATUS") == 2:
                        send(b"* BYE scan aborted\r\n" if self.sizes["scan_failure"] == "abort"
                             else b'* STATUS "unfinished" (SIZE ')
                        assert conn.recv(1) == b"", "command sent after abnormal size response"
                        return
                    reply = settings.get("status")
                    if reply == "denied":
                        send(tag + b" NO inaccessible\r\n")
                    else:
                        send_status(raw, settings)
                        send(tag + b" OK status\r\n")
                elif command in ("FETCH", "UID"):
                    operands = args[0]
                    if command == "UID":
                        assert operands.startswith(b"FETCH ")
                        operands = operands[6:]
                    sequence, attrs = operands.split(b" ", 1)
                    if command == "FETCH":
                        assert attrs == b"(UID)", "sequence FETCH must only probe UID metadata"
                        if self.header_mode == "inventory-oversized" and self.connections == 1:
                            send(b"* 1 FETCH (UID 1 BODY[] {999999999}\r\n")
                            assert conn.recv(1) == b"", "client buffered unexpected inventory literal"
                            return
                        start, end = map(int, sequence.split(b":"))
                        assert 1 <= start <= end and end - start < 10
                        self.fetch_ranges.append((start, end))
                        for index in range(start, min(end, len(self.messages)) + 1):
                            send(f"* {index} FETCH (UID {self.messages[index - 1][0]})\r\n".encode())
                        send(tag + b" OK inventory\r\n")
                        continue
                    requested = list(map(int, sequence.split(b",")))
                    assert len(requested) <= 10 and len(requested) == len(set(requested))
                    if attrs == b"(UID RFC822.SIZE)":
                        self.size_fetches.append(requested)
                        if self.current_size.get("fetch_denied"):
                            send(tag + b" NO size unavailable\r\n")
                            continue
                        for index, (uid, _) in enumerate(self.messages, 1):
                            if uid in requested:
                                size = self.current_size.get("values", {}).get(uid, uid * 1024)
                                if size is not None:
                                    send(f"* {index} FETCH (UID {uid} RFC822.SIZE {size})\r\n".encode())
                        send(tag + b" OK measured\r\n")
                        continue
                    assert attrs == b"(UID RFC822.SIZE INTERNALDATE BODY.PEEK[HEADER.FIELDS (DATE FROM SUBJECT)]<0.8192>)"
                    self.header_fetches.append(requested)
                    indices = [index for index, (uid, _) in enumerate(self.messages, 1) if uid in requested]
                    start = indices[0] if indices else 0
                    if self.header_mode == "rejected" and len(self.header_fetches) == 1:
                        send(tag + b" NO unavailable right now\r\n")
                        continue
                    for index in indices:
                        uid, raw = self.messages[index - 1]
                        literal = raw[:8192]
                        metadata = (f'* {index} FETCH (UID {uid} RFC822.SIZE {uid * 1024} '
                                    'INTERNALDATE "02-Jan-2020 03:04:05 +0000" '
                                    'BODY[HEADER.FIELDS (DATE FROM SUBJECT)]<0> ').encode()
                        if self.header_mode == "oversized" and self.connections == 1:
                            send(metadata + b"{999999999}\r\n")
                            assert conn.recv(1) == b"", "client buffered an oversized literal"
                            return
                        if self.header_mode in ("cancel", "encoding") and self.connections == 1:
                            send(metadata + b"{8192}\r\nDate: ")
                            self.literal_started.set()
                            assert conn.recv(1) == b"", "client sent command on partial literal connection"
                            return
                        if self.header_mode == "malformed" and len(self.header_fetches) == 1:
                            metadata = metadata.replace(f"UID {uid} ".encode(), b"")
                        suffix = b")\r\n"
                        if self.header_mode in ("suffix", "empty"):
                            metadata = f'* {index} FETCH (BODY[HEADER.FIELDS (DATE FROM SUBJECT)]<0> '.encode()
                            suffix = (f' UID {uid} RFC822.SIZE {uid * 1024} '
                                      'INTERNALDATE "02-Jan-2020 03:04:05 +0000")\r\n').encode()
                            if self.header_mode == "empty":
                                if index == start:
                                    send(metadata + b"NIL" + suffix)
                                    continue
                                literal = b""
                        self.header_bytes += len(literal)
                        send(metadata + b"{" + str(len(literal)).encode() + b"}\r\n" + literal + suffix)
                        if self.header_mode == "suffix":
                            send(f"* {index} FETCH (FLAGS (\\Flagged))\r\n".encode())
                    send(tag + b" OK fetched\r\n")
                elif command == "ENABLE":
                    assert self.utf8 and args == [b"UTF8=ACCEPT"]
                    if self.enable_reply == "confirmed":
                        send(b"* ENABLED UTF8=ACCEPT\r\n" + tag + b" OK enabled\r\n")
                    elif self.enable_reply == "empty":
                        send(b"* ENABLED\r\n" + tag + b" OK no extensions enabled\r\n")
                    else:
                        send(tag + b" NO enable refused\r\n")
                elif command in ("LIST", "LSUB"):
                    if command == "LSUB" and self.version == "IMAP4rev2":
                        send(tag + b" BAD legacy LSUB unavailable\r\n")
                        continue
                    assert self.folders is not None or self.write_state is not None, "unexpected LIST"
                    subscribed = command == "LIST" and args[0].startswith(b"(SUBSCRIBED) ")
                    if subscribed:
                        args[0] = args[0][len(b"(SUBSCRIBED) "):]
                    assert args[0].startswith(b'"" '), "LIST reference must be empty"
                    pattern = args[0][3:]
                    status_return = False
                    if pattern.startswith(b"{"):
                        length = int(pattern[1:-1])
                        send(b"+ send literal\r\n")
                        literal = bytearray()
                        while len(literal) < length + 2:
                            literal.extend(conn.recv(length + 2 - len(literal)))
                        assert literal[-2:] == b"\r\n"
                        pattern = bytes(literal[:-2])
                        self.wire.append({"direction": "client", "command": "LIST_LITERAL", "tls": secure,
                                          "line": bytes(literal).decode("utf-8")})
                    else:
                        pattern, end = folderlib.read_quoted(pattern, 0)
                        remainder = args[0][3:][end:]
                        assert remainder in (b"", b" RETURN (STATUS (SIZE))")
                        status_return = bool(remainder)
                        if status_return:
                            assert self.sizes.get("list_status") or self.version == "IMAP4rev2"
                    assert self.write_state is not None or pattern in self.folders, "unexpected LIST pattern " + repr(pattern)
                    if command == "LIST":
                        self.patterns.append(pattern)
                    if pattern in self.sizes.get("list_denied", ()):
                        send(tag + b" NO subtree inaccessible\r\n")
                        continue
                    if status_return and self.sizes.get("reject_extended"):
                        # Data from a rejected extended command must not leak
                        # into the ordinary LIST or its per-folder fallback.
                        send(b'* LIST () NIL "Rejected"\r\n* STATUS "A" (SIZE 999999999)\r\n')
                        send(tag + b" " + self.sizes["reject_extended"].encode() + b" unsupported return option\r\n")
                        continue
                    if status_return and self.sizes.get("extended_abort"):
                        send(b"* BYE extended LIST aborted\r\n")
                        assert conn.recv(1) == b"", "ordinary LIST retried after abort"
                        return
                    if self.write_state is not None:
                        expression = re.escape(pattern).replace(rb"\*", b".*").replace(b"%", b".*" if self.write_namespace is None else b"[^" + re.escape(self.write_namespace) + b"]*")
                        names = self.subscriptions if command == "LSUB" or subscribed else self.write_state
                        if subscribed and self.sizes.get("inject_unsubscribed"):
                            names = self.subscriptions | self.write_state.keys()
                        values = [(b"", self.write_namespace, (), False)] if pattern == b"" else [
                            (raw, self.write_namespace, (b"\\Subscribed",) if subscribed and raw in self.subscriptions
                             else self.sizes.get("write_flags", {}).get(raw, ()),
                             b"\n" in raw or b"\r" in raw)
                            for raw in names if re.fullmatch(expression, raw, re.DOTALL)
                            or (self.write_namespace is not None and raw.endswith(self.write_namespace)
                                and re.fullmatch(expression, raw[:-len(self.write_namespace)], re.DOTALL))]
                    else:
                        values = self.folders[pattern]
                    if command == "LIST" and pattern in self.sizes.get("probe_rows", {}):
                        values = self.sizes["probe_rows"][pattern]
                    if callable(values):
                        values = values(self.patterns.count(pattern))
                    for raw, delimiter, flags, literal in values:
                        header = b"* " + command.encode() + b" (" + b" ".join(flags) + b") "
                        header += b"NIL " if delimiter is None else folderlib.quoted(delimiter) + b" "
                        extension = b' (CHILDINFO ("SUBSCRIBED"))' if raw.startswith(b"Archive") else b""
                        if literal:
                            send(header + b"{" + str(len(raw)).encode() + b"}\r\n" + raw + extension + b"\r\n")
                        else:
                            send(header + folderlib.quoted(raw) + extension + b"\r\n")
                        if status_return and not {flag.upper() for flag in flags}.intersection((b"\\NOSELECT", b"\\NONEXISTENT")):
                            settings = self.sizes.get("mailboxes", {}).get(raw, self.sizes)
                            if raw not in self.sizes.get("omit", ()) and settings.get("status") != "denied":
                                send_status(raw, settings)
                    send(tag + b" OK listed\r\n")
                else:
                    raise AssertionError("unexpected wire command " + command)
        except (OSError, ssl.SSLError):
            pass
        except BaseException as exc:
            self.failure = exc
        finally:
            if self.conn:
                self.conn.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.stop.set()
        if self.conn:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(6)
        assert not self.thread.is_alive(), "server did not settle"
        if self.failure:
            raise self.failure


results = []
audit = work / "audit-python"
audit.write_text('''#!/usr/bin/env python3
import json, os, runpy, sys
from pathlib import Path
if sys.argv[1] == '-c':
    exec(sys.argv[2])
else:
    markers = ('private-marker-password', 'fixture user')
    exposed = any(marker in repr(sys.argv) + repr(dict(os.environ)) for marker in markers)
    Path(__file__).with_suffix('.json').write_text(json.dumps({'secretExposed': exposed}))
    sys.path.insert(0, str(Path(sys.argv[1]).parent))
    runpy.run_path(sys.argv[1], run_name='__main__')
''')
audit.chmod(0o700)
pinentry = work / "bin/pinentry"
pinentry.write_text('''#!/usr/bin/env python3
import sys
assert 'GETPIN' in sys.stdin.read()
print('D private-marker-password')
print('OK')
''')
pinentry.chmod(0o700)


@contextlib.contextmanager
def client_process(argv, env, stdin, control_tty=None):
    def acquire_terminal():
        os.setsid()
        fcntl.ioctl(control_tty, termios.TIOCSCTTY, 0)
    process = subprocess.Popen(argv, env=env, stdin=stdin, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=control_tty is None,
                               preexec_fn=acquire_terminal if control_tty is not None else None,
                               pass_fds=(control_tty,) if control_tty is not None else ())
    try:
        yield process
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)


def apply_dialogue(process, master, server, steps):
    output, terminal, identifiers = bytearray(), bytearray(), []

    def read_until(fd, target, offset, predicate):
        limit = time.monotonic() + 10
        with selectors.DefaultSelector() as selector:
            selector.register(fd, selectors.EVENT_READ)
            while not predicate(bytes(target[offset:])):
                remaining = limit - time.monotonic()
                assert remaining > 0 and selector.select(remaining), "apply dialogue deadline"
                data = os.read(fd, 4096)
                assert data, "apply dialogue closed early"
                target.extend(data)

    for step in steps:
        if "preview" in step:
            offset = len(output)
            process.stdin.write(step["preview"] + b"\n")
            process.stdin.flush()
            read_until(process.stdout.fileno(), output, offset,
                       lambda data: re.search(br"Preview ID: ([0-9a-f]+-[0-9]+)\n", data))
            identifiers.append(re.search(br"Preview ID: ([0-9a-f]+-[0-9]+)\n", output[offset:])[1])
        elif "commands" in step:
            process.stdin.write(step["commands"])
            process.stdin.flush()
        elif "hook" in step:
            step["hook"](server)
        elif "delete" in step:
            offset, terminal_offset = len(output), len(terminal)
            previous_writes = list(server.writes)
            process.stdin.write(step["delete"] + b"\n")
            process.stdin.flush()
            if master is not None and not step.get("no_prompt"):
                read_until(master, terminal, terminal_offset, lambda data: b"[no] Type yes: " in data)
                assert b"Delete /" in terminal[terminal_offset:]
                assert server.writes == previous_writes, "DELETE before consent"
                if "after_prompt" in step:
                    step["after_prompt"](server)
                if step.get("answer") == "interrupt-prompt":
                    os.killpg(process.pid, signal.SIGINT)
                else:
                    os.write(master, b"yes\n" if step.get("answer") == "interrupt-write" else step.get("answer", b"yes\n"))
                    if step.get("answer") == "interrupt-write":
                        assert server.literal_started.wait(5), "DELETE did not reach held reply"
                        os.killpg(process.pid, signal.SIGINT)
            elif master is None and not step.get("no_prompt"):
                process.stdin.write(b"yes\n")
            process.stdin.write(b"help\n")
            process.stdin.flush()
            read_until(process.stdout.fileno(), output, offset, lambda data: b"Commands: " in data)
        else:
            identifier = identifiers[step["apply"]]
            offset, terminal_offset = len(output), len(terminal)
            process.stdin.write(b"apply " + identifier + b"\n")
            if master is None:
                process.stdin.write(b"yes\n")
            process.stdin.flush()
            if not step.get("reject") and not step.get("no_prompt") and master is not None:
                read_until(master, terminal, terminal_offset, lambda data: b"Type yes: " in data)
                assert b"rename " in terminal[terminal_offset:]
                if "after_prompt" in step:
                    step["after_prompt"](server)
                if step.get("answer") == "interrupt-prompt":
                    os.killpg(process.pid, signal.SIGINT)
                else:
                    os.write(master, step.get("answer", b"yes\n") if step.get("answer") != "interrupt-write" else b"yes\n")
                    if step.get("answer") == "interrupt-write":
                        assert server.literal_started.wait(5), "write did not commit before interruption"
                        os.killpg(process.pid, signal.SIGINT)
            marker = b"create a fresh preview\n" if step.get("reject") else b"Fresh preview required before another apply\n"
            read_until(process.stdout.fileno(), output, offset, lambda data: marker in data)
    return bytes(output), bytes(terminal)


def run(name, installed=False, mode="implicit", cert="trusted", policy="check", ca=True,
        version="IMAP4rev1", denied=False, expected=0, commands=b"help\nhelp\nquit\n", interrupt=False,
        interpreter=None, debug=False, login_disabled=False, preauth=False, ipv6=False,
        prompt=False, terminal=False, folders=None, utf8=False, expected_out=None, patterns=None,
        expected_err=(), enable_reply="confirmed", enable_count=None, messages=None, header_mode=None,
        expected_uids=None, expected_contains=(), expected_subjects=None, events=None, max_inventory=None,
        sizes=None, size_uids=None, status_count=None, du_rows=None, du_total=None, list_status_count=None,
        du_labels=(), du_cancel=False, write_state=None, write_replies=None,
        expected_writes=None, final_state=None, write_namespace=b".", write_interrupt=False,
        write_disconnect=None, subscriptions=None, subscriptions_after=None, final_subscriptions=None,
        preview_rows=None, preview_blocked=False, preview_errors=(), apply_steps=None,
        apply_tty=True, apply_states=None, delete_targets=None, delete_results=None):
    with Server(mode, cert, version, denied, login_disabled, preauth, ipv6, folders, utf8, enable_reply,
                messages, header_mode, events, sizes, write_state, write_replies, write_namespace, write_interrupt,
                write_disconnect, subscriptions, subscriptions_after) as server:
        account = work / "mail/Accounts/shell-fixture"
        account.write_text(f'''name fixture
email fixture@example.org
login "fixture user"
{"" if prompt else "password private-marker-password"}
proto imap
imap {"[::1]" if ipv6 else "localhost"}
imap_port {server.port}
transport {"starttls" if mode in ("starttls", "no-starttls") else "plain" if mode == "plain" else "implicit"}
cert {policy}
certfile {tls / 'ca.pem' if ca else tls / 'wrong.pem'}
''')
        env = dict(os.environ, JAROMAILDIR=str(work / "mail"), JARO_IMAP_TIMEOUT="10" if events or len(messages or []) > 100 else "1",
                   password="private-marker-password", login="fixture user",
                   JAROWORKDIR=str(prefix / "share/jaromail" if installed else repo / "src"))
        env["JARO_IMAP_PYTHON"] = interpreter or str(audit)
        entry = prefix / "bin/jaro" if installed else repo / "src/jaro"
        argv = [str(entry), "-D" if debug else "-q", "-a", "shell-fixture", "imap", "shell"]
        master, tty_slave = pty.openpty() if terminal or (apply_steps and apply_tty) else (None, None)
        slave = tty_slave if terminal else subprocess.PIPE
        captured, tty_output = b"", b""
        try:
            with client_process(argv, env, slave, tty_slave if apply_steps and apply_tty else None) as process:
                if apply_steps:
                    captured, tty_output = apply_dialogue(process, master, server, apply_steps)
                if write_interrupt:
                    process.stdin.write(commands)
                    process.stdin.flush()
                    assert server.literal_started.wait(5), "mailbox literal marker did not arrive"
                    os.killpg(process.pid, signal.SIGINT)
                    commands = b"pwd\nquit\n"
                if du_cancel:
                    process.stdin.write(commands)
                    process.stdin.flush()
                    assert server.literal_started.wait(5), "size scan did not reach held response"
                    os.killpg(process.pid, signal.SIGINT)
                    commands = b"pwd\ndu A\nquit\n"
                if header_mode in ("cancel", "encoding"):
                    process.stdin.write(b"cd Caf\xc3\xa9\nls --messages\n" if header_mode == "encoding"
                                        else b"cd Inbox\nls --messages\n")
                    process.stdin.flush()
                    assert server.literal_started.wait(5), "header literal did not begin"
                    os.killpg(process.pid, signal.SIGINT)
                    commands = b"pwd\nls --messages\nquit\n"
                if interrupt:
                    assert server.ready.wait(5), "login did not finish"
                    # Wait for the post-login CAPABILITY before cancellation.
                    limit = time.monotonic() + 3
                    while server.commands.count("CAPABILITY") < 2 and time.monotonic() < limit:
                        time.sleep(.01)
                    os.killpg(process.pid, signal.SIGINT)
                if terminal:
                    os.write(master, b"help\n\x04")
                out, err = process.communicate(None if terminal else commands, timeout=20)
                out = captured + out
        finally:
            if master is not None:
                os.close(master)
                os.close(tty_slave)
        assert process.returncode == expected, (name, process.returncode, server.wire, err[-300:].decode(errors="replace"))
        assert b"private-marker-password" not in out + err, "secret in diagnostics"
        assert b"fixture user" not in out + err, "login identifier in diagnostics"
        assert b"shell-fixture" not in out + err, "account identifier in diagnostics"
        if not interpreter:
            assert json.loads(audit.with_suffix('.json').read_text()) == {"secretExposed": False}
        assert not (Path(env["HOME"]) / ".python_history").exists()
        if expected == 0 or interrupt:
            assert server.commands.count("LOGIN") == (2 if du_cancel or write_interrupt or write_disconnect or header_mode in ("cancel", "oversized", "inventory-oversized", "encoding") else 1), server.commands
            assert server.commands[-1] == "LOGOUT", server.commands
        if expected == 0:
            assert server.commands.count("CAPABILITY") == (4 if du_cancel or write_interrupt or write_disconnect or header_mode in ("cancel", "oversized", "inventory-oversized", "encoding") else 3 if mode == "starttls" else 2)
            assert not any(c not in ("CAPABILITY", "STARTTLS", "LOGIN", "LOGOUT", "LIST", "LSUB", "ENABLE", "EXAMINE", "FETCH", "UID", "STATUS", "CREATE", "RENAME", "DELETE") for c in server.commands)
        if expected_writes is not None:
            assert server.writes == expected_writes, (name, server.writes, expected_writes, err.decode())
            assert not set(server.commands).intersection(("FETCH", "UID", "COPY", "EXPUNGE"))
        if final_state is not None:
            assert server.write_state == final_state, (name, server.write_state, final_state)
        if apply_states is not None:
            statuses = [tuple(line.decode().split("\t")) for line in out.splitlines()
                        if line.split(b"\t", 1)[0] in (b"completed", b"failed", b"unknown", b"pending")]
            assert statuses == [(state, old, "->", new) for state, old, new in apply_states], (name, statuses, out, err)
            assert b"Fresh preview required" in out
            assert (b"Type yes: " in tty_output) == apply_tty
        if delete_targets is not None:
            target_rows = [line.decode().split("\t") for line in out.splitlines() if line.startswith(b"target\t")]
            assert [(row[1], row[2]) for row in target_rows] == delete_targets, (name, target_rows)
        if delete_results is not None:
            result_rows = [line.decode().split("\t") for line in out.splitlines()
                           if line.split(b"\t", 1)[0] in (b"removed", b"surviving", b"unknown", b"absent")]
            assert [(row[0], row[1]) for row in result_rows] == delete_results, (name, result_rows, out, err)
        if preview_rows is not None:
            rows = [line.split("\t") for line in out.decode().splitlines() if "\t->\t" in line]
            assert [(row[0], row[2], row[3]) for row in rows] == [
                (old, new, f"subtree: {count} listed mailboxes" if count is not None else "subtree: scope unavailable")
                for old, new, count in preview_rows
            ], (name, rows, preview_rows, err.decode())
            assert not set(server.commands).difference(("CAPABILITY", "LOGIN", "LOGOUT", "ENABLE", "LIST")), server.commands
            assert not server.writes and not server.header_fetches and not server.size_fetches
            assert (b"Apply blocked" in out or b"Rename preview blocked:" in err) == preview_blocked, (name, out, err)
            if not preview_blocked:
                assert f"Rename preview: {len(preview_rows)} matches".encode() in out
                assert b"Preview only; no changes made" in out
            for cause in preview_errors:
                assert cause.encode() in out + err, (name, cause, out, err)
            assert b"\x1b" not in out and b"\xc2\x9b" not in out
        if final_subscriptions is not None:
            assert server.subscriptions == set(final_subscriptions), (name, server.subscriptions)
            assert "SUBSCRIBE" not in server.commands and "UNSUBSCRIBE" not in server.commands
            if version == "IMAP4rev2":
                assert "LSUB" not in server.commands, "rev2-only subscription discovery used LSUB"
                assert any("LIST (SUBSCRIBED)" in e["line"] for e in server.wire if e["direction"] == "client")
        if write_interrupt:
            assert server.interrupted_write_closed.wait(5), "literal connection did not close cleanly"
            assert server.connections == 2 and server.commands.count("CREATE") == 1
        if write_interrupt or write_disconnect:
            assert b"outcome unknown" in err and (apply_steps or b"confirmed" not in err), (name, err)
            assert not any(e["command"] == "LOGOUT" and e.get("connection") == 1 for e in server.wire)
            assert sum(c in ("CREATE", "RENAME", "DELETE") for c in server.commands) == 1, "unknown write replayed"
            if write_disconnect == "encoding":
                assert b"encoding changed" in err
                assert not any(e["command"] in ("LIST", "LSUB") and e.get("connection") == 2 for e in server.wire)
        if write_state == {}:
            assert server.patterns[:2] == [b"%", b""], "empty root delimiter was not discovered"
        if expected_out is not None:
            assert out.decode("utf-8") == expected_out, (name, out.decode("utf-8"), expected_out)
        for message in expected_err:
            assert message.encode() in err, (name, "missing expected diagnostic")
        for message in expected_contains:
            assert message.encode() in out + err, (name, message, out, err)
        if patterns is not None:
            assert server.patterns == patterns, (name, server.patterns, patterns)
        if enable_count is not None:
            assert server.commands.count("ENABLE") == enable_count
        assert "SELECT" not in server.commands and "STORE" not in server.commands
        if size_uids is not None:
            assert [uid for batch in server.size_fetches for uid in batch] == size_uids
            assert not server.header_fetches, "du requested body/header attributes"
            if not size_uids:
                assert not server.fetch_ranges, "server size fast path issued FETCH"
        if status_count is not None:
            assert server.commands.count("STATUS") == status_count, (name, server.commands.count("STATUS"), status_count, err.decode())
        if list_status_count is not None:
            assert sum(item["direction"] == "client" and "RETURN (STATUS (SIZE))" in item["line"] for item in server.wire) == list_status_count
        if du_rows is not None:
            table = [line.decode().split("\t") for line in out.splitlines() if line.startswith(b"/") and len(line.split(b"\t")) == 7]
            assert [(row[0], row[1], int(row[2])) for row in table] == du_rows, (name, table)
            assert not server.header_fetches, "recursive du requested message headers"
            assert b"shared/labeled copies may count twice" in out
            assert b"du: scanned" in err or not any(row[1] != "-" for row in table)
            for label in du_labels:
                assert label.encode() in out, (name, label, out)
            if du_total is not None:
                assert f"TOTAL\t{du_total} mailbox bytes".encode() in out, (name, out)
        if du_cancel:
            assert b"Scan interrupted; incomplete results retained" in out
            assert b"Size operation cancelled; connection renewed" in err
            assert b"/\n/A\t10 bytes\tserver-reported\n" in out
            assert server.connections == 2
        if sizes and sizes.get("scan_failure"):
            assert b"Scan failed; incomplete results retained" in out
            assert b"du: scanned 1 folders; incomplete" in err and b"finished" not in err
            assert server.commands.count("LOGIN") == 1 and "LOGOUT" not in server.commands
        if sizes and sizes.get("extended_abort"):
            assert b"Scan failed; incomplete results retained" in out
            assert b"du: scanned 0 folders; incomplete" in err and b"finished" not in err
            assert server.commands.count("LIST") == 1 and "LOGOUT" not in server.commands
        if expected_uids is not None:
            rows = [line.split(b"\t") for line in out.splitlines() if line[:1].isdigit()]
            assert [int(row[0]) for row in rows] == expected_uids, (name, rows)
            assert server.header_bytes <= len(expected_uids) * 8192
            assert all(len(row) == 5 for row in rows)
            if expected_subjects is not None:
                assert {int(row[0]): row[4].decode() for row in rows} == expected_subjects
            assert b"\x1b" not in out and b"\xc2\x9b" not in out
            assert server.flags == {uid: {"\\Flagged"} for uid, _ in server.messages}
            for value in expected_contains:
                assert value.encode() in out, (name, "missing header field", value)
            if len(messages or []) == 65 and events is None:
                assert server.fetch_ranges == [(16, 25), (26, 35), (36, 45), (46, 55), (56, 65)]
            if max_inventory is not None:
                assert len(server.fetch_ranges) <= max_inventory, (name, server.fetch_ranges)
            if header_mode in ("cancel", "oversized", "inventory-oversized"):
                assert b"/Inbox\n" in out, "command immediately after cancellation did not run"
                assert server.connections == 2
            if header_mode in ("rejected", "malformed"):
                assert server.connections == 1 and server.commands.count("FETCH") == 2
                assert b"/Inbox\n" in out
        if expected and not denied and not interrupt and not write_interrupt and not (sizes and (sizes.get("scan_failure") or sizes.get("extended_abort"))):
            assert "LOGIN" not in server.commands, server.commands
        if mode == "starttls":
            assert all(x["tls"] for x in server.wire if x["command"] == "LOGIN")
        if policy == "ignore" and expected == 0:
            assert b"NOT verified" in err
        if mode == "plain":
            assert b"plaintext" in err
        results.append({"name": name, "exit": process.returncode, "wire": server.wire, "installed": installed,
                        "mailbox_state": None if server.write_state is None else
                        {raw.hex(): list(value) for raw, value in server.write_state.items()},
                        "subscriptions": sorted(raw.hex() for raw in server.subscriptions),
                        "stdout": out.decode("utf-8"), "stderr": err.decode("utf-8"), "terminal": tty_output.decode("utf-8")})
        target = Path(os.environ.get("JARO_TEST_LOG_DIR", work)) / "imap-shell-wire.json"
        target.write_text(json.dumps(results, indent=2) + "\n")
    print("PASS: IMAP shell " + name)


run("source persistent rev1", debug=True)
run("installed persistent rev2", installed=True, version="IMAP4rev2")
run("credential prompt", prompt=True, debug=True)
run("terminal Ctrl-D", terminal=True)
run("STARTTLS", mode="starttls")
run("wrong hostname", cert="wrong", expected=1)
run("wrong CA", ca=False, expected=1)
run("login denied", denied=True, expected=1)
run("STARTTLS required", mode="no-starttls", expected=1)
run("LOGIN disabled", login_disabled=True, expected=1)
run("STARTTLS cleartext PREAUTH rejected", mode="starttls", preauth=True, expected=1)
run("deadline", mode="timeout", expected=124)
run("cert ignore", cert="wrong", policy="ignore")
run("explicit plaintext", mode="plain")
if ipv6_available:
    run("bracketed IPv6 plaintext", mode="plain", ipv6=True)
run("EOF", commands=b"help\n")
sentinel = work / "never-execute"
run("quoted and unevaluated input", commands=f'"help"\nhelp "$(touch {sentinel})"\n"unterminated\nquit\n'.encode())
assert not sentinel.exists(), "command substitution executed"
run("Ctrl-C", interrupt=True, commands=None, expected=130)
run("missing interpreter", interpreter="/nonexistent/jaro-python", expected=1)

# Mixed namespaces deliberately include unrelated/deeper replies to verify that
# exact identity/depth checks follow wildcard LIST matching.
def mailbox(raw, delimiter=None, flags=(), literal=False):
    return raw, delimiter, flags, literal


mixed = {
    b"%": [mailbox(b"Projects", b"/", (b"\\Noselect",)), mailbox(b"Archive", b"."),
            mailbox(b"A*%", b"/", (b"\\Noselect",)), mailbox(b"flat/slash"), mailbox(b"."), mailbox(b".."),
            mailbox(b"Work projects"), mailbox(b'quote"name'), mailbox(b"line\nbreak", literal=True),
            mailbox(b"Caf&AOk-"), mailbox(b"amp&-ersand"), mailbox(b"Projects/hidden", b"/")],
    b"Projects/%": [mailbox(b"Projects/Leaf", b"/"), mailbox(b"Projects/.", b"/"),
                     mailbox(b"Projects/..", b"/"), mailbox(b"Projects/deep/hidden", b"/"),
                     mailbox(b"Other/Leaf", b"/")],
    b"Archive.%": [mailbox(b"Archive.Leaf", b"."), mailbox(b"Archive.folder/with/slash", b".", literal=True)],
    b"A*%/%": lambda count: [mailbox(b"A*%/" + (b"Leaf" if count == 1 else b"NewLeaf"), b"/"),
                             mailbox(b"AX/Spy", b"/")],
}
root_listing = '%2E\n%2E%2E\nA%2A%25 [container]\nArchive\nCafé\nProjects [container]\nWork%20projects\namp&ersand\nflat%2Fslash\nline%0Abreak\nquote%22name\n'
run("mixed shallow folders", folders=mixed,
    commands=b'ls\ncd Projects\npwd\nls --folders\nls --messages\ncd Leaf\npwd\nls --messages\ncd /Archive\nfolders\ncd folder%2Fwith%2Fslash\npwd\ncd /A%2A%25\nls\nrefresh\nls\ncd /flat%2Fslash\nls\npwd\ncd /%2E\npwd\ncd /%2E%2E\npwd\ncd /\nls\nquit\n',
    expected_out=root_listing + '/Projects\n%2E\n%2E%2E\nLeaf\n/Projects/Leaf\nLeaf\nfolder%2Fwith%2Fslash\n/Archive/folder%2Fwith%2Fslash\nLeaf\nNewLeaf\nNewLeaf\n/flat%2Fslash\n/%2E\n/%2E%2E\n' + root_listing,
    expected_err=("Choose a selectable folder",),
    patterns=[b"%", b"Projects/%", b"Archive.%", b"A*%/%", b"A*%/%"])
utf8_folders = {b"%": [mailbox("旅行".encode(), b"/", (b"\\Noselect",), True)],
                "旅行/%".encode(): [mailbox("旅行/Étage".encode(), b"/", literal=True)]}
unicode_commands = 'folders\ncd 旅行\nls\ncd Étage\npwd\ncd ..\nrefresh\nquit\n'.encode()
unicode_output = '旅行 [container]\nÉtage\n/旅行/Étage\nÉtage\n'
for version, utf8 in (("IMAP4rev1", True), ("IMAP4rev2", False)):
    run("Unicode folder paths " + version, installed=True, version=version, utf8=utf8,
        folders=utf8_folders, commands=unicode_commands, expected_out=unicode_output,
        patterns=[b"%", "旅行/%".encode(), "旅行/%".encode()])

control_folders = {b"%": [mailbox(b"line\nbreak", b"/", (b"\\Noselect",), True)],
                   b"line\nbreak/%": [mailbox(b"line\nbreak/Leaf", b"/", literal=True)]}
run("literal LIST pattern", folders=control_folders,
    commands=b'cd line%0Abreak\nls\npwd\nquit\n', expected_out='Leaf\n/line%0Abreak\n',
    patterns=[b"%", b"line\nbreak/%"])
escaped = {b"%": [mailbox(b"Shared/", b"/", (b"\\Noselect",)), mailbox(b"Bob's?[]"),
                   mailbox(b"control&AJs-", literal=True)],
           b"Shared/%": [mailbox(b"Shared/Leaf", b"/"), mailbox(b"SharedOther/Leaf", b"/")]}
run("namespace prefix and escaped components", folders=escaped,
    commands=b"ls\ncd Bob%27s%3F%5B%5D\npwd\ncd /control%C2%9B\npwd\ncd /Shared\nls\ncd Leaf\npwd\ncd /not-found\npwd\ncd /bad%escape\npwd\nquit\n",
    expected_out="Bob%27s%3F%5B%5D\nShared [container]\ncontrol%C2%9B\n/Bob%27s%3F%5B%5D\n/control%C2%9B\nLeaf\n/Shared/Leaf\n/Shared/Leaf\n/Shared/Leaf\n",
    patterns=[b"%", b"Shared/%"])

dual_folders = {b"%": [mailbox(b"Caf&AOk-", b"/")],
                b"Caf&AOk-/%": [mailbox(b"Caf&AOk-/Leaf", b"/")]}
dual_commands = 'folders\ncd Café\nls\npwd\nquit\n'.encode()
dual_output = 'Café\nLeaf\n/Café\n'
run("dual rev1 rev2 retains modified UTF-7", version="IMAP4rev1 IMAP4rev2",
    folders=dual_folders, commands=dual_commands, expected_out=dual_output,
    patterns=[b"%", b"Caf&AOk-/%"], enable_count=0)
for reply in ("empty", "refused"):
    run("dual UTF8 enable fallback " + reply, version="IMAP4rev1 IMAP4rev2", utf8=True,
        enable_reply=reply, folders=dual_folders, commands=dual_commands, expected_out=dual_output,
        patterns=[b"%", b"Caf&AOk-/%"], enable_count=1)
run("dual confirmed UTF8 enable", version="IMAP4rev1 IMAP4rev2", utf8=True,
    folders=utf8_folders, commands=unicode_commands, expected_out=unicode_output,
    patterns=[b"%", "旅行/%".encode(), "旅行/%".encode()], enable_count=1)
missing = {b"%": [mailbox(b"Missing", b"/", (b"\\NonExistent", b"\\HasChildren"))],
           b"Missing/%": [mailbox(b"Missing/Leaf", b"/")]}
run("NonExistent parent retains children", folders=missing,
    commands=b'folders\ncd Missing\nls --messages\nls\ncd Leaf\npwd\nquit\n',
    expected_out='Missing [container]\nLeaf\n/Missing/Leaf\n',
    expected_err=("Choose a selectable folder",), patterns=[b"%", b"Missing/%"])
header_folders = {b"%": [mailbox(b"Inbox")]}
normal_header = (b"Date: Fri, 27 Nov 2026 15:20:00 +0100\r\n"
                 b"From: =?utf-8?b?SsO2cmc=?= <fixture@example.org>\r\n"
                 b"Subject: =?utf-8?q?Folded_caf=C3=A9?=\r\n continuation\r\n\r\n")
messages = [(index * 100003, normal_header) for index in range(1, 66)]
messages[-1] = (messages[-1][0], b"Date: broken\r\nFrom: bad\x1b[2J\x00name\r\nSubject: controls\x1b]0;title\x07\r\n\r\n")
messages[-2] = (messages[-2][0], b"From: missing date\r\n\r\n")
messages[-3] = (messages[-3][0], b"Date: Fri, 27 Nov 2026 15:20:00 +0100\r\nSubject: " + b"x" * 12000 + b"\r\n\r\n")
for installed in (False, True):
    run("bounded header page " + ("installed" if installed else "source"), installed=installed,
        folders=header_folders, messages=messages, commands=b"cd Inbox\nls --messages\nquit\n",
        expected_uids=[uid for uid, _ in reversed(messages[-50:])],
        expected_contains=("Jörg <fixture@example.org>", "Folded café continuation", "[missing]",
                           "2026-11-27 15:20:00+01:00", "02-Jan-2020 03:04:05 +0000 [INTERNALDATE]",
                           "[headers truncated]", "GiB", "[malformed]", "controls�]0;title�"))
run("headers alongside folders", folders=header_folders, messages=messages[:1],
    commands=b"cd Inbox\nls\nquit\n", expected_uids=[messages[0][0]])
for mode in ("cancel", "oversized", "inventory-oversized"):
    run("header recovery " + mode, folders=header_folders, messages=messages[:1], header_mode=mode,
        commands=b"cd Inbox\nls --messages\npwd\nls --messages\nquit\n",
        expected_err=("connection renewed",), expected_uids=[messages[0][0]])
adjacent = [(100003, b"Subject: first UID 999 RFC822.SIZE 1\r\n\r\n"),
            (200006, b"Subject: second\r\n\r\n")]
run("suffix FETCH metadata with adjacent rows and unsolicited FLAGS", folders=header_folders,
    messages=adjacent, header_mode="suffix", commands=b"cd Inbox\nls --messages\nquit\n",
    expected_uids=[200006, 100003], expected_subjects={100003: "first UID 999 RFC822.SIZE 1", 200006: "second"})
run("NIL and empty header fields", folders=header_folders, messages=adjacent, header_mode="empty",
    commands=b"cd Inbox\nls --messages\nquit\n", expected_uids=[200006, 100003],
    expected_subjects={100003: "[missing]", 200006: "[missing]"}, expected_contains=("[INTERNALDATE]",))
for mode in ("rejected", "malformed"):
    run("synchronized header failure " + mode, folders=header_folders, messages=messages[:1], header_mode=mode,
        commands=b"cd Inbox\nls --messages\npwd\nls --messages\nquit\n",
        expected_uids=[messages[0][0]], expected_err=("Folder operation failed",))
run("reconnect mailbox encoding drift", folders={b"%": [mailbox(b"Caf&AOk-")]}, messages=messages[:1],
    header_mode="encoding", expected_out="/\n", expected_uids=[],
    expected_err=("Mailbox encoding changed", "Choose a selectable folder"))

sparse = [(uid, normal_header) for uid in (1, 70, 900000, 2147483648, 4294967200, 4294967280, 4294967290)]
run("sparse UID pages near 32-bit boundary", installed=True, folders=header_folders, messages=sparse,
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nmore\nmore\nquit\n",
    expected_uids=[uid for uid, _ in reversed(sparse)], expected_contains=("No more messages",), max_inventory=18)
run("limit one", folders=header_folders, messages=sparse,
    commands=b"cd Inbox\nls --limit 1 --messages\nmore\nquit\n", expected_uids=[4294967290, 4294967280], max_inventory=5)
many = [(index * 100003, normal_header) for index in range(1, 502)]
run("limit 500 bounded window", installed=True, folders=header_folders, messages=many,
    commands=b"cd Inbox\nls --limit 500 --messages\nquit\n",
    expected_uids=[uid for uid, _ in reversed(many[-500:])], max_inventory=50)
run("empty folder paging", folders=header_folders, messages=[],
    commands=b"cd Inbox\nmore\nls --messages\nmore\nquit\n", expected_uids=[],
    expected_err=("Run ls",), expected_contains=("No more messages",), max_inventory=0)
run("limits and command syntax", folders=header_folders, messages=sparse,
    commands=b"cd Inbox\nls --limit 0\nls --limit 501\nls --limit -1\nls --limit +1\nls --limit x\nls --limit 1.5\nls --limit\nls --limit 2 --limit 3\nls --messages --folders\nmore junk\nmore\nquit\n",
    expected_uids=[], expected_err=("--limit must be an integer from 1 to 500", "Invalid ls options", "Run ls"),
    max_inventory=0)
run("container and root do not fetch pages", folders={b"%": [mailbox(b"Parent", b"/", (b"\\Noselect",))], b"Parent/%": []},
    commands=b"ls --limit 2\nmore\ncd Parent\nls --messages --limit 2\nmore\nquit\n",
    expected_uids=[], expected_err=("Choose a selectable folder",), max_inventory=0)

def expunge(server, send, position):
    uid, _ = server.messages.pop(position - 1)
    server.flags.pop(uid)
    send(f"* {position} EXPUNGE\r\n".encode())


def arrival(server, send, uid):
    server.messages.append((uid, normal_header))
    server.messages.sort()
    server.flags[uid] = {"\\Flagged"}
    send(f"* {len(server.messages)} EXISTS\r\n".encode())


def between_pages(server, send):
    expunge(server, send, 2)  # An unseen old message vanishes.
    expunge(server, send, len(server.messages))  # Previously displayed UID vanishes.
    arrival(server, send, 4294967293)


run("arrival and expunges between pages", folders=header_folders, messages=list(sparse),
    events={("EXAMINE", 2): between_pages},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nmore\nquit\n",
    expected_uids=[4294967290, 4294967280, 4294967200, 2147483648, 900000, 1], max_inventory=20)

def probe_change(server, send):
    expunge(server, send, 1)
    arrival(server, send, 4294967293)


run("expunge and arrival during continuation probe", folders=header_folders, messages=list(sparse),
    events={("FETCH", 2): probe_change},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nmore\nquit\n",
    expected_uids=[4294967290, 4294967280, 4294967200, 2147483648, 900000, 70], max_inventory=20)
run("arrival during continuation probe", folders=header_folders, messages=list(sparse),
    events={("FETCH", 2): lambda s, send: arrival(s, send, 4294967293)},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nmore\nquit\n",
    expected_uids=[uid for uid, _ in reversed(sparse)], max_inventory=20)
run("first inventory expunge retry freezes upper UID", folders=header_folders, messages=list(sparse),
    events={("FETCH", 1): probe_change},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nquit\n",
    expected_uids=[4294967290, 4294967280, 4294967200, 2147483648, 900000, 70], max_inventory=20)
run("expunge after captured inventory", folders=header_folders, messages=list(sparse),
    events={("UID", 1): lambda s, send: expunge(s, send, len(s.messages))},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nmore\nquit\n",
    expected_uids=[4294967280, 4294967200, 2147483648, 900000, 70, 1], max_inventory=20)

def replace_mailbox(server, send):
    server.validity += 1


run("UIDVALIDITY replacement discards cursor", folders=header_folders, messages=list(sparse),
    events={("EXAMINE", 2): replace_mailbox},
    commands=b"cd Inbox\nls --messages --limit 2\nmore\nmore\nquit\n",
    expected_uids=[4294967290, 4294967280], expected_err=("UIDVALIDITY changed", "run ls to refresh", "Run ls"), max_inventory=1)
run("refresh resets paging", folders=header_folders, messages=list(sparse),
    commands=b"cd Inbox\nls --messages --limit 2\nrefresh\nmore\nquit\n",
    expected_uids=[4294967290, 4294967280], expected_err=("Run ls",), max_inventory=1)
run("repeated probe churn is bounded and leaves shell usable", folders=header_folders, messages=list(sparse),
    events={("FETCH", count): lambda s, send: expunge(s, send, 1) for count in (1, 2, 3)},
    commands=b"cd Inbox\nls --messages --limit 2\npwd\nmore\nquit\n", expected_uids=[],
    expected_contains=("/Inbox",), expected_err=("Mailbox changed repeatedly", "Run ls"), max_inventory=3)
size_messages = [(uid, normal_header) for uid in (1, 70, 4294967290)]
size_values = {1: 1024, 70: 2048, 4294967290: 5 * 1024 ** 3}
size_sum = sum(size_values.values())
for total in (size_sum, size_sum + 98765, 9223372036854775807, 0):
    run("server-reported size " + str(total), installed=True, folders=header_folders,
        messages=size_messages if total else [], sizes={"capability": True, "status": total},
        commands=b"du Inbox\npwd\nquit\n", size_uids=[], status_count=1,
        expected_out=f"/Inbox\t{total} bytes\tserver-reported\n/\n")
for exact in (False, True):
    run("measured wide size " + str(exact), installed=exact, folders=header_folders,
        messages=size_messages, sizes={"capability": exact, "status": size_sum + 99, "values": size_values},
        commands=b"cd Inbox\ndu" + (b" --exact" if exact else b"") + b"\npwd\nquit\n",
        size_uids=[1, 70, 4294967290], status_count=0,
        expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n/Inbox\n")
for reply in (None, "denied"):
    run("missing STATUS falls back " + str(reply), folders=header_folders, messages=size_messages,
        sizes={"capability": True, "status": reply, "values": size_values},
        commands=b"du Inbox\nquit\n", size_uids=[1, 70, 4294967290], status_count=1,
        expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
run("wrong STATUS identity falls back", folders=header_folders, messages=size_messages,
    sizes={"capability": True, "status": 99999, "reply_raw": b"Other", "values": size_values},
    commands=b"du Inbox\nquit\n", size_uids=[1, 70, 4294967290], status_count=1,
    expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
run("empty measured folder", folders=header_folders,
    commands=b"du Inbox --exact\nquit\n", size_uids=[],
    expected_out="/Inbox\t0 bytes\tmeasured for UID inventory; complete; 0/0 messages\n")
run("inaccessible is not empty", folders=header_folders, sizes={"capability": True, "status": "denied", "denied": True},
    commands=b"du Inbox\npwd\nquit\n", size_uids=[], status_count=1, expected_out="/\n",
    expected_err=("Folder inaccessible or size inventory unavailable",))
run("unavailable message size is partial", folders=header_folders, messages=size_messages,
    sizes={"values": {1: 1024, 70: None, 4294967290: 5 * 1024 ** 3}},
    commands=b"du Inbox\nquit\n", size_uids=[1, 70, 4294967290],
    expected_out=f"/Inbox\t{size_sum - 2048} bytes\tmeasured for UID inventory; partial; 2/3 messages\n")
run("denied metadata is partial", folders=header_folders, messages=size_messages,
    sizes={"fetch_denied": True}, commands=b"du Inbox\npwd\nquit\n", size_uids=[1, 70, 4294967290],
    expected_out="/Inbox\t0 bytes\tmeasured for UID inventory; partial; 0/3 messages\n/\n")
run("expunged captured UID is partial", folders=header_folders, messages=list(size_messages),
    sizes={"values": size_values}, events={("UID", 1): lambda s, send: expunge(s, send, 2)},
    commands=b"du Inbox --exact\nquit\n", size_uids=[1, 70, 4294967290],
    expected_out=f"/Inbox\t{size_sum - 2048} bytes\tmeasured for UID inventory; partial; 2/3 messages\n")
run("arrival after capture excluded", folders=header_folders, messages=list(size_messages),
    sizes={"values": size_values}, events={("UID", 1): lambda s, send: arrival(s, send, 4294967293)},
    commands=b"du Inbox --exact\nquit\n", size_uids=[1, 70, 4294967290],
    expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
run("inventory retry keeps upper UID", folders=header_folders, messages=list(size_messages),
    sizes={"values": size_values}, events={("FETCH", 1): probe_change},
    commands=b"du Inbox --exact\nquit\n", size_uids=[70, 4294967290],
    expected_out=f"/Inbox\t{size_sum - 1024} bytes\tmeasured for UID inventory; complete; 2/2 messages\n")
run("arrival during inventory excluded", folders=header_folders, messages=list(size_messages),
    sizes={"values": size_values}, events={("FETCH", 1): lambda s, send: arrival(s, send, 4294967293)},
    commands=b"du Inbox --exact\nquit\n", size_uids=[1, 70, 4294967290],
    expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
for raw, display, literal in ((b'A*%" mailbox', "A%2A%25%22%20mailbox", False),
                              (b"Caf&AOk-", "Café", True)):
    run("exact STATUS mailbox identity " + display, folders={b"%": [mailbox(raw)]},
        sizes={"capability": True, "status": size_sum, "raw": raw, "literal": literal},
        commands=f"du {display}\nquit\n".encode(), size_uids=[], status_count=1,
        expected_out=f"/{display}\t{size_sum} bytes\tserver-reported\n")
    run("exact measured mailbox identity " + display, folders={b"%": [mailbox(raw)]},
        messages=size_messages, sizes={"raw": raw, "values": size_values},
        commands=f"du --exact {display}\nquit\n".encode(), size_uids=[1, 70, 4294967290], status_count=0,
        expected_out=f"/{display}\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
run("bounded complete size inventory", folders=header_folders, messages=list(many),
    commands=b"du Inbox --exact\nquit\n", size_uids=[uid for uid, _ in many],
    expected_out=f"/Inbox\t{sum(uid * 1024 for uid, _ in many)} bytes\tmeasured for UID inventory; complete; 501/501 messages\n")
run("du syntax and containers", folders={b"%": [mailbox(b"Parent", b"/", (b"\\Noselect",))]},
    commands=b"du\ndu Parent\ndu Inbox --recursive\ndu --exact --exact\ndu a b\npwd\nquit\n",
    size_uids=[], expected_out="/\n", expected_err=("Choose a selectable folder", "Invalid du options"))
run("rev2 core STATUS SIZE without extension", installed=True, version="IMAP4rev2",
    folders=header_folders, messages=size_messages, sizes={"status": size_sum},
    commands=b"du Inbox\nquit\n", size_uids=[], status_count=1,
    expected_out=f"/Inbox\t{size_sum} bytes\tserver-reported\n")
run("dual server remains rev1 after UTF8 enable", version="IMAP4rev1 IMAP4rev2", utf8=True,
    folders=header_folders, messages=size_messages, sizes={"status": size_sum + 99, "values": size_values},
    commands=b"du Inbox\nquit\n", size_uids=[1, 70, 4294967290], status_count=0,
    expected_out=f"/Inbox\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
for literal in (False, True):
    run("canonical INBOX STATUS identity " + str(literal), folders=header_folders,
        sizes={"capability": True, "status": size_sum, "reply_raw": b"INBOX", "literal": literal},
        commands=b"du Inbox\nquit\n", size_uids=[], status_count=1,
        expected_out=f"/Inbox\t{size_sum} bytes\tserver-reported\n")
run("ordinary STATUS mailbox names remain case sensitive", folders={b"%": [mailbox(b"Archive")]},
    messages=size_messages, sizes={"capability": True, "raw": b"Archive", "reply_raw": b"ARCHIVE",
                                   "status": size_sum + 99, "values": size_values},
    commands=b"du Archive\nquit\n", size_uids=[1, 70, 4294967290], status_count=1,
    expected_out=f"/Archive\t{size_sum} bytes\tmeasured for UID inventory; complete; 3/3 messages\n")
for exact in (False, True):
    run("du preserves header paging " + str(exact), installed=exact, folders=header_folders,
        messages=size_messages, sizes={"capability": True, "status": size_sum, "values": size_values},
        commands=b"cd Inbox\nls --messages --limit 1\ndu" + (b" --exact" if exact else b"") + b"\nmore\nquit\n",
        expected_uids=[4294967290, 70], status_count=0 if exact else 1)
tree_folders = {
    b"%": [mailbox(b"A", b"/"), mailbox(b"B", b"/"), mailbox(b"C", b"/", (b"\\Noselect",)),
            mailbox(b"Empty"), mailbox(b"Inbox"), mailbox(b"INBOX"), mailbox(b"A/Spy", b"/")],
    b"A/%": [mailbox(b"A/Big", b"/", literal=True)], b"A/Big/%": [],
    b"B/%": [], b"C/%": [mailbox(b"C/Leaf", b"/")], b"C/Leaf/%": [],
}
tree_totals = {b"A": 10, b"A/Big": 1000, b"B": 900, b"C/Leaf": 40, b"Empty": 0,
               b"Inbox": 5, b"INBOX": 5, b"A/Spy": 999999}
tree_boxes = {raw: {"status": total, "messages": [(1, normal_header)] if total else [], "values": {1: total}}
              for raw, total in tree_totals.items()}
tree_expected = [("/", "-", 1955), ("/A", "10", 1010), ("/A/Big", "1000", 1000),
                 ("/B", "900", 900), ("/C", "-", 40), ("/C/Leaf", "40", 40),
                 ("/Empty", "0", 0), ("/INBOX", "5", 5)]
for version, combined in (("IMAP4rev1", False), ("IMAP4rev1", True), ("IMAP4rev2", True)):
    run("recursive independent totals " + version + str(combined), installed=combined,
        version=version, folders=tree_folders,
        sizes={"capability": version == "IMAP4rev1", "list_status": combined and version == "IMAP4rev1", "mailboxes": tree_boxes},
        commands=b"du --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
        size_uids=[], status_count=0 if combined else 6, list_status_count=6 if combined else 0,
        du_labels=("6/6 known folders complete",))
run("recursive metadata totals", installed=True, folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes},
    commands=b"du --exact --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
    size_uids=[1, 1, 1, 1, 1], status_count=0, list_status_count=0,
    du_labels=("5/5 measured messages (6 metadata folders)",))
run("rank by direct bytes not subtree", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes},
    commands=b"du --top 4\nquit\n", du_rows=[tree_expected[i] for i in (2, 3, 5, 1)], du_total=1955,
    size_uids=[], status_count=0, list_status_count=6)
run("depth one limits discovery", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes},
    commands=b"du --depth 1\nquit\n", du_rows=[("/", "-", 915), ("/A", "10", 10), ("/B", "900", 900),
                                                ("/C", "-", 0), ("/Empty", "0", 0), ("/INBOX", "5", 5)],
    du_total=915, patterns=[b"%"], size_uids=[], status_count=0, list_status_count=1,
    du_labels=("depth 1", "4/4 known folders complete"))
run("depth zero root has zero extent", folders={}, commands=b"du --depth 0\nquit\n",
    du_rows=[("/", "-", 0)], du_total=0, patterns=[], size_uids=[], status_count=0,
    du_labels=("0/0 known folders complete", "depth 0"))
run("depth zero selectable folder", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes},
    commands=b"du A --depth 0\nquit\n", du_rows=[("/A", "10", 10)], du_total=10,
    patterns=[b"%"], size_uids=[], status_count=1, list_status_count=0)
run("LIST STATUS omission recovered separately", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes, "omit": (b"A",)},
    commands=b"du --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
    size_uids=[], status_count=1, list_status_count=6)
mixed_boxes = dict(tree_boxes)
mixed_boxes[b"A"] = {"status": None, "messages": [(1, normal_header), (2, normal_header)], "values": {1: 10, 2: None}}
run("mixed sources and partial message coverage", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": mixed_boxes},
    commands=b"du --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
    size_uids=[1, 2], status_count=1, list_status_count=6,
    du_labels=("measured+server-reported", "5/6 known folders complete", "1/2 measured messages (1 metadata folders)", "partial; 1/2 messages"))
denied_boxes = dict(tree_boxes)
denied_boxes[b"A"] = {"status": "denied", "denied": True}
denied_expected = list(tree_expected)
denied_expected[0], denied_expected[1] = ("/", "-", 1945), ("/A", "unknown", 1000)
run("inaccessible folder retains accessible descendants", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": denied_boxes},
    commands=b"du --recursive\nquit\n", du_rows=denied_expected, du_total=1945,
    size_uids=[], status_count=1, list_status_count=6, du_labels=("server-reported+unknown", "5/6 known folders complete"))
subtree_expected = [row for row in tree_expected if row[0] != "/C/Leaf"]
subtree_expected[0], subtree_expected[4] = ("/", "-", 1915), ("/C", "-", 0)
run("denied subtree has unknown extent", folders=tree_folders,
    sizes={"capability": True, "list_status": True, "mailboxes": tree_boxes, "list_denied": (b"C/%",)},
    commands=b"du --recursive\nquit\n", du_rows=subtree_expected, du_total=1915,
    size_uids=[], status_count=0, list_status_count=5, du_labels=("partial; 5/5 known folders complete; descendants unknown",),
    expected_err=("descendants unavailable for /C",))
run("dual rev1 tree requires LIST STATUS advertisement", version="IMAP4rev1 IMAP4rev2", utf8=True,
    folders=tree_folders, sizes={"capability": True, "mailboxes": tree_boxes},
    commands=b"du --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
    size_uids=[], status_count=6, list_status_count=0)
cancel_names = [b"A"] + [f"B{number:03d}".encode() for number in range(100)]
run("actual Ctrl-C retains partial ranking and renews transport", installed=True,
    folders={b"%": [mailbox(raw) for raw in cancel_names]},
    sizes={"capability": True, "cancel": True, "mailboxes": {raw: {"status": 10} for raw in cancel_names}},
    commands=b"du --top 2\n", du_cancel=True, du_rows=[("/A", "10", 10), ("/B000", "unknown", 0)],
    du_total=10, size_uids=[], status_count=3, du_labels=("partial; 1/101 known folders complete",))
run("recursive syntax validation", folders={}, commands=b"du --depth -1\ndu --depth\ndu --top 0\ndu --top nope\ndu --recursive --recursive\ndu --depth 1 --depth 2\npwd\nquit\n",
    expected_out="/\n", expected_err=("Invalid du options",), size_uids=[], status_count=0, patterns=[])
for rejection in ("NO", "BAD"):
    run("advertised extended LIST rejected " + rejection, installed=rejection == "BAD", folders=tree_folders,
        sizes={"capability": True, "list_status": True, "reject_extended": rejection, "mailboxes": tree_boxes},
        commands=b"du --recursive\nquit\n", du_rows=tree_expected, du_total=1955,
        size_uids=[], status_count=6, list_status_count=6)
wide_names = [f"Box{number:04d}".encode() for number in range(512)] + [b"box0000", b"Inbox"]
wide_boxes = {raw: {"status": index + 1} for index, raw in enumerate(wide_names)}
wide_boxes[b"Inbox"] = {"status": 999999, "reply_raw": b"INBOX", "literal": True}
run("wide STATUS index preserves ordinary case and INBOX alias", installed=True,
    folders={b"%": [mailbox(raw) for raw in wide_names]},
    sizes={"capability": True, "list_status": True, "mailboxes": wide_boxes},
    commands=b"du --depth 1 --top 3\nquit\n", du_rows=[("/Inbox", "999999", 999999), ("/box0000", "513", 513), ("/Box0511", "512", 512)],
    du_total=sum(range(1, 513)) + 513 + 999999, size_uids=[], status_count=0, list_status_count=1,
    du_labels=("514/514 known folders complete",))
for failure, exit_code in (("timeout", 124), ("abort", 1)):
    run("abnormal size scan preserves partial and closes " + failure,
        folders={b"%": [mailbox(b"A"), mailbox(b"B")]},
        sizes={"capability": True, "scan_failure": failure, "mailboxes": {b"A": {"status": 10}, b"B": {"status": 20}}},
        commands=b"du --recursive\nquit\n", expected=exit_code,
        du_rows=[("/", "-", 10), ("/A", "10", 10), ("/B", "unknown", 0)], du_total=10,
        size_uids=[], status_count=2, list_status_count=0,
        du_labels=("partial; 1/2 known folders complete",))
run("aborted extended LIST never retries ordinary LIST", folders={b"%": []},
    sizes={"capability": True, "list_status": True, "extended_abort": True},
    commands=b"du --recursive\nquit\n", expected=1, du_rows=[("/", "-", 0)],
    du_total=0, size_uids=[], status_count=0, list_status_count=1,
    du_labels=("partial; 0/0 known folders complete; descendants unknown",))
write_initial = {b"Inbox": (1,), b"Work projects": (2, 3), b"Work projects.Child": (4,),
                 b"Work projects.Child.Deep": (5,), b"Archive": (), b"Archive.Exists": (6,)}
write_final = {b"Inbox": (1,), b"Archive": (), b"Archive.Exists": (6,), b"New folder": (),
               b"Archive.New child": (), b"Archive.Moved tree": (2, 3),
               b"Archive.Moved tree.Child": (4,), b"Archive.Moved tree.Child.Deep": (5,)}
run("CREATE and single RENAME preserve dotted subtree", write_state=write_initial,
    commands=b'mkdir "New folder"\nmkdir Archive/New%20child\nmv "Work projects" Archive/Moved%20tree\nquit\n',
    expected_writes=[("CREATE", b"New folder"), ("CREATE", b"Archive.New child"),
                     ("RENAME", b"Work projects", b"Archive.Moved tree")], final_state=write_final)
run("rename alias uses complete exact target", installed=True, write_state=write_initial,
    commands=b'rename "Work projects" Archive/Other\nquit\n',
    expected_writes=[("RENAME", b"Work projects", b"Archive.Other")],
    final_state={**{raw: value for raw, value in write_initial.items() if not raw.startswith(b"Work projects")},
                 b"Archive.Other": (2, 3), b"Archive.Other.Child": (4,), b"Archive.Other.Child.Deep": (5,)})
run("invalid writes never mutate", write_state=write_initial,
    commands=b'mkdir /\nmkdir Archive/Exists\nmkdir Missing/Child\nmkdir Archive/a%2Eb\nmv / New\nmv Inbox New\nmv "Work projects" Inbox\nmv "Work projects" Archive/Exists\nmv "Work projects" Missing/New\nmv "Work projects" "Work projects/Child/New"\nmkdir Archive/%00\npwd\nquit\n',
    expected_writes=[], final_state=write_initial, expected_out="/\n", expected_err=("Folder operation failed",))
for rejected in ("NO", "BAD"):
    run("synchronized " + rejected + " writes leave session usable", write_state=write_initial,
        write_replies={"CREATE": rejected, "RENAME": rejected},
        commands=b'mkdir Archive/New\nmv "Work projects" Archive/New\npwd\nfolders\nquit\n',
        expected_writes=[("CREATE", b"Archive.New"), ("RENAME", b"Work projects", b"Archive.New")],
        final_state=write_initial, expected_err=("Folder operation failed",),
        expected_out="/\nArchive\nInbox\nWork%20projects\n")
for version, utf8, encoded in (("IMAP4rev1", False, b"Caf&AOk-"),
                               ("IMAP4rev2", False, "Café".encode()),
                               ("IMAP4rev1", True, "Café".encode())):
    run("write negotiated mailbox encoding " + version + str(utf8), version=version, utf8=utf8,
        write_state={b"Archive": ()}, commands='mkdir Café\nrename Café Archive/Étage\nquit\n'.encode(),
        expected_writes=[("CREATE", encoded), ("RENAME", encoded,
                         b"Archive." + (b"&AMk-tage" if not utf8 and version == "IMAP4rev1" else "Étage".encode()))],
        final_state={b"Archive": (), b"Archive." + (b"&AMk-tage" if not utf8 and version == "IMAP4rev1" else "Étage".encode()): ()})
run("both RENAME operands use binary safe literals", version="IMAP4rev2",
    write_state={b"old\nname": (9,), b"old\nname.Child": (10,), b"Archive": ()},
    commands=b'rename old%0Aname Archive/new%0Dname\nmkdir Archive/line%0Abreak\nquit\n',
    expected_writes=[("RENAME", b"old\nname", b"Archive.new\rname"), ("CREATE", b"Archive.line\nbreak")],
    final_state={b"Archive": (), b"Archive.new\rname": (9,), b"Archive.new\rname.Child": (10,), b"Archive.line\nbreak": ()})
run("ambiguous root namespace refuses CREATE", folders=mixed,
    commands=b'mkdir New\nquit\n', expected_writes=[], expected_err=("Folder operation failed",))
for namespace in (b".", None):
    run("first folder discovers empty root " + repr(namespace), write_state={}, write_namespace=namespace,
        commands=b'mkdir "First folder"\nquit\n', expected_writes=[("CREATE", b"First folder")],
        final_state={b"First folder": ()})


def remove_cached_parent(server, send):
    assert b"Parent.Child" in server.write_state
    del server.write_state[b"Parent"]
    del server.write_state[b"Parent.Child"]


run("relative writes revalidate deleted cwd and ancestors", write_state={b"Parent": (), b"Parent.Child": (), b"Survivor": ()},
    events={("LIST", 3): remove_cached_parent},
    commands=b'cd Parent/Child\nmkdir new\nmkdir ./second\nmkdir ../sibling\nmkdir ../../Survivor/new\nquit\n',
    expected_writes=[("CREATE", b"Survivor.new")], final_state={b"Survivor": (), b"Survivor.new": ()},
    expected_err=("Folder operation failed",))
run("cancel before mailbox literal continuation closes transport", version="IMAP4rev2", write_state={b"Archive": ()},
    commands=b'mkdir Archive/new%0Aname\n', write_interrupt=True,
    expected_writes=[], final_state={b"Archive": ()}, expected_err=("outcome unknown", "Observed new path: not listed"))
navigation_state = {b"Old": (1,), b"Old.Child": (2,), b"Old.Child.Deep": (3,), b"Archive": ()}
navigation_final = {b"Archive": (), b"Archive.New": (1,), b"Archive.New.Child": (2,), b"Archive.New.Child.Deep": (3,)}
run("confirmed rename remaps cwd descendants and refreshes parents", write_state=navigation_state,
    commands=b'cd Old/Child\nfolders\nrename /Old /Archive/New\npwd\nfolders\ncd /\nfolders\nquit\n',
    expected_writes=[("RENAME", b"Old", b"Archive.New")], final_state=navigation_final,
    expected_out="Deep\n/Archive/New/Child\nDeep\nArchive\n", expected_err=("RENAME confirmed", "Observed subscriptions: +0 -0"),
    subscriptions=(b"Old", b"Old.Child"), final_subscriptions=(b"Old", b"Old.Child"))
run("observed subscription remap is reported without subscription writes", installed=True, write_state=navigation_state,
    commands=b'rename Old Archive/New\nquit\n', expected_writes=[("RENAME", b"Old", b"Archive.New")],
    subscriptions=(b"Old", b"Old.Child"), subscriptions_after=(b"Archive.New", b"Archive.New.Child"),
    final_subscriptions=(b"Archive.New", b"Archive.New.Child"), final_state=navigation_final,
    expected_err=("Observed subscriptions: +2 -2",))


def remove_current_on_create(server, send):
    del server.write_state[b"Archive.Current"]


run("confirmed create falls back from externally deleted cwd", write_state={b"Archive": (), b"Archive.Current": ()},
    events={("CREATE", 1): remove_current_on_create}, commands=b'cd Archive/Current\nmkdir /Archive/New\npwd\nfolders\nquit\n',
    expected_writes=[("CREATE", b"Archive.New")], final_state={b"Archive": (), b"Archive.New": ()},
    expected_out="/Archive\nNew\n", expected_err=("CREATE confirmed", "Current folder unavailable; returned to /Archive"))


def collide_before_rename(server, send):
    server.write_state[b"Archive.New"] = (99,)


run("external target collision stays rejected and refreshes navigation", write_state=navigation_state,
    events={("RENAME", 1): collide_before_rename}, write_replies={"RENAME": "NO"},
    commands=b'cd Old\nrename /Old /Archive/New\npwd\ncd /Archive\nfolders\nquit\n',
    expected_writes=[("RENAME", b"Old", b"Archive.New")], final_state={**navigation_state, b"Archive.New": (99,)},
    expected_out="/Old\nNew\n", expected_err=("RENAME rejected",))
for loss in ("before", "after"):
    run("missing RENAME completion " + loss + " remote commit", write_state=navigation_state, write_disconnect=loss,
        commands=b'cd Old/Child\nrename /Old /Archive/New\npwd\nfolders\nquit\n',
        expected_writes=[("RENAME", b"Old", b"Archive.New")],
        final_state=navigation_state if loss == "before" else navigation_final,
        expected_out="/Old/Child\nDeep\n" if loss == "before" else "/\nArchive\n",
        expected_err=("RENAME outcome unknown", "Observed old path: " + ("present" if loss == "before" else "not listed"),
                      "Observed new path: " + ("not listed" if loss == "before" else "present")))
run("missing CREATE completion remains unknown after observed commit", write_state={b"Archive": ()}, write_disconnect="after",
    commands=b'mkdir Archive/New\ncd Archive\nfolders\nquit\n', expected_writes=[("CREATE", b"Archive.New")],
    final_state={b"Archive": (), b"Archive.New": ()}, expected_out="New\n",
    expected_err=("CREATE outcome unknown", "Observed new path: present; outcome remains unknown"))
run("unknown write does not probe cached names after encoding change", write_state={b"Archive": ()}, write_disconnect="encoding",
    commands='mkdir Café\npwd\nquit\n'.encode(), expected_writes=[("CREATE", b"Caf&AOk-")],
    final_state={b"Archive": (), b"Caf&AOk-": ()}, expected_out="/\n", expected_err=("outcome unknown", "encoding changed"))
run("unknown probes filter wildcard name aliases exactly", write_state={b"A*%": (), b"AX": (), b"Archive": ()},
    sizes={"probe_rows": {b"Archive.New": [mailbox(b"Archive.New", b".", (b"\\NonExistent",))]}},
    write_disconnect="before", commands=b'rename A%2A%25 Archive/New\nquit\n',
    expected_writes=[("RENAME", b"A*%", b"Archive.New")], final_state={b"A*%": (), b"AX": (), b"Archive": ()},
    expected_err=("Observed old path: present", "Observed new path: not listed"))
run("rev2 subscribed LIST tracks trailing-delimiter namespace descendants", version="IMAP4rev2",
    write_state={b"Namespace.": (), b"Namespace.Child": (1,), b"Namespace.Unsubscribed": (2,), b"Archive": ()},
    sizes={"inject_unsubscribed": True}, subscriptions=(b"Namespace.", b"Namespace.Child"),
    subscriptions_after=(b"Archive.Moved", b"Archive.Moved.Child"),
    final_subscriptions=(b"Archive.Moved", b"Archive.Moved.Child"),
    commands=b'cd Namespace/Child\nrename /Namespace /Archive/Moved\npwd\ncd ..\nfolders\nquit\n',
    expected_writes=[("RENAME", b"Namespace.", b"Archive.Moved")],
    final_state={b"Archive": (), b"Archive.Moved": (), b"Archive.Moved.Child": (1,), b"Archive.Moved.Unsubscribed": (2,)},
    expected_out="/Archive/Moved/Child\nChild\nUnsubscribed\n", expected_err=("Observed subscriptions: +2 -2",))
preview_initial = {b"Lists": (), b"List-Z": (1,), b"List-A": (2,), b"List-A.Child": (3,), b"list-lower": (4,)}
for installed in (False, True):
    run("sorted read-only rename preview " + str(installed), installed=installed, write_state=preview_initial,
        commands=b"rename-glob 'List-*' 'Lists/{1}'\npwd\nquit\n", final_state=preview_initial,
        patterns=[b"%", b"Lists.%", b"*"],
        preview_rows=[("/List-A", "/Lists/A", 2), ("/List-Z", "/Lists/Z", 1)])
run("rename preview zero matches", write_state=preview_initial,
    commands=b"rename-glob 'LIST-*' 'Lists/{1}'\nquit\n", final_state=preview_initial, preview_rows=[])
run("rename preview empty capture is allowed in a component", write_state={b"Old-": ()},
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", final_state={b"Old-": ()},
    preview_rows=[("/Old-", "/New-", 1)])
run("rename preview empty destination capture blocks", write_state={b"Old-": (), b"Archive": ()},
    commands=b"rename-glob 'Old-*' 'Archive/{1}'\nquit\n", final_state={b"Old-": (), b"Archive": ()},
    preview_rows=[("/Old-", "[invalid destination]", 1)], preview_blocked=True,
    preview_errors=("empty destination component",))
for version in ("IMAP4rev1", "IMAP4rev2"):
    unicode_state = {folderlib.mailbox_bytes(text, version == "IMAP4rev2"): ()
                     for text in ("É-Café", "é-excluded", "Archive")}
    run("rename preview Unicode case and raw encoding " + version, installed=True, version=version,
        write_state=unicode_state, commands="rename-glob 'É-*' 'Archive/{1}'\nquit\n".encode(),
        final_state=unicode_state, preview_rows=[("/É-Café", "/Archive/Café", 1)])
for source in (r"'Old-\**'", r'"Old-\**"', r"Old-\**"):
    state = {b"Old-*One": (), b"Old-excluded": ()}
    run("rename preview tokenizer preserves escaped star " + source, write_state=state,
        commands=("rename-glob " + source + " 'New-{1}'\nquit\n").encode(), final_state=state,
        preview_rows=[("/Old-%2AOne", "/New-One", 1)])
run("rename preview percent escaped star stays literal", write_state={b"Old-*One": ()},
    commands=b"rename-glob 'Old-%2A*' 'New-{1}'\nquit\n", final_state={b"Old-*One": ()},
    preview_rows=[("/Old-%2AOne", "/New-One", 1)])
literal_capture = "a/b%\\{1}?[]'\""
literal_state = {("Old-" + literal_capture).encode(): ()}
literal_display = folderlib.display_component(literal_capture)
run("rename preview captured metacharacters remain literal", installed=True, write_namespace=None,
    write_state=literal_state, commands=b"rename-glob 'Old-*' 'New-{1}-{1}'\nquit\n",
    final_state=literal_state,
    preview_rows=[("/Old-" + literal_display, "/New-" + literal_display + "-" + literal_display, 1)])
for capture in (".", ".."):
    state = {("Old-" + capture).encode(): ()}
    run("rename preview captured dot is a literal component " + capture, write_namespace=None, write_state=state,
        commands=b"rename-glob 'Old-*' '{1}'\nquit\n", final_state=state,
        preview_rows=[("/Old-" + capture, "/" + folderlib.display_component(capture), 1)])
run("rename preview escaped template and percent are literal", write_namespace=None, write_state={b"Old-%x": ()},
    commands=b"rename-glob 'Old-\\%*' 'New-\\{1\\}-\\%{1}'\nquit\n", final_state={b"Old-%x": ()},
    preview_rows=[("/Old-%25x", "/New-{1}-%25x", 1)])
parent_state = {b"Parent*": (), b"Parent*.Old-a": (), b"ParentX": (), b"ParentX.Old-excluded": (), b"Archive": ()}
run("rename preview literal wildcard parent filters wire aliases", write_state=parent_state,
    commands=b"rename-glob 'Parent\\*/Old-*' 'Archive/{1}'\nquit\n", final_state=parent_state,
    preview_rows=[("/Parent%2A/Old-a", "/Archive/a", 1)])
run("rename preview escaped command whitespace stays literal", write_state={b"Old folder-a": ()},
    commands=b"rename-glob Old\\ folder-* New\\ folder-{1}\nquit\n", final_state={b"Old folder-a": ()},
    preview_rows=[("/Old%20folder-a", "/New%20folder-a", 1)])
relative_preview = {b"Parent": (), b"Parent.Old-a": (), b"Parent.Archive": ()}
run("rename preview relative paths preserve navigation", installed=True, write_state=relative_preview,
    commands=b"cd Parent\nrename-glob 'Old-*' 'Archive/{1}'\npwd\nquit\n", final_state=relative_preview,
    preview_rows=[("/Parent/Old-a", "/Parent/Archive/a", 1)])


def add_preview_match(server, send):
    server.write_state[b"Old-b"] = (2,)


run("rename preview refreshes cached membership", write_state={b"Old-a": (1,)},
    events={("LIST", 2): add_preview_match}, commands=b"folders\nrename-glob 'Old-*' 'New-{1}'\nquit\n",
    final_state={b"Old-a": (1,), b"Old-b": (2,)},
    preview_rows=[("/Old-a", "/New-a", 1), ("/Old-b", "/New-b", 1)])
run("rename preview refuses deleted cached cwd", write_state={b"Parent": (), b"Parent.Child": ()},
    events={("LIST", 3): remove_cached_parent},
    commands=b"cd Parent/Child\nrename-glob '*' 'New-{1}'\npwd\nquit\n", final_state={},
    preview_rows=[], preview_blocked=True, preview_errors=("folder does not exist",))
for source, template, absolute in (("/Old-*", "/New-{1}", True),
                                   ("Old-*", "/New-{1}", False),
                                   ("/Old-*", "New-{1}", False)):
    run("rename preview deleted cwd absolute fence " + source + template, installed=absolute,
        write_state={b"Parent": (), b"Parent.Child": (), b"Old-a": ()},
        events={("LIST", 3): remove_cached_parent},
        commands=f"cd Parent/Child\nrename-glob '{source}' '{template}'\npwd\nquit\n".encode(),
        final_state={b"Old-a": ()}, preview_rows=[("/Old-a", "/New-a", 1)] if absolute else [],
        preview_blocked=not absolute, preview_errors=() if absolute else ("folder does not exist",))
for source, template, cause in (("Old", "New-{1}", "exactly one unescaped *"),
                                ("Old-**", "New-{1}", "exactly one unescaped *"),
                                ("P*/Old-*", "New-{1}", "parent must be literal"),
                                ("Old-*", "Archive//{1}", "empty path component"),
                                ("Old-%xx*", "New-{1}", "invalid display escape")):
    run("rename preview rejects grammar " + source + template, write_state={b"Old-a": ()},
        commands=f"rename-glob '{source}' '{template}'\nquit\n".encode(), final_state={b"Old-a": ()},
        preview_rows=[] if source != "Old-*" else [("/Old-a", "[invalid destination]", 1)],
        preview_blocked=True, preview_errors=(cause,))
for state, template, rows, cause in (
        ({b"Old-a": (), b"New-a": ()}, "New-{1}", [("/Old-a", "/New-a", 1)], "destination exists"),
        ({b"Old-a": (), b"Old-a.Child": (), b"New-a.Child": ()}, "New-{1}",
         [("/Old-a", "/New-a", 2)], "destination subtree collides"),
        ({b"Old-a": (), b"Old-b": ()}, "New", [("/Old-a", "/New", 1), ("/Old-b", "/New", 1)], "duplicate destinations"),
        ({b"Old-a": (), b"Old-aa": ()}, "Old-a{1}", [("/Old-a", "/Old-aa", 1), ("/Old-aa", "/Old-aaa", 1)], "dependency chain or cycle"),
        ({b"Old-a": ()}, "Old-{1}", [("/Old-a", "/Old-a", 1)], "inside source or unchanged"),
        ({b"Old-a": ()}, "Old-a/New-{1}", [("/Old-a", "/Old-a/New-a", 1)], "inside source or unchanged"),
        ({b"Old-a": ()}, "Missing/{1}", [("/Old-a", "/Missing/a", 1)], "folder does not exist"),
        ({b"Old-a": ()}, "INBOX", [("/Old-a", "/INBOX", 1)], "special INBOX destination")):
    run("rename preview blocks " + cause, write_state=state, installed=cause == "dependency chain or cycle",
        commands=f"rename-glob 'Old-*' '{template}'\nquit\n".encode(), final_state=state,
        preview_rows=rows, preview_blocked=True, preview_errors=(cause,))
run("rename preview protects INBOX source", write_state={b"Inbox": ()},
    commands=b"rename-glob 'In*' 'New-{1}'\nquit\n", final_state={b"Inbox": ()},
    preview_rows=[("/Inbox", "/New-box", 1)], preview_blocked=True, preview_errors=("cannot rename root or INBOX",))
trailing_preview = {b"Old-a.": (), b"Old-a.Child": (), b"New-a.Child": ()}
run("rename preview indexes trailing-delimiter subtree collision", write_state=trailing_preview,
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", final_state=trailing_preview,
    preview_rows=[("/Old-a", "/New-a", 2)], preview_blocked=True,
    preview_errors=("destination subtree collides",))
ambiguous = [mailbox(b"Old-a", b"."), mailbox(b"Old-a.", b".")]
run("rename preview blocks ambiguous path identity", folders={b"%": ambiguous},
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", preview_rows=[], preview_blocked=True,
    preview_errors=("ambiguous folder identity",))
overlapping = [mailbox(b"Old-A", b"."), mailbox(b"Old-A.Child", None), mailbox(b"Target", b".")]
run("rename preview blocks overlapping raw subtrees", folders={b"%": overlapping, b"Target.%": [], b"*": overlapping},
    commands=b"rename-glob 'Old-*' 'Target/{1}'\nquit\n",
    preview_rows=[("/Old-A", "/Target/A", 1), ("/Old-A.Child", "/Target/A.Child", 1)],
    preview_blocked=True, preview_errors=("overlapping source subtrees",))
cross_namespace = [mailbox(b"Old-a", b"/"), mailbox(b"Target", b".")]
run("rename preview blocks cross-delimiter subtree projection",
    folders={b"%": cross_namespace, b"Target.%": [],
             b"*": cross_namespace + [mailbox(b"Old-a/Child", b"/")]},
    commands=b"rename-glob 'Old-*' 'Target/{1}'\nquit\n", preview_rows=[("/Old-a", "/Target/a", 2)],
    preview_blocked=True, preview_errors=("namespace delimiters differ",))
run("rename preview incomplete inventory blocks apply", write_state={b"Old-a": ()}, sizes={"list_denied": (b"*",)},
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", final_state={b"Old-a": ()},
    preview_rows=[("/Old-a", "/New-a", None)], preview_blocked=True, preview_errors=("LIST unavailable",))
run("rename preview controls remain escaped and read-only", version="IMAP4rev2", write_state={b"Old-\n\x1b": ()},
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", final_state={b"Old-\n\x1b": ()},
    preview_rows=[("/Old-%0A%1B", "/New-%0A%1B", 1)])
wide_preview_state = {f"Old-{index:03}".encode(): () for index in range(130)}
run("rename preview prints every mapping without truncation", installed=True, write_state=wide_preview_state,
    commands=b"rename-glob 'Old-*' 'New-{1}'\nquit\n", final_state=wide_preview_state,
    patterns=[b"%", b"*"],
    preview_rows=[(f"/Old-{index:03}", f"/New-{index:03}", 1) for index in range(130)])
bulk_state = {b"Old-A": (1,), b"Old-B": (2,), b"Old-C": (3,)}
bulk_preview = b"rename-glob 'Old-*' 'New-{1}'"
bulk_writes = [("RENAME", b"Old-" + letter, b"New-" + letter) for letter in (b"A", b"B", b"C")]
bulk_final = {b"New-A": (1,), b"New-B": (2,), b"New-C": (3,)}


def bulk_statuses(*states):
    return [(state, "/Old-" + letter, "/New-" + letter) for state, letter in zip(states, "ABC")]


run("bulk apply confirmed serial mapping on controlling PTY", installed=True, write_state=bulk_state,
    apply_steps=[{"preview": bulk_preview}, {"apply": 0}], commands=b"pwd\nquit\n",
    expected_writes=bulk_writes, final_state=bulk_final,
    apply_states=bulk_statuses("completed", "completed", "completed"))
for answer in (b"no\n", "interrupt-prompt"):
    run("bulk apply confirmation cancelled " + str(answer), write_state=bulk_state,
        apply_steps=[{"preview": bulk_preview}, {"apply": 0, "answer": answer}, {"apply": 0, "reject": True}],
        commands=b"pwd\nquit\n", expected_writes=[], final_state=bulk_state,
        apply_states=bulk_statuses("pending", "pending", "pending"))
run("bulk apply pipe cannot supply controlling-terminal confirmation", write_state=bulk_state,
    apply_steps=[{"preview": bulk_preview}, {"apply": 0}], apply_tty=False, commands=b"pwd\nquit\n",
    expected_writes=[], final_state=bulk_state, apply_states=bulk_statuses("pending", "pending", "pending"))
run("bulk apply replaced and consumed IDs never replay", write_state={b"Old-A": (1,)},
    apply_steps=[{"preview": bulk_preview}, {"preview": b"rename-glob 'Old-*' 'Later-{1}'"},
                 {"apply": 0, "reject": True}, {"apply": 1, "reject": True},
                 {"preview": b"rename-glob 'Old-*' 'Latest-{1}'"}, {"apply": 2}, {"apply": 2, "reject": True}],
    commands=b"pwd\nquit\n", expected_writes=[("RENAME", b"Old-A", b"Latest-A")],
    final_state={b"Latest-A": (1,)}, apply_states=[("completed", "/Old-A", "/Latest-A")])
run("bulk apply blocked preview is never actionable", write_state={b"Old-A": (1,)},
    apply_steps=[{"preview": b"rename-glob 'Old-*' 'Old-{1}'"}, {"apply": 0}], apply_tty=False,
    commands=b"pwd\nquit\n", expected_writes=[], final_state={b"Old-A": (1,)},
    apply_states=[("pending", "/Old-A", "/Old-A")])
for changed in ({b"Old-D": (4,)}, {b"New-B": (9,)}, {b"Old-B.Child": (8,)}):
    run("bulk apply drift after confirmation prompt " + repr(next(iter(changed))), write_state=bulk_state,
        apply_steps=[{"preview": bulk_preview}, {"apply": 0, "after_prompt": lambda server, changed=changed: server.write_state.update(changed)}],
        commands=b"pwd\nquit\n", expected_writes=[], final_state={**bulk_state, **changed},
        apply_states=bulk_statuses("pending", "pending", "pending"))
run("bulk apply stops at refusal two of three", write_state=bulk_state, write_replies={("RENAME", 2): "NO"},
    apply_steps=[{"preview": bulk_preview}, {"apply": 0}, {"apply": 0, "reject": True}], commands=b"pwd\nquit\n",
    expected_writes=bulk_writes[:2], final_state={b"New-A": (1,), b"Old-B": (2,), b"Old-C": (3,)},
    apply_states=bulk_statuses("completed", "failed", "pending"))
for changed in ({b"Old-D": (4,)}, {b"New-B": (9,)}, {b"Old-B.Child": (8,)}):
    run("bulk apply rechecks between operations " + repr(next(iter(changed))), write_state=bulk_state,
        events={("LSUB", 5): lambda server, send, changed=changed: server.write_state.update(changed)},
        apply_steps=[{"preview": bulk_preview}, {"apply": 0}], commands=b"pwd\nquit\n",
        expected_writes=bulk_writes[:1], final_state={b"New-A": (1,), b"Old-B": (2,), b"Old-C": (3,), **changed},
        apply_states=bulk_statuses("completed", "pending", "pending"))
for loss in ("before", "after", "hold-after"):
    run("bulk apply unknown result not replayed " + loss, write_state=bulk_state, write_disconnect=loss,
        apply_steps=[{"preview": bulk_preview}, {"apply": 0, "answer": "interrupt-write" if loss == "hold-after" else b"yes\n"},
                     {"apply": 0, "reject": True}], commands=b"pwd\nquit\n", expected_writes=bulk_writes[:1],
        final_state=bulk_state if loss == "before" else {b"New-A": (1,), b"Old-B": (2,), b"Old-C": (3,)},
        apply_states=bulk_statuses("unknown", "pending", "pending"))
relative_apply = {b"Parent": (), b"Parent.Archive": (), b"Parent.Old-A": (1,), b"Other": (), b"Other.Old-A": (9,)}
run("bulk apply cd cannot retarget saved raw identities", installed=True, write_state=relative_apply,
    apply_steps=[{"commands": b"cd Parent\n"}, {"preview": b"rename-glob 'Old-*' 'Archive/{1}'"},
                 {"commands": b"cd /Other\n"}, {"apply": 0}], commands=b"pwd\nquit\n",
    expected_writes=[("RENAME", b"Parent.Old-A", b"Parent.Archive.A")],
    final_state={**{raw: value for raw, value in relative_apply.items() if raw != b"Parent.Old-A"}, b"Parent.Archive.A": (1,)},
    apply_states=[("completed", "/Parent/Old-A", "/Parent/Archive/A")])
capture = "旅行/a%\\{1}"
old_raw, new_raw = ("Old-" + capture).encode(), ("New-" + capture).encode()
run("bulk apply literal Unicode separator and template capture", installed=True, version="IMAP4rev2", write_namespace=None,
    write_state={old_raw: (7,)}, apply_steps=[{"preview": bulk_preview}, {"apply": 0}], commands=b"pwd\nquit\n",
    expected_writes=[("RENAME", old_raw, new_raw)], final_state={new_raw: (7,)},
    apply_states=[("completed", "/Old-" + folderlib.display_component(capture), "/New-" + folderlib.display_component(capture))])
run("bulk apply cwd template markers stay literal", write_state={b"Parent{1}": (), b"Parent{1}.Old-x": (1,)},
    apply_steps=[{"commands": b"cd Parent{1}\n"}, {"preview": bulk_preview}, {"commands": b"cd /\n"}, {"apply": 0}],
    commands=b"pwd\nquit\n", expected_writes=[("RENAME", b"Parent{1}.Old-x", b"Parent{1}.New-x")],
    final_state={b"Parent{1}": (), b"Parent{1}.New-x": (1,)},
    apply_states=[("completed", "/Parent{1}/Old-x", "/Parent{1}/New-x")])
run("bulk apply tracks completed targets matching original wildcard", write_state={b"Box-a": (1,), b"Box-b": (2,)},
    apply_steps=[{"preview": b"rename-glob 'Box-*' 'Box-a{1}'"}, {"apply": 0}], commands=b"pwd\nquit\n",
    expected_writes=[("RENAME", b"Box-a", b"Box-aa"), ("RENAME", b"Box-b", b"Box-ab")],
    final_state={b"Box-aa": (1,), b"Box-ab": (2,)},
    apply_states=[("completed", "/Box-a", "/Box-aa"), ("completed", "/Box-b", "/Box-ab")])
run("bulk apply cancelled long terminal line cannot confirm a later ID", write_state=bulk_state,
    apply_steps=[{"preview": bulk_preview}, {"apply": 0, "answer": b"n" * 16 + b"yes\n"},
                 {"preview": bulk_preview}, {"apply": 1, "answer": b"no\n"}], commands=b"pwd\nquit\n",
    expected_writes=[], final_state=bulk_state,
    apply_states=bulk_statuses("pending", "pending", "pending") * 2)
for command in (b"rename-glob 'Old-*'\n", b"rename-glob 'Old' 'New-{1}'\n"):
    run("bulk apply invalid new preview invalidates old ID " + command.decode().strip(), write_state=bulk_state,
        apply_steps=[{"preview": bulk_preview}, {"commands": command}, {"apply": 0, "reject": True}],
        commands=b"pwd\nquit\n", expected_writes=[], final_state=bulk_state)
for command in (b"rename-glob 'unterminated\n", b"apply 'unterminated\n"):
    run("bulk apply malformed authorization command consumes old ID " + command.decode().strip(), write_state=bulk_state,
        apply_steps=[{"preview": bulk_preview}, {"commands": command}, {"apply": 0, "reject": True}],
        commands=b"pwd\nquit\n", expected_writes=[], final_state=bulk_state,
        expected_err=("Invalid quoted command",))
run("bulk apply partial refusal resumes only with fresh review", installed=True, write_state=bulk_state,
    write_replies={("RENAME", 2): "NO"},
    apply_steps=[{"preview": bulk_preview}, {"apply": 0}, {"hook": lambda server: server.write_replies.clear()},
                 {"preview": bulk_preview}, {"apply": 1}], commands=b"pwd\nquit\n",
    expected_writes=bulk_writes[:2] + bulk_writes[1:], final_state=bulk_final,
    apply_states=bulk_statuses("completed", "failed", "pending") + bulk_statuses("pending", "completed", "completed")[1:])
delete_state = {b"Box": (1, 2), b"Survivor": ()}
for installed in (False, True):
    run("delete explicit terminal yes " + ("installed" if installed else "source"), installed=installed,
        write_state=delete_state, apply_steps=[{"delete": b"rmdir Box"}],
        expected_writes=[("DELETE", b"Box")], final_state={b"Survivor": ()},
        expected_contains=("Delete target: /Box", "Messages: 2", "WARNING: nonempty folder"),
        expected_err=("DELETE confirmed",))
    for answer in (b"no\n", b"\n", b"\x04", "interrupt-prompt"):
        run("delete terminal cancellation " + repr(answer) + (" installed" if installed else " source"),
            installed=installed, write_state=delete_state,
            apply_steps=[{"delete": b"rmdir Box", "answer": answer}],
            expected_writes=[], final_state=delete_state, expected_contains=("Deletion cancelled",))
    run("delete pipe cannot approve " + str(installed), installed=installed, write_state=delete_state,
        apply_steps=[{"delete": b"rmdir Box"}], apply_tty=False,
        expected_writes=[], final_state=delete_state,
        expected_contains=("controlling terminal required; command stdin cannot confirm",))
    run("delete explicit scripted yes " + str(installed), installed=installed, write_state=delete_state,
        commands=b"rmdir Box --yes\npwd\nquit\n", expected_writes=[("DELETE", b"Box")],
        final_state={b"Survivor": ()}, expected_err=("DELETE confirmed",))
for reply in ("NO", "BAD"):
    run("delete ACL tagged " + reply, write_state=delete_state, write_replies={"DELETE": reply},
        apply_steps=[{"delete": b"rmdir Box"}], expected_writes=[("DELETE", b"Box")],
        final_state=delete_state, expected_err=("DELETE rejected",))
run("delete unknown count warning", write_state=delete_state, sizes={"denied": True},
    apply_steps=[{"delete": b"rmdir Box", "answer": b"no\n"}], expected_writes=[],
    final_state=delete_state, expected_contains=("Messages: unknown", "WARNING: message count unknown"))
run("delete literal wildcard exact raw identity", installed=True,
    write_state={b"Literal*%?": (3,), b"Literal-other": (4,), b"Literal*%?.Child": (5,)},
    commands=b"rmdir Literal%2A%25%3F --yes\nquit\n", expected_writes=[],
    final_state={b"Literal*%?": (3,), b"Literal-other": (4,), b"Literal*%?.Child": (5,)},
    expected_contains=("Folder has descendants",))
run("delete literal wildcard never expands unrelated names", installed=True,
    write_state={b"Literal*%?": (3,), b"Literal-other": (4,), b"Literal-other.Child": (5,)},
    apply_steps=[{"delete": b"rmdir Literal%2A%25%3F"}],
    expected_writes=[("DELETE", b"Literal*%?")],
    final_state={b"Literal-other": (4,), b"Literal-other.Child": (5,)},
    expected_contains=("Delete target: /Literal%2A%25%3F",))
run("delete binary mailbox uses synchronized literals", version="IMAP4rev2",
    write_state={b"line\nbreak": (8,)}, commands=b"rmdir line%0Abreak --yes\nquit\n",
    expected_writes=[("DELETE", b"line\nbreak")], final_state={})
run("delete root INBOX and unapproved descendants refused", write_state={b"iNbOx": (1,), b"Parent": (), b"Parent.Child": ()},
    commands=b"rmdir / --yes\nrmdir iNbOx --yes\nrmdir Parent --yes\nrmdir iNbOx --recursive --yes\nrmdir / --recursive --yes\nrmdir Parent --unknown --yes\nquit\n",
    expected_writes=[], final_state={b"iNbOx": (1,), b"Parent": (), b"Parent.Child": ()},
    expected_contains=("Cannot delete root or INBOX", "Folder has descendants", "Use rmdir EXACT-PATH"))
run("delete scope enumeration denied", write_state=delete_state, sizes={"list_denied": (b"Box*",)},
    commands=b"rmdir Box --yes\nquit\n", expected_writes=[], final_state=delete_state,
    expected_err=("Folder operation failed",))


def delete_add_child(server):
    server.write_state[b"Box.New"] = ()


def delete_add_message(server):
    server.write_state[b"Box"] += (3,)


def delete_replace(server):
    server.validity += 1


for hook, final, cause in (
        (delete_add_child, {**delete_state, b"Box.New": ()}, "Folder has descendants"),
        (delete_add_message, {**delete_state, b"Box": (1, 2, 3)}, "message state changed"),
        (delete_replace, delete_state, "message state changed")):
    run("delete rechecks after prompt " + hook.__name__, write_state=delete_state,
        apply_steps=[{"delete": b"rmdir Box", "after_prompt": hook}],
        expected_writes=[], final_state=final, expected_contains=(cause,))
run("delete current folder refreshes navigation before relative create", installed=True,
    write_state={b"Parent": (), b"Parent.Box": (1,)}, commands=b"cd Parent/Box\nrmdir . --yes\npwd\nmkdir New\nquit\n",
    expected_writes=[("DELETE", b"Parent.Box"), ("CREATE", b"Parent.New")],
    final_state={b"Parent": (), b"Parent.New": ()}, expected_contains=("/Parent\n",),
    expected_err=("Current folder unavailable; returned to /Parent",))
for disconnect in ("before", "after"):
    run("delete unknown outcome " + disconnect, write_state=delete_state, write_disconnect=disconnect,
        commands=b"rmdir Box --yes\npwd\nquit\n", expected_writes=[("DELETE", b"Box")],
        final_state=delete_state if disconnect == "before" else {b"Survivor": ()},
        expected_err=("DELETE outcome unknown", "Observed target path", "outcome remains unknown"))
for installed in (False, True):
    run("delete flat NIL literal slash and wildcard " + ("installed scripted yes" if installed else "source terminal yes"),
        installed=installed, write_namespace=None, write_state={b"Flat/*?": (1,), b"Flat/other": (2,)},
        apply_steps=None if installed else [{"commands": b"cd Flat%2F%2A%3F\n"}, {"delete": b"rmdir ."}],
        commands=b"cd Flat%2F%2A%3F\nrmdir . --yes\npwd\nfolders\nquit\n" if installed else b"pwd\nfolders\nquit\n",
        expected_writes=[("DELETE", b"Flat/*?")], final_state={b"Flat/other": (2,)},
        expected_contains=("Delete target: /Flat%2F%2A%3F", "Messages: 1", "/\nFlat%2Fother\n"),
        expected_err=("DELETE confirmed", "Current folder unavailable; returned to /"))
recursive_state = {b"Tree": (1,), b"Tree.A": (2,), b"Tree.A.Deep": (3,), b"Tree.B": (4,), b"Outside": (5,)}
recursive_order = [b"Tree.A.Deep", b"Tree.A", b"Tree.B", b"Tree"]
recursive_paths = ["/Tree", "/Tree/A", "/Tree/A/Deep", "/Tree/B"]
recursive_targets = [("/Tree/A/Deep", "messages: 1"), ("/Tree/A", "messages: 1"),
                     ("/Tree/B", "messages: 1"), ("/Tree", "messages: 1")]
recursive_writes = [("DELETE", raw) for raw in recursive_order]
for installed in (False, True):
    run("recursive delete fixed deepest-first scope " + str(installed), installed=installed,
        write_state=recursive_state,
        apply_steps=None if installed else [{"delete": b"rmdir Tree --recursive"}],
        commands=b"rmdir Tree --recursive --yes\npwd\nquit\n" if installed else b"pwd\nquit\n",
        expected_writes=recursive_writes, final_state={b"Outside": (5,)},
        delete_targets=recursive_targets, delete_results=[("removed", path) for path in recursive_paths])
run("recursive delete default-negative terminal blank", installed=True, write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive", "answer": b"\n"}],
    expected_writes=[], final_state=recursive_state, delete_targets=recursive_targets,
    delete_results=[("surviving", path) for path in recursive_paths], expected_contains=("confirmation cancelled",))
run("recursive delete pipe never confirms", write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive"}], apply_tty=False,
    expected_writes=[], final_state=recursive_state, delete_targets=recursive_targets,
    delete_results=[("surviving", path) for path in recursive_paths], expected_contains=("controlling terminal required",))


def recursive_new_child(server):
    server.write_state[b"Tree.New"] = ()


def recursive_new_between(server, send):
    assert server.writes == recursive_writes[:1]
    recursive_new_child(server)


run("recursive delete new child after consent scope before first write", write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive", "after_prompt": recursive_new_child}],
    expected_writes=[], final_state={**recursive_state, b"Tree.New": ()}, delete_targets=recursive_targets,
    delete_results=[("surviving", path) for path in recursive_paths + ["/Tree/New"]],
    expected_contains=("Recursive scope drift", "outside confirmed scope"))
run("recursive delete new child between serial writes", installed=True, write_state=recursive_state,
    events={("LSUB", 2): recursive_new_between}, commands=b"rmdir Tree --recursive --yes\npwd\nquit\n",
    expected_writes=recursive_writes[:1],
    final_state={**{raw: value for raw, value in recursive_state.items() if raw != b"Tree.A.Deep"}, b"Tree.New": ()},
    delete_results=[("surviving", "/Tree"), ("surviving", "/Tree/A"), ("removed", "/Tree/A/Deep"),
                    ("surviving", "/Tree/B"), ("surviving", "/Tree/New")], expected_contains=("Recursive scope drift",))
run("recursive delete inaccessible child enumeration blocks all writes", write_state=recursive_state,
    sizes={"list_denied": (b"Tree.A.%",)}, commands=b"rmdir Tree --recursive --yes\nquit\n",
    expected_writes=[], final_state=recursive_state, expected_err=("Folder operation failed",))
run("recursive delete inaccessible child count blocks all writes", installed=True, write_state=recursive_state,
    sizes={"mailboxes": {raw: {"denied": raw == b"Tree.A.Deep"} for raw in recursive_state}},
    commands=b"rmdir Tree --recursive --yes\nquit\n", expected_writes=[], final_state=recursive_state,
    expected_contains=("Recursive target inaccessible",))
run("recursive delete hidden child marker blocks incomplete scope", write_state={b"Tree": (), b"Tree.A": ()},
    sizes={"write_flags": {b"Tree.A": (b"\\HasChildren",)}}, commands=b"rmdir Tree --recursive --yes\nquit\n",
    expected_writes=[], final_state={b"Tree": (), b"Tree.A": ()}, expected_contains=("Recursive child scope is incomplete",))
run("recursive delete inconsistent subtree listing blocks all writes", write_state=recursive_state,
    sizes={"probe_rows": {b"Tree*": [(raw, b".", (), False) for raw in recursive_state if raw != b"Tree.A.Deep" and raw != b"Outside"]}},
    commands=b"rmdir Tree --recursive --yes\nquit\n", expected_writes=[], final_state=recursive_state,
    expected_contains=("Recursive scope changed during enumeration",))
run("recursive delete mid-tree refusal preserves exact survivors", installed=True, write_state=recursive_state,
    write_replies={("DELETE", 2): "NO"}, commands=b"rmdir Tree --recursive --yes\npwd\nquit\n",
    expected_writes=recursive_writes[:2], final_state={raw: value for raw, value in recursive_state.items() if raw != b"Tree.A.Deep"},
    delete_results=[("surviving", "/Tree"), ("surviving", "/Tree/A"), ("removed", "/Tree/A/Deep"), ("surviving", "/Tree/B")],
    expected_contains=("server refusal", "rejected; observed present"))
for disconnect in ("before", "after", "hold-before", "hold-after"):
    run("recursive delete ambiguous first reply " + disconnect, write_state=recursive_state,
        write_disconnect=disconnect,
        apply_steps=[{"delete": b"rmdir Tree --recursive", "answer": "interrupt-write"}] if disconnect.startswith("hold-") else None,
        commands=b"pwd\nquit\n" if disconnect.startswith("hold-") else b"rmdir Tree --recursive --yes\npwd\nquit\n",
        expected_writes=recursive_writes[:1],
        final_state=recursive_state if disconnect in ("before", "hold-before") else {
            raw: value for raw, value in recursive_state.items() if raw != b"Tree.A.Deep"},
        delete_results=[("surviving", "/Tree"), ("surviving", "/Tree/A"), ("unknown", "/Tree/A/Deep"), ("surviving", "/Tree/B")],
        expected_contains=("unknown outcome; not replayed",), expected_err=("DELETE outcome unknown",))
run("recursive delete vanished nonselectable ancestors are not replayed", installed=True,
    write_state={b"Tree": (), b"Tree.A": (), b"Tree.A.Deep": (3,), b"Outside": ()},
    sizes={"write_flags": {b"Tree": (b"\\Noselect",), b"Tree.A": (b"\\Noselect",)},
           "auto_containers": (b"Tree", b"Tree.A")},
    commands=b"cd Tree/A/Deep\nrmdir /Tree --recursive --yes\npwd\nmkdir New\nquit\n",
    expected_writes=[("DELETE", b"Tree.A.Deep"), ("CREATE", b"New")], final_state={b"Outside": (), b"New": ()},
    delete_targets=[("/Tree/A/Deep", "messages: 1"), ("/Tree/A", "messages: unknown"), ("/Tree", "messages: unknown")],
    delete_results=[("absent", "/Tree"), ("absent", "/Tree/A"), ("removed", "/Tree/A/Deep")],
    expected_contains=("container disappeared; observed not listed", "/\n"),
    expected_err=("Current folder unavailable; returned to /",))
run("recursive delete exact literal wildcard subtree excludes wire aliases", installed=True,
    write_state={b"Root*": (), b"Root*.Child?": (1,), b"Root-other": (), b"Root-other.Child": (2,)},
    commands=b"rmdir Root%2A --recursive --yes\nquit\n",
    expected_writes=[("DELETE", b"Root*.Child?"), ("DELETE", b"Root*")],
    final_state={b"Root-other": (), b"Root-other.Child": (2,)},
    delete_targets=[("/Root%2A/Child%3F", "messages: 1"), ("/Root%2A", "messages: 0")],
    delete_results=[("removed", "/Root%2A"), ("removed", "/Root%2A/Child%3F")])
run("recursive delete flat NIL name remains one exact target", installed=True, write_namespace=None,
    write_state={b"Flat/*?": (1,), b"Flat/other": (2,)},
    commands=b"cd Flat%2F%2A%3F\nrmdir . --recursive --yes\npwd\nfolders\nquit\n",
    expected_writes=[("DELETE", b"Flat/*?")], final_state={b"Flat/other": (2,)},
    delete_targets=[("/Flat%2F%2A%3F", "messages: 1")], delete_results=[("removed", "/Flat%2F%2A%3F")],
    expected_contains=("/\nFlat%2Fother\n",), expected_err=("Current folder unavailable; returned to /",))


def recursive_message_drift(server):
    server.write_state[b"Tree.B"] += (9,)


def recursive_target_disappears(server):
    del server.write_state[b"Tree.A.Deep"]


def recursive_namespace_child(server):
    server.write_state[b"Tree.New."] = ()
    server.sizes["write_flags"] = {b"Tree.New.": (b"\\Noselect",)}


run("recursive delete count drift after prompt blocks all writes", write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive", "after_prompt": recursive_message_drift}],
    expected_writes=[], final_state={**recursive_state, b"Tree.B": (4, 9)},
    delete_results=[("surviving", path) for path in recursive_paths],
    expected_contains=("Recursive target or message state drift",))
run("recursive delete selectable target disappearing is drift", installed=True, write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive", "after_prompt": recursive_target_disappears}],
    expected_writes=[], final_state={raw: value for raw, value in recursive_state.items() if raw != b"Tree.A.Deep"},
    delete_results=[("surviving", "/Tree"), ("surviving", "/Tree/A"), ("absent", "/Tree/A/Deep"), ("surviving", "/Tree/B")],
    expected_contains=("Recursive scope drift", "pending; observed not listed"))
run("recursive delete reports unexpected trailing namespace container", installed=True, write_state=recursive_state,
    apply_steps=[{"delete": b"rmdir Tree --recursive", "after_prompt": recursive_namespace_child}],
    expected_writes=[], final_state={**recursive_state, b"Tree.New.": ()},
    delete_results=[("surviving", path) for path in recursive_paths + ["/Tree/New"]],
    expected_contains=("Recursive scope drift", "outside confirmed scope"))
print(f"PASS: {len(results)} public shell scenarios; all endpoints joined")
