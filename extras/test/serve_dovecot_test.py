#!/usr/bin/env python3
"""Exercise the real rootless daemon, authentication and runtime ownership."""
import imaplib
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest


REPO = Path(__file__).resolve().parents[2]


class ServeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(
            prefix="", dir=os.environ.get("JARO_SERVE_TEST_TMPDIR"))
        self.root = Path(self.temp.name)
        # Spaces and # must remain literal in config and userdb paths.
        self.mail = self.root / "m #"
        for part in ("cur", "new", "tmp"):
            (self.mail / "known" / part).mkdir(parents=True)
        self.message = self.mail / "known/new/fixture"
        self.message.write_bytes(b"From: test@example.invalid\nSubject: serve fixture\n\nlocal body\n")
        self.original = self.message.read_bytes()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.env = dict(os.environ, JAROMAILDIR=str(self.mail),
                        JAROWORKDIR=str(REPO / "src"), JARO_SERVE_USER="fixture",
                        JARO_SERVE_PASSWORD="test-password", JARO_SERVE_PORT=str(self.port))
        self.env.pop("JARO_SERVE_CONFIG_ONLY", None)
        self.proc = None
        self.log = (self.root / "launch.log").open("w+")

    def tearDown(self):
        if self.proc is not None:
            # Only the process group created by this fixture is signalled.
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        self.log.close()
        self.temp.cleanup()

    def run_jaro(self, **overrides):
        return subprocess.run([str(REPO / "src/jaro"), "serve"],
                              env=dict(self.env, **overrides), capture_output=True,
                              text=True, timeout=10)

    def diagnostics(self):
        self.log.flush()
        self.log.seek(0)
        daemon_log = self.mail / ".imap/log/dovecot.log"
        return self.log.read() + (daemon_log.read_text() if daemon_log.exists() else "")

    def start(self):
        self.proc = subprocess.Popen([str(REPO / "src/jaro"), "serve"], env=self.env,
                                     stdout=self.log, stderr=self.log, start_new_session=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.fail(self.diagnostics())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.2) as conn:
                    conn.settimeout(.5)
                    if conn.recv(1024).startswith(b"* OK"):
                        return
            except (OSError, TimeoutError):
                pass
            time.sleep(.05)
        self.fail("No IMAP greeting: " + self.diagnostics())

    def test_config_login_mail_and_lifetime_lock(self):
        generated = self.run_jaro(JARO_SERVE_CONFIG_ONLY="1")
        self.assertEqual(generated.returncode, 0, generated.stderr)
        conf = self.mail / ".imap/dovecot.conf"
        parsed = subprocess.run(["doveconf", "-c", str(conf), "-n"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.start()
        passwd = self.mail / ".imap/passwd"
        before = (conf.read_bytes(), passwd.read_bytes())
        # Neither a different port nor config-only may bypass runtime ownership.
        for config_only in ("0", "1"):
            rejected = self.run_jaro(JARO_SERVE_PASSWORD="replacement",
                                     JARO_SERVE_PORT=str(self.port + 1),
                                     JARO_SERVE_CONFIG_ONLY=config_only)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual((conf.read_bytes(), passwd.read_bytes()), before)
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertEqual(client.select("known")[0], "OK")
            status, messages = client.fetch("1", "BODY.PEEK[]")
            self.assertEqual(status, "OK")
            self.assertIn(b"local body", messages[0][1])
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=5) as client:
            with self.assertRaises(imaplib.IMAP4.error):
                client.login("fixture", "wrong-password")
        # Selecting a Maildir can legitimately rename new/ entries into cur/.
        messages = list((self.mail / "known/new").iterdir()) + list((self.mail / "known/cur").iterdir())
        self.assertEqual([message.read_bytes() for message in messages], [self.original])
        self.proc.terminate()  # Signal jaro, not its Dovecot child.
        self.assertEqual(self.proc.wait(timeout=5), 143, self.diagnostics())
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", self.port), timeout=.2)
        retry = self.run_jaro(JARO_SERVE_CONFIG_ONLY="1")
        self.assertEqual(retry.returncode, 0, retry.stderr)

    def test_busy_listener_preserves_credentials(self):
        generated = self.run_jaro(JARO_SERVE_CONFIG_ONLY="1")
        self.assertEqual(generated.returncode, 0, generated.stderr)
        passwd = self.mail / ".imap/passwd"
        original = passwd.read_bytes()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", self.port))
            listener.listen()
            rejected = self.run_jaro(JARO_SERVE_PASSWORD="replacement")
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(passwd.read_bytes(), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
