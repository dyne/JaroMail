"""Verify installed shell mutations and archive bytes against isolated Dovecot."""
import contextlib
import hashlib
import imaplib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


def exercise(entry, work, socket_parent):
    mail = work / "dovecot-mail"
    mail.mkdir(mode=0o700)
    saved = Path(os.environ.get("JARO_TEST_LOG_DIR", work))
    secret = secrets.token_hex(24)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    env = dict(os.environ, JAROMAILDIR=str(mail), JARO_SERVE_HOST="127.0.0.1",
               JARO_SERVE_PORT=str(port), JARO_SERVE_USER="fixture", JARO_SERVE_PASSWORD=secret,
               JARO_SERVE_CONFIG_ONLY="1")
    subprocess.run([entry, "-q", "serve"], env=env, check=True, stdout=subprocess.DEVNULL, timeout=20)
    conf = mail / ".imap/dovecot.conf"
    (Path(os.environ["JAROMAILDIR"]) / "Accounts/interop").write_text(
        f"name fixture\nlogin fixture\nemail fixture@example.invalid\npassword {secret}\n"
        f"proto imap\nimap 127.0.0.1\nimap_port {port}\ntransport plain\ncert check\n")
    logs = []
    def shell(commands):
        proc = subprocess.run([entry, "-q", "-a", "interop", "imap", "shell"], input=commands,
                              cwd=work, capture_output=True, text=True, timeout=30)
        assert secret not in proc.stdout + proc.stderr
        logs.append(dict(commands=commands, exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr))
        (saved / "imap-dovecot.json").write_text(json.dumps(logs, indent=2) + "\n")
        assert proc.returncode == 0, (commands, proc.stderr)
        return proc

    @contextlib.contextmanager
    def oracle():
        with imaplib.IMAP4("127.0.0.1", port, timeout=8) as client:
            assert client.login("fixture", secret)[0] == "OK"
            yield client

    first = b"From: fixture@example.invalid\r\nSubject: first\r\n\r\none\r\n"
    second = (b"Subject: second\r\nContent-Type: application/octet-stream\r\n"
              b"Content-Transfer-Encoding: base64\r\n\r\nAP8=\r\n")
    unrelated = b"Subject: unrelated\r\n\r\nsurvivor\r\n"
    # Only Unix-domain socket names need a short path. Mail, configuration,
    # indexes and logs remain under the fixture's private work directory.
    with tempfile.TemporaryDirectory(prefix="di-", dir=socket_parent) as sockets:
        args = ["dovecot", "-F", "-c", str(conf), "-o", "base_dir=" + sockets]
        with open(work / "daemon.log", "wb") as log:
            proc = subprocess.Popen(args, stdout=log, stderr=log, start_new_session=True)
            try:
                end = time.monotonic() + 12
                while time.monotonic() < end and proc.poll() is None:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=.3) as conn:
                            if conn.recv(1024).startswith(b"* OK"):
                                break
                    except OSError:
                        pass
                    time.sleep(.05)
                else:
                    raise RuntimeError("Dovecot startup failed: " + (work / "daemon.log").read_text()[-1200:])
                shell("mkdir 'Fixture Folder'\n")
                with oracle() as client:
                    for payload, flags in ((first, r"(\Flagged)"), (second, None)):
                        assert client.append('"Fixture Folder"', flags, None, payload)[0] == "OK"
                    assert client.append("known", None, None, unrelated)[0] == "OK"
                shell("mv '/Fixture Folder' '/Verified Folder'\n")
                with oracle() as client:
                    status, names = client.list('""', '"Fixture Folder"')
                    assert status == "OK" and not any(names)
                    assert client.select('"Verified Folder"', readonly=True)[0] == "OK"
                    generation = int(client.response("UIDVALIDITY")[1][0])
                    uids = [int(uid) for uid in client.uid("SEARCH", None, "ALL")[1][0].split()]
                    assert len(uids) == 2
                    metadata = {}
                    for uid, original in zip(uids, (first, second)):
                        status, fetched = client.uid("FETCH", str(uid), "(FLAGS INTERNALDATE BODY.PEEK[])")
                        assert status == "OK"
                        prefix, payload = next(item for item in fetched if isinstance(item, tuple))
                        assert payload == original, ("independent server literal differs from APPEND input", uid)
                        attrs = b" ".join([prefix, *(item for item in fetched if isinstance(item, bytes))])
                        date = re.search(rb'INTERNALDATE "([^"]+)"', attrs)[1].decode("ascii")
                        flags = {f.decode("ascii") for f in imaplib.ParseFlags(attrs)}
                        metadata[uid] = date, flags - {r"\Recent"}
                copied = shell("cd '/Verified Folder'\nls --messages --limit 2\ndu\n"
                               "download . --output isolated.tar.gz\n")
                assert "first" in copied.stdout and "second" in copied.stdout
                archive = work / "isolated.tar.gz"
                dest = work / "isolated-extract"
                dest.mkdir(mode=0o700)
                subprocess.run(["gzip", "-t", archive], check=True)
                listing = subprocess.check_output(["tar", "-tzf", archive], text=True).splitlines()
                assert set(listing) == {"manifest.jsonl", *(f"messages/f1-u{uid}.eml" for uid in uids)}
                subprocess.run(["tar", "-xzf", archive, "-C", dest], check=True)
                manifest = [json.loads(line) for line in (dest / "manifest.jsonl").read_text().splitlines()]
                folder, = [r for r in manifest if r["type"] == "folder"]
                assert folder["path"] == "/Verified%20Folder" and folder["uidvalidity"] == generation
                assert folder["message_count"] == 2
                records = {r["uid"]: r for r in manifest if r["type"] == "message"}
                for uid, data in zip(uids, (first, second)):
                    r = records[uid]
                    assert (dest / r["path"]).read_bytes() == data
                    assert (r["size"], r["sha256"]) == (len(data), hashlib.sha256(data).hexdigest())
                    assert (r["internaldate"], set(r["flags"]) - {r"\Recent"}) == metadata[uid]
                assert r"\Flagged" in records[uids[0]]["flags"]
                with oracle() as client:
                    assert client.select('"Verified Folder"', readonly=True)[0] == "OK"
                    assert client.uid("SEARCH", None, "UNSEEN")[1][0].split() == [str(u).encode() for u in uids]
                shutil.copyfile(archive, saved / "imap-dovecot.tar.gz")
                deleted = shell("rmdir '/Verified Folder' --yes\n")
                assert "DELETE confirmed" in deleted.stderr
                with oracle() as client:
                    assert client.list('""', '"Verified Folder"') == ("OK", [None])
                    assert client.select("known", readonly=True)[0] == "OK"
                    data = client.fetch("1", "(BODY.PEEK[])")[1]
                    assert [item[1] for item in data if isinstance(item, tuple)] == [unrelated]
                logs.append(dict(version=subprocess.check_output(["dovecot", "--version"], text=True).strip(),
                                 listen="127.0.0.1", unprivileged_uid=os.getuid(), members=listing,
                                 archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), manifest=manifest,
                                 status="extracted both originals before exact confirmed delete; unrelated mailbox survived"))
                (saved / "imap-dovecot.json").write_text(json.dumps(logs, indent=2) + "\n")
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
    print("PASS: installed Dovecot create, rename, page, size, verified tar extraction and explicit deletion")


if __name__ == "__main__":
    exercise(*map(Path, sys.argv[1:]))
