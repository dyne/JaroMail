"""Local-only TLS contract tests; Python is not a Jaromail runtime dependency."""
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading

repo, work = map(Path, sys.argv[1:])
tls = work / "tls"
tls.mkdir()
(tls / "empty-trust").mkdir()


def openssl(*args):
    subprocess.run(["openssl", *args], cwd=tls, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=10)


openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=Fixture CA", "-keyout", "ca.key", "-out", "ca.pem")
for label, dns in (("trusted", "localhost"), ("wrong", "wrong.invalid")):
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={dns}",
            "-keyout", f"{label}.key", "-out", f"{label}.csr")
    (tls / f"{label}.ext").write_text(f"subjectAltName=DNS:{dns}\n")
    openssl("x509", "-req", "-in", f"{label}.csr", "-CA", "ca.pem",
            "-CAkey", "ca.key", "-CAcreateserial", "-days", "1",
            "-extfile", f"{label}.ext", "-out", f"{label}.pem")


class Endpoint:
    def __init__(self, label="trusted", mode="implicit"):
        self.mode = mode
        self.received = b""
        self.pre_tls = b""
        self.error = None
        self.failure = None
        self.connection = None
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(6)
        self.port = self.listener.getsockname()[1]
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(tls / f"{label}.pem", tls / f"{label}.key")
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def line(self, conn):
        line = b""
        while not line.endswith(b"\n"):
            byte = conn.recv(1)
            if not byte:
                break
            line += byte
            if len(line) > 4096:
                raise AssertionError("unbounded fixture input")
        return line

    def serve(self):
        try:
            conn, _ = self.listener.accept()
            self.connection = conn
            conn.settimeout(4)
            if self.mode == "timeout":
                self.stop.wait(5)
                return
            if self.mode.startswith("starttls"):
                conn.sendall(b"* OK fixture IMAP ready\r\n")
                line = self.line(conn)
                self.pre_tls += line
                # s_client probes capabilities before requesting STARTTLS.
                if b"CAPABILITY" in line.upper():
                    tag = line.split()[0]
                    conn.sendall(b"* CAPABILITY IMAP4rev1 STARTTLS\r\n" + tag + b" OK capability\r\n")
                    line = self.line(conn)
                    self.pre_tls += line
                assert b"STARTTLS" in line.upper(), self.pre_tls
                tag = line.split()[0]
                if self.mode == "starttls-reject":
                    conn.sendall(tag + b" NO unavailable\r\n")
                    return
                conn.sendall(tag + b" OK begin TLS\r\n")
            conn = self.context.wrap_socket(conn, server_side=True)
            self.connection = conn
            if self.mode == "implicit":
                conn.sendall(b"* OK fixture IMAP ready\r\n")
            while True:
                line = self.line(conn)
                self.received += line
                if not line:
                    break
                if self.mode == "command-timeout":
                    self.stop.wait(5)
                    break
                conn.sendall(line.split()[0] + b" OK fixture\r\n")
                if b"LOGOUT" in line:
                    conn.unwrap().close()
                    self.connection = None
                    break
        except (ssl.SSLError, ConnectionError, OSError) as exc:
            self.error = type(exc).__name__
        except Exception as exc:
            self.failure = exc
        finally:
            if self.connection is not None:
                self.connection.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.stop.set()
        if self.connection is not None:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(7)
        assert not self.thread.is_alive(), "fixture endpoint did not settle"
        if self.failure is not None:
            raise self.failure


script = r'''
source "$1/extras/test/lib/module_helpers.zsh"
source "$1/src/zlibs/imap"
imap=localhost imap_port="$2" transport="$3" cert=check certfile="$4"
JARO_IMAP_TIMEOUT=2
print -r -- $'B00001 LOGIN "fixture" "secret-marker"\nB00002 LOGOUT' | run_imap_query
'''


def check(name, *, label="trusted", mode="implicit", custom=True, success=True,
          system=False, timeout=False, authenticated=False):
    with Endpoint(label, mode) as endpoint:
        env = dict(os.environ, SSL_CERT_FILE=str(tls / "ca.pem") if system else "/dev/null",
                   SSL_CERT_DIR=str(tls / "empty-trust"))
        result = subprocess.run(["zsh", "-c", script, "fixture", str(repo),
                                 str(endpoint.port), "starttls" if mode.startswith("starttls") else "TLS1",
                                 str(tls / "ca.pem") if custom else ""],
                                env=env, capture_output=True, timeout=7)
        assert (result.returncode == 0) == success, (name, result.returncode, result.stderr)
        if success or authenticated:
            assert b'LOGIN "fixture" "secret-marker"' in endpoint.received, (name, endpoint.error)
        else:
            assert b"LOGIN" not in endpoint.received, (name, endpoint.received)
        assert b"LOGIN" not in endpoint.pre_tls, (name, endpoint.pre_tls)
        assert b"secret-marker" not in result.stderr, (name, result.stderr)
        if timeout:
            assert b"deadline" in result.stderr, (name, result.stderr)
    print(f"PASS: local TLS {name}; endpoint joined")


check("system trust", custom=False, system=True)
check("custom trust")
check("wrong hostname", label="wrong", success=False)
check("untrusted chain", custom=False, success=False)
check("STARTTLS trusted", mode="starttls")
check("STARTTLS rejection", mode="starttls-reject", success=False)
check("STARTTLS verification failure", mode="starttls", label="wrong", success=False)
check("handshake timeout", mode="timeout", success=False, timeout=True)
check("command timeout", mode="command-timeout", success=False, timeout=True, authenticated=True)
