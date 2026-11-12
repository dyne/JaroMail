#!/usr/bin/env python3
"""Exercise the real rootless daemon, authentication and runtime ownership."""
import imaplib
import os
from pathlib import Path
import re
import signal
import socket
import stat
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

    def maildir(self, folder):
        path = self.mail / folder
        for part in ("new", "cur", "tmp"):
            (path / part).mkdir(parents=True, exist_ok=True)
        return path

    def deliver(self, folder, name, content):
        path = self.maildir(folder)
        temporary = path / "tmp" / name
        temporary.write_bytes(content)
        temporary.rename(path / "new" / name)

    def uids(self, client, criterion="ALL"):
        status, data = client.uid("search", None, criterion)
        self.assertEqual(status, "OK")
        return data[0].split()

    def fetch_message(self, client, uid):
        status, data = client.uid("fetch", uid, "(BODY.PEEK[])")
        self.assertEqual(status, "OK")
        literals = [item[1] for item in data if isinstance(item, tuple)]
        self.assertEqual(len(literals), 1)
        return literals[0]

    def listed_mailboxes(self, client, pattern="*"):
        status, data = client.list('""', '"' + pattern + '"')
        self.assertEqual(status, "OK")
        names = set()
        for item in data:
            match = re.fullmatch(rb'\([^)]*\) "([^"]*)" (.+)', item)
            self.assertIsNotNone(match, item)
            self.assertEqual(match[1], b"/")
            name = match[2]
            if name.startswith(b'"'):
                self.assertTrue(name.endswith(b'"'))
                name = re.sub(rb'\\(["\\])', rb'\1', name[1:-1])
            names.add(name.decode("ascii"))
        return names

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

    def test_listing_nested_folders_and_exact_message_routing(self):
        folders = [("", "INBOX"), ("archive", "archive"),
                   ("archive/2026", "archive/2026"),
                   ("archive/2026/Quarter 1", "archive/2026/Quarter 1"),
                   ("Projects #1", "Projects #1")]
        messages = {}
        for index, (disk_folder, mailbox) in enumerate(folders):
            content = f"Subject: folder {index}\r\n\r\nbody for {mailbox}\r\n".encode("ascii")
            self.deliver(disk_folder, f"folder-{index}", content)
            messages[mailbox] = content
        self.start()
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertTrue({"known", *messages}.issubset(self.listed_mailboxes(client)))
            self.assertEqual(self.listed_mailboxes(client, "archive/%"), {"archive/2026"})
            for mailbox, content in messages.items():
                with self.subTest(mailbox=mailbox):
                    self.assertEqual(client.select('"' + mailbox + '"', readonly=True)[0], "OK")
                    uids = self.uids(client)
                    self.assertEqual(len(uids), 1)
                    self.assertEqual(self.fetch_message(client, uids[0]), content)

    def test_flag_updates_persist_in_maildir_and_after_reconnect(self):
        self.start()
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertEqual(client.select("known")[0], "OK")
            uid, = self.uids(client)
            self.assertEqual(client.uid("store", uid, "+FLAGS.SILENT", r"(\Seen \Flagged)")[0], "OK")
            status, data = client.uid("fetch", uid, "(FLAGS)")
            self.assertEqual(status, "OK")
            self.assertTrue({br"\Seen", br"\Flagged"}.issubset(imaplib.ParseFlags(data[0])))
            self.assertEqual(client.uid("store", uid, "-FLAGS.SILENT", r"(\Seen)")[0], "OK")
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertEqual(client.select("known")[0], "OK")
            self.assertEqual(self.uids(client), [uid])
            status, data = client.uid("fetch", uid, "(FLAGS)")
            self.assertEqual(status, "OK")
            flags = imaplib.ParseFlags(data[0])
            self.assertIn(br"\Flagged", flags)
            self.assertNotIn(br"\Seen", flags)
        messages = list((self.mail / "known/cur").iterdir())
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0].read_bytes(), self.original)
        flags = messages[0].name.split(":2,", 1)[1]
        self.assertIn("F", flags)
        self.assertNotIn("S", flags)
        self.assertEqual(list((self.mail / "known/new").iterdir()), [])

    def test_deleted_flag_and_expunge_remove_only_selected_message(self):
        survivor = b"Subject: survivor\r\n\r\nkeep this message\r\n"
        other_folder = b"Subject: other folder\r\n\r\nkeep this folder\r\n"
        self.deliver("known", "survivor", survivor)
        self.deliver("archive", "unrelated", other_folder)
        self.start()
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertEqual(client.select("known")[0], "OK")
            messages = {uid: self.fetch_message(client, uid) for uid in self.uids(client)}
            removed, = [uid for uid, content in messages.items() if b"local body" in content]
            kept, = [uid for uid, content in messages.items() if content == survivor]
            self.assertEqual(client.uid("store", removed, "+FLAGS.SILENT", r"(\Deleted)")[0], "OK")
            self.assertEqual(self.uids(client, "DELETED"), [removed])
            self.assertEqual(len(list((self.mail / "known/new").iterdir())) +
                             len(list((self.mail / "known/cur").iterdir())), 2)
            self.assertEqual(client.expunge()[0], "OK")
            self.assertEqual(self.uids(client), [kept])
            self.assertEqual(self.fetch_message(client, kept), survivor)
        messages = list((self.mail / "known/new").iterdir()) + list((self.mail / "known/cur").iterdir())
        self.assertEqual([message.read_bytes() for message in messages], [survivor])
        self.assertEqual((self.mail / "archive/new/unrelated").read_bytes(), other_folder)

    def test_mail_arriving_while_selected_is_visible_without_restart(self):
        arrival = b"Subject: arrived live\r\n\r\nnewly delivered body\r\n"
        self.start()
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login("fixture", "test-password")[0], "OK")
            self.assertEqual(client.select("known")[0], "OK")
            original_uids = self.uids(client)
            self.assertEqual(len(original_uids), 1)
            self.deliver("known", "arrived-live", arrival)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                self.assertEqual(client.noop()[0], "OK")
                current = self.uids(client)
                if len(current) == 2:
                    break
                time.sleep(.05)
            self.assertEqual(len(current), 2, "active IMAP session did not observe delivered mail")
            self.assertTrue(set(original_uids).issubset(current))
            added, = set(current) - set(original_uids)
            self.assertEqual(self.fetch_message(client, added), arrival)
            self.assertIsNone(self.proc.poll(), self.diagnostics())
        messages = list((self.mail / "known/new").iterdir()) + list((self.mail / "known/cur").iterdir())
        self.assertCountEqual([message.read_bytes() for message in messages], [self.original, arrival])

    def test_same_user_reads_private_generated_credentials_and_authenticates(self):
        self.env.pop("JARO_SERVE_PASSWORD")
        self.start()
        runtime = self.mail / ".imap"
        cookie = runtime / "passwd"
        for path, mode in ((runtime, 0o700), (cookie, 0o600)):
            self.assertEqual(path.stat().st_uid, os.getuid())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), mode)
        user, stored = cookie.read_text().removesuffix("\n").split(":", 1)
        self.assertEqual(user, "fixture")
        self.assertTrue(stored.startswith("{PLAIN}"))
        password = stored[len("{PLAIN}"):]
        self.assertTrue(password, "generated credential must not be empty")
        with imaplib.IMAP4("127.0.0.1", self.port, timeout=3) as client:
            self.assertEqual(client.login(user, password)[0], "OK")
            self.assertEqual(client.select("known", readonly=True)[0], "OK")
            uid, = self.uids(client)
            self.assertIn(b"local body", self.fetch_message(client, uid))


if __name__ == "__main__":
    unittest.main(verbosity=2)
