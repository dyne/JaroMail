"""Execute the installed jaro(1) examples against one disposable account."""

import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import shutil
import struct
import subprocess
import termios


def exercise_manual(server_type, client_process, apply_dialogue, prefix, work, transfers):
    runtime = prefix / "share/jaromail"
    manual = (runtime / "jaro.1.md").read_text().split("### IMAP folder shell\n")[1].split("\n### ")[0]
    browse, download, resume, organize, practice = re.findall(r"```text\n(.*?)```", manual, re.S)
    verify, = re.findall(r"```sh\n(.*?)```", manual, re.S)
    assert "IMAP folder shell" in (prefix / "share/man/man1/jaro.1").read_text()
    raw = "Projects.Café team".encode()
    rows = [(uid, f"From: demo@example.org\r\nSubject: Café {uid}\r\n\r\n".encode() + body)
            for uid, body in ((2, b"first"), (17, b"second"), (4294967200, bytes(range(256)) * 3072))]
    boxes = {b"Projects": {}, raw: {"messages": rows}, b"Projects.List-Dev": {}, b"Projects.Practice": {}}
    tree = {b"Projects": (), raw: tuple(uid for uid, _ in rows), b"Projects.List-Dev": ()}
    for box in boxes.values():
        box["status"] = sum(len(data) for _, data in box.get("messages", ()))
    env = dict(os.environ, JAROMAILDIR=str(work / "mail"), JARO_IMAP_TIMEOUT="5", TERM="dumb")
    env.pop("JAROWORKDIR", None)
    env.pop("JARO_IMAP_PYTHON", None)
    argv = [str(prefix / "bin/jaro"), "-q", "-a", "manual-fixture", "imap", "shell"]
    saved = Path(os.environ.get("JARO_TEST_LOG_DIR", work))
    records = []
    with server_type(mode="implicit", version="IMAP4rev2", write_state=tree,
                     sizes={"mailboxes": boxes}) as server:
        server.flags_by_box = {raw: {uid: {r"\Flagged"} for uid, _ in rows}}
        (work / "mail/Accounts/manual-fixture").write_text(
            f"name fixture\nemail fixture@example.org\nlogin \"fixture user\"\npassword private-marker-password\n"
            f"proto imap\nimap localhost\nimap_port {server.port}\ntransport implicit\ncert check\n"
            f"certfile {work / 'shell-tls/ca.pem'}\n")

        def batch(label, commands, directory, expected=0):
            start = len(server.wire)
            proc = subprocess.run(argv, input=commands, text=True, capture_output=True, cwd=directory,
                                  env=env, timeout=20)
            assert proc.returncode == expected, (label, proc.returncode, proc.stderr)
            assert "imap> " not in proc.stdout + proc.stderr and "private-marker" not in proc.stdout + proc.stderr
            records.append(dict(label=label, commands=commands, exit=proc.returncode,
                                stdout=proc.stdout, stderr=proc.stderr, wire=server.wire[start:]))
            return proc

        discovery = subprocess.run([argv[0], "-q", "help"], env=env, cwd=work, capture_output=True,
                                   text=True, timeout=10)
        assert discovery.returncode == 0 and "imap shell" in discovery.stdout and "man jaro" in discovery.stdout
        records.append(dict(label="launcher help", exit=discovery.returncode, commands="jaro -q help"))
        piped = batch("piped help", "help\nquit\n", work)
        assert all(len(line) <= 80 for line in piped.stdout.splitlines())
        assert "Commands: " in piped.stdout and "apply ID" in piped.stdout and "gzip -t" in piped.stdout
        listing = batch("inspect", browse, work)
        assert "/Projects/Café%20team\n" in listing.stdout
        assert [int(uid) for uid in re.findall(r"^(\d+)\t", listing.stdout, re.M)] == [4294967200, 17, 2]
        assert "TOTAL\t" in listing.stdout and "du: scanned" in listing.stderr and "du: scanned" not in listing.stdout
        refused = batch("batch remembers failure at EOF", "cd /absent\npwd\n", work, 1)
        assert "Folder operation failed" in refused.stderr and refused.stdout == "/\n"
        _narrow_terminal(client_process, argv, env, work, records)

        for mode in ("fresh", "resume"):
            directory = work / ("manual-" + mode)
            directory.mkdir(mode=0o700)
            archive = directory / "cafe-team.tar.gz"
            mark = len(transfers)
            if mode == "resume":
                server.fault = "disconnect"
                interrupted = batch("interrupted download", download, directory, 1)
                assert "interrupted" in interrupted.stderr and not archive.exists()
                assert {t["uid"] for t in transfers[mark:]} == {2, 17, 4294967200}
                mark = len(transfers)
            result = batch(mode + " download", resume if mode == "resume" else download, directory)
            assert not result.stdout and "Download verified and published" in result.stderr
            assert {t["uid"] for t in transfers[mark:]} == ({4294967200} if mode == "resume" else {2, 17, 4294967200})
            script = subprocess.run(["sh", "-ec", verify], cwd=directory, capture_output=True, text=True, timeout=10)
            assert script.returncode == 0 and "All listed messages verified" in script.stdout, script.stderr
            dest = directory / "verified-copy"
            entries = [json.loads(line) for line in (dest / "manifest.jsonl").read_text().splitlines()]
            folder, = [e for e in entries if e["type"] == "folder"]
            assert bytes.fromhex(folder["raw_hex"]) == raw and folder["delimiter_hex"] == "2e"
            assert {e["uid"]: (dest / e["path"]).read_bytes() for e in entries if e["type"] == "message"} == dict(rows)
            shutil.copyfile(archive, saved / ("manual-" + mode + ".tar.gz"))
            records.append(dict(label=mode + " verification", commands=verify, exit=script.returncode,
                                stdout=script.stdout, manifest=entries, sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))

        create, move, preview = organize.splitlines()
        mkdir, delete = practice.splitlines()
        def confirmed(label, commands, steps):
            master, slave = pty.openpty()
            start = len(server.wire)
            try:
                with client_process(argv, env, subprocess.PIPE, slave, cwd=work) as proc:
                    out, prompts = apply_dialogue(proc, master, server, steps)
                    tail, err = proc.communicate(b"quit\n", timeout=10)
                    assert proc.returncode == 0, err
            finally:
                os.close(master)
                os.close(slave)
            records.append(dict(label=label, commands=commands, exit=proc.returncode, stdout=(out + tail).decode(),
                                stderr=err.decode(), terminal=prompts.decode(), wire=server.wire[start:]))
            return out, prompts

        out, _ = confirmed("review and reorganize", organize + "apply ID\n", [
            {"commands": (create + "\n" + move + "\n").encode()}, {"preview": preview.encode()}, {"apply": 0}])
        assert b"completed\t/Projects/List-Dev\t->\t/Projects/Lists/Dev" in out
        batch("create empty practice folder", mkdir, work)
        _, prompts = confirmed("confirm exact deletion", delete, [{"delete": delete.encode()}])
        assert b"Delete /Projects/Practice? [no] Type yes: " in prompts
        assert server.writes == [("CREATE", b"Projects.Lists"), ("RENAME", raw, "Projects.Café archive".encode()),
                                 ("RENAME", b"Projects.List-Dev", b"Projects.Lists.Dev"),
                                 ("CREATE", b"Projects.Practice"), ("DELETE", b"Projects.Practice")]
        assert server.write_state == {b"Projects": (), "Projects.Café archive".encode(): tree[raw],
                                      b"Projects.Lists": (), b"Projects.Lists.Dev": ()}
    (saved / "imap-installed-manual.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS: {len(records)} installed manual checks with verified TLS, dot hierarchy, spaces and Unicode")


def _narrow_terminal(client_process, argv, env, work, records):
    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 32, 0, 0))
        attrs = termios.tcgetattr(slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        with client_process(argv, env, slave, slave, cwd=work, stdout=slave) as proc:
            os.write(master, "help\ncd '/Projects/Café team'\nls --messages --limit 1\nquit\n".encode())
            _, err = proc.communicate(timeout=10)
            assert proc.returncode == 0 and err.count(b"imap> ") == 4, err
    finally:
        os.close(slave)
        data = bytearray()
        try:
            while True:
                block = os.read(master, 4096)
                if not block:
                    break
                data.extend(block)
        except OSError as exc:
            if exc.errno != errno.EIO:
                raise
        finally:
            os.close(master)
    lines = data.decode().splitlines()
    assert "UID 4294967200" in lines and any("SUBJECT Café" in line for line in lines), lines
    assert all(len(line) <= 32 for line in lines) and any("Commands:" in line for line in lines), lines
    records.append(dict(label="32-column PTY", columns=32, stdout=lines, stderr=err.decode(), exit=proc.returncode))
