"""Exercise the public source/installed shell with disposable loopback accounts."""
import json
import contextlib
import os
from pathlib import Path
import signal
import runpy
import pty
import socket
import ssl
import subprocess
import sys
import threading
import time

repo, work, prefix = map(Path, sys.argv[1:])
sys.path.insert(0, str(repo / "src/zlibs/imap-shell"))
sys.dont_write_bytecode = True
import folders as folderlib
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
                 enable_reply="confirmed"):
        self.mode, self.version, self.denied = mode, version, denied
        self.login_disabled, self.preauth = login_disabled, preauth
        self.folders, self.utf8 = folders, utf8
        self.enable_reply = enable_reply
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
        try:
            conn, _ = self.listener.accept()
            self.conn = conn
            conn.settimeout(5)
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
                                  "line": safe_line.decode("utf-8")})

                def send(response):
                    safe = response.replace(b"private-marker-password", b"<redacted>")
                    self.wire.append({"direction": "server", "command": command, "tls": secure,
                                      "line": safe.decode("utf-8")})
                    conn.sendall(response)

                if command == "CAPABILITY":
                    caps = self.version + " UIDPLUS"
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
                elif command == "ENABLE":
                    assert self.utf8 and args == [b"UTF8=ACCEPT"]
                    if self.enable_reply == "confirmed":
                        send(b"* ENABLED UTF8=ACCEPT\r\n" + tag + b" OK enabled\r\n")
                    elif self.enable_reply == "empty":
                        send(b"* ENABLED\r\n" + tag + b" OK no extensions enabled\r\n")
                    else:
                        send(tag + b" NO enable refused\r\n")
                elif command == "LIST":
                    assert self.folders is not None, "unexpected LIST"
                    assert args[0].startswith(b'"" '), "LIST reference must be empty"
                    pattern = args[0][3:]
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
                        assert end == len(args[0]) - 3
                    assert pattern in self.folders, "unexpected LIST pattern " + repr(pattern)
                    self.patterns.append(pattern)
                    values = self.folders[pattern]
                    if callable(values):
                        values = values(self.patterns.count(pattern))
                    for raw, delimiter, flags, literal in values:
                        header = b"* LIST (" + b" ".join(flags) + b") "
                        header += b"NIL " if delimiter is None else folderlib.quoted(delimiter) + b" "
                        extension = b' (CHILDINFO ("SUBSCRIBED"))' if raw.startswith(b"Archive") else b""
                        if literal:
                            send(header + b"{" + str(len(raw)).encode() + b"}\r\n" + raw + extension + b"\r\n")
                        else:
                            send(header + folderlib.quoted(raw) + extension + b"\r\n")
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
def client_process(argv, env, stdin):
    process = subprocess.Popen(argv, env=env, stdin=stdin, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
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


def run(name, installed=False, mode="implicit", cert="trusted", policy="check", ca=True,
        version="IMAP4rev1", denied=False, expected=0, commands=b"help\nhelp\nquit\n", interrupt=False,
        interpreter=None, debug=False, login_disabled=False, preauth=False, ipv6=False,
        prompt=False, terminal=False, folders=None, utf8=False, expected_out=None, patterns=None,
        expected_err=(), enable_reply="confirmed", enable_count=None):
    with Server(mode, cert, version, denied, login_disabled, preauth, ipv6, folders, utf8, enable_reply) as server:
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
        env = dict(os.environ, JAROMAILDIR=str(work / "mail"), JARO_IMAP_TIMEOUT="1",
                   password="private-marker-password", login="fixture user",
                   JAROWORKDIR=str(prefix / "share/jaromail" if installed else repo / "src"))
        env["JARO_IMAP_PYTHON"] = interpreter or str(audit)
        entry = prefix / "bin/jaro" if installed else repo / "src/jaro"
        argv = [str(entry), "-D" if debug else "-q", "-a", "shell-fixture", "imap", "shell"]
        master, slave = pty.openpty() if terminal else (None, subprocess.PIPE)
        try:
            with client_process(argv, env, slave) as process:
                if interrupt:
                    assert server.ready.wait(5), "login did not finish"
                    # Wait for the post-login CAPABILITY before cancellation.
                    limit = time.monotonic() + 3
                    while server.commands.count("CAPABILITY") < 2 and time.monotonic() < limit:
                        time.sleep(.01)
                    os.killpg(process.pid, signal.SIGINT)
                if terminal:
                    os.write(master, b"help\n\x04")
                out, err = process.communicate(None if terminal else commands, timeout=7)
        finally:
            if terminal:
                os.close(master)
                os.close(slave)
        assert process.returncode == expected, (name, process.returncode, server.wire, err[-300:].decode(errors="replace"))
        assert b"private-marker-password" not in out + err, "secret in diagnostics"
        assert b"fixture user" not in out + err, "login identifier in diagnostics"
        assert b"shell-fixture" not in out + err, "account identifier in diagnostics"
        if not interpreter:
            assert json.loads(audit.with_suffix('.json').read_text()) == {"secretExposed": False}
        assert not (Path(env["HOME"]) / ".python_history").exists()
        if expected == 0 or interrupt:
            assert server.commands.count("LOGIN") == 1, server.commands
            assert server.commands[-1] == "LOGOUT", server.commands
        if expected == 0:
            assert server.commands.count("CAPABILITY") == (3 if mode == "starttls" else 2)
            assert not any(c not in ("CAPABILITY", "STARTTLS", "LOGIN", "LOGOUT", "LIST", "ENABLE") for c in server.commands)
        if expected_out is not None:
            assert out.decode("utf-8") == expected_out, (name, out.decode("utf-8"), expected_out)
        for message in expected_err:
            assert message.encode() in err, (name, "missing expected diagnostic")
        if patterns is not None:
            assert server.patterns == patterns, (name, server.patterns, patterns)
        if enable_count is not None:
            assert server.commands.count("ENABLE") == enable_count
        assert "FETCH" not in server.commands and "SELECT" not in server.commands
        if expected and not denied and not interrupt:
            assert "LOGIN" not in server.commands, server.commands
        if mode == "starttls":
            assert all(x["tls"] for x in server.wire if x["command"] == "LOGIN")
        if policy == "ignore" and expected == 0:
            assert b"NOT verified" in err
        if mode == "plain":
            assert b"plaintext" in err
        results.append({"name": name, "exit": process.returncode, "wire": server.wire})
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
    expected_err=("Choose a selectable folder", "Header browsing is unavailable"),
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
print(f"PASS: {len(results)} public shell scenarios; all endpoints joined")
