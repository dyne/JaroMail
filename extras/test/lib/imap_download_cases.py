"""Public export commands against the shell fixture, with durable and wire oracles."""

import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import time


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checkpoint(stage):
    with contextlib.closing(sqlite3.connect(f"file:{stage}/inventory.sqlite?mode=ro", uri=True)) as db:
        return list(db.execute("SELECT folder, uid, state, size, sha256, internaldate, flags FROM messages ORDER BY folder, uid"))


def snapshot(stage):
    return {p.name: (p.stat().st_mtime_ns, digest(p.read_bytes())) for p in stage.iterdir()}


def exercise(base_server, client_process, repo, prefix, work):
    small = (b"Message-ID: <duplicate>\r\nMIME-Version: 1.0\r\nContent-Type: application/octet-stream\r\n"
             b"Content-Transfer-Encoding: binary\r\n\r\n\x00\xff\x1b\r\n")
    large = small + bytes(range(256)) * 3072
    internaldate = "02-Jan-2020 03:04:05 +0000"
    messages = {b"Tree": [(2, small), (4294967200, large)], b"Tree.Kid": [(7, b"")], b"Other": []}
    boxes = {raw: {"messages": rows} for raw, rows in messages.items()}

    class Server(base_server):
        fault = None

        def serve(self):
            while not self.stop.is_set():
                self.serve_once()
                if self.failure:
                    return

        def export_body(self, requested, attrs, send, conn, tag):
            match = re.fullmatch(rb"\(UID RFC822.SIZE INTERNALDATE FLAGS BODY\.PEEK\[\]<(\d+)\.(\d+)>\)", attrs)
            assert match and len(requested) == 1, attrs
            uid = requested[0]
            offset, span = map(int, match.groups())
            assert 0 < span <= 256 * 1024
            attempt = dict(connection=self.connections, folder=self.selected.hex(), uid=uid,
                           offset=offset, span=span, sent=0)
            transfers.append(attempt)
            rows = self.current_size["messages"]
            if self.fault == "vanish":
                rows[:] = [row for row in rows if row[0] != uid]
                self.fault = None
            for position, (actual_uid, raw) in enumerate(rows, 1):
                if actual_uid != uid:
                    continue
                data = raw[offset:offset + span]
                response_uid, response_offset, size = uid, offset, len(raw)
                fault = self.fault
                if fault == "uid":
                    response_uid += 1
                if fault == "offset":
                    response_offset += 1
                if fault == "size":
                    size += 1
                if fault == "oversize":
                    data = b""
                literal_size = span + 1 if fault == "oversize" else len(data)
                send(f"* {position} FETCH (BODY[]<{response_offset}> {{{literal_size}}}\r\n".encode())
                if fault in ("disconnect", "cancel") and uid == 4294967200 and offset:
                    conn.sendall(data[:11])
                    attempt["sent"] = 11
                    self.literal_started.set()
                    if fault == "cancel":
                        assert conn.recv(1) == b"", "command on cancelled literal connection"
                    self.fault = None
                    return False
                if fault == "oversize":
                    assert conn.recv(1) == b"", "oversized literal was consumed"
                    return False
                # Payloads are intentionally omitted from the saved wire.
                conn.sendall(data)
                attempt["sent"] = len(data)
                flags = " ".join(sorted(self.flags_by_box[self.selected][uid]))
                if fault == "truncated":
                    send(f' UID {uid} RFC822.SIZE {size} INTERNALDATE "{internaldate}" FLAGS ({flags})\r\n'.encode())
                else:
                    send(f' UID {response_uid} FLAGS ({flags}) RFC822.SIZE {size} INTERNALDATE "{internaldate}")\r\n'.encode())
                send(f"* {position} FETCH (FLAGS ({flags}))\r\n".encode())
            send(tag + b" OK partial fetch\r\n")
            return True

    evidence = Path(os.environ.get("JARO_TEST_LOG_DIR", work)) / "imap-download-wire.json"
    records, transfers = [], []
    with Server(mode="plain", write_state=dict.fromkeys(boxes, ()), sizes={"mailboxes": boxes}) as server:
        server.flags_by_box = {raw: {uid: {r"\Flagged", "UID"} for uid, _ in rows} for raw, rows in messages.items()}
        exports = work / "downloads"
        exports.mkdir(mode=0o700)
        for account in ("export-fixture", "other-fixture"):
            (work / "mail/Accounts" / account).write_text(f'''name fixture
email fixture@example.org
login "fixture user"
password private-marker-password
proto imap
imap localhost
imap_port {server.port}
transport plain
cert ignore
''')

        full_disk = work / "disk-full-python"
        full_disk.write_text('''#!/usr/bin/env python3
import errno, gzip, os, runpy, signal, sys, tarfile
from pathlib import Path
if sys.argv[1] == "-c":
    exec(sys.argv[2])
else:
    mode = os.environ["FIXTURE_FAULT"]
    stage = Path(os.environ["FIXTURE_STAGE"])
    output = Path(os.environ["FIXTURE_OUTPUT"])
    sync = os.fsync
    def injected(fd):
        partial = stage / "message.partial"
        if mode == "disk" and partial.exists() and os.path.samestat(os.fstat(fd), partial.stat()):
            raise OSError(errno.ENOSPC, "fixture full")
        if mode == "publish-sync" and output.exists() and os.path.samestat(os.fstat(fd), output.parent.stat()):
            raise OSError(errno.ENOSPC, "fixture parent sync failed")
        return sync(fd)
    os.fsync = injected
    write = gzip.GzipFile.write
    armed = True
    def archive_write(stream, data):
        global armed
        result = write(stream, data)
        if armed and mode == "pack-full":
            armed = False
            raise OSError(errno.ENOSPC, "compression fixture full")
        if armed and mode == "pack-cancel":
            armed = False
            Path(os.environ["FIXTURE_SIGNAL"]).touch()
            signal.pause()
        return result
    gzip.GzipFile.write = archive_write
    add = tarfile.TarFile.addfile
    def archive_member(tar, info, source=None):
        if mode == "pack-corrupt" and info.name.endswith(".eml") and info.size:
            import io
            source = io.BytesIO(b"?" + source.read()[1:])
        return add(tar, info, source)
    tarfile.TarFile.addfile = archive_member
    link = os.link
    def publish_link(src, dst, **kwargs):
        if mode == "race":
            output.write_bytes(b"competitor")
        if mode == "swap-stage":
            stage.rename(stage.with_suffix(".owned"))
            stage.symlink_to(Path(os.environ["FIXTURE_FOREIGN"]), target_is_directory=True)
        return link(src, dst, **kwargs)
    os.link = publish_link
    sys.path.insert(0, str(Path(sys.argv[1]).parent))
    runpy.run_path(sys.argv[1], run_name="__main__")
''')
        full_disk.chmod(0o700)

        def issue(label, target, *, source="Tree", account="export-fixture", recursive=False, resume=False,
                  installed=False, fault=None, status=0, diagnostic="Download verified and published", residual=False):
            output = exports / (target + ".tar.gz")
            stage = exports / ("." + output.name + ".incomplete")
            runtime = prefix / "share/jaromail" if installed else repo / "src"
            entry = prefix / "bin/jaro" if installed else repo / "src/jaro"
            env = dict(os.environ, JAROMAILDIR=str(work / "mail"), JAROWORKDIR=str(runtime),
                       JARO_IMAP_TIMEOUT="5", JARO_IMAP_PYTHON="python3")
            marker = work / "compression-started"
            if fault in ("disk", "pack-full", "pack-cancel", "pack-corrupt", "race", "swap-stage", "publish-sync"):
                marker.unlink(missing_ok=True)
                env.update(JARO_IMAP_PYTHON=str(full_disk), FIXTURE_STAGE=str(stage), FIXTURE_OUTPUT=str(output),
                           FIXTURE_FAULT=fault, FIXTURE_SIGNAL=str(marker), FIXTURE_FOREIGN=str(work / "foreign"))
            server.fault = fault
            server.literal_started.clear()
            start, mark = len(server.wire), len(transfers)
            flags = (" --recursive" if recursive else "") + (" --resume" if resume else "")
            command = f"download {shlex.quote(source)} --output {shlex.quote(str(output))}{flags}\nquit\n".encode()
            with client_process([str(entry), "-q", "-a", account, "imap", "shell"], env, subprocess.PIPE) as proc:
                if fault in ("cancel", "pack-cancel"):
                    proc.stdin.write(command)
                    proc.stdin.flush()
                    if fault == "cancel":
                        assert server.literal_started.wait(6), "cancellation precondition not reached"
                    else:
                        deadline = time.monotonic() + 6
                        while not marker.exists() and time.monotonic() < deadline:
                            time.sleep(.01)
                        assert marker.exists(), "compressor did not enter the held write"
                    os.killpg(proc.pid, signal.SIGINT)
                    command = None
                out, err = proc.communicate(command, timeout=14)
            assert not server.failure, repr(server.failure)
            assert proc.returncode == status and diagnostic.encode() in err, (label, proc.returncode, err[-600:])
            assert not any(text in out + err for text in (b"private-marker-password", b"fixture user", account.encode(), small))
            if status == 0:
                assert output.is_file() and not output.stat().st_mode & 0o077
                assert residual or fault == "swap-stage" or not os.path.lexists(stage)
            elif fault == "race":
                assert output.read_bytes() == b"competitor"
            elif fault == "publish-sync":
                assert output.is_file() and (stage / "inventory.sqlite").exists()
            elif not output.is_symlink() and diagnostic != "output already exists":
                assert not os.path.lexists(output), "unverified archive published"
            wire = server.wire[start:]
            assert not any(item["command"] in ("STORE", "SELECT", "EXPUNGE", "DELETE") for item in wire)
            subset = transfers[mark:]
            records.append(dict(name=label, installed=installed, status=status, wire=wire, transfers=subset,
                                stderr=err.decode(), checkpoint=checkpoint(stage) if (stage / "inventory.sqlite").is_file() else [],
                                archive_sha256=digest(output.read_bytes()) if status == 0 else None))
            evidence.write_text(json.dumps(records, indent=2))
            print("PASS: IMAP download " + label)
            return stage, subset

        stage, sent = issue("mid-literal disconnect with durable predecessor", "resume", recursive=True,
                            fault="disconnect", status=1, diagnostic="interrupted")
        assert [entry["uid"] for entry in sent] == [2, 4294967200, 4294967200]
        assert sent[-1]["sent"] == 11
        assert [(row[1], row[2]) for row in checkpoint(stage)] == [(2, "complete"), (4294967200, "pending"), (7, "pending")]
        assert (stage / "f1-u2.eml").read_bytes() == small
        assert (stage / "message.partial").stat().st_size == sent[1]["sent"]
        assert all(not p.stat().st_mode & 0o077 for p in [stage, *stage.iterdir()])
        initial = snapshot(stage)
        for label, change in (("source", {"source": "Other"}), ("recursive", {"recursive": False}),
                              ("account", {"account": "other-fixture"})):
            settings = {"recursive": True, **change}
            _, sent = issue("reject changed " + label, "resume", resume=True, status=1, diagnostic="identity mismatch", **settings)
            assert not sent and snapshot(stage) == initial
        account_path = work / "mail/Accounts/export-fixture"
        account_text = account_path.read_text()
        account_path.write_text(account_text.replace("imap localhost", "imap 127.0.0.1"))
        _, sent = issue("same account name on changed server", "resume", recursive=True, resume=True,
                        status=1, diagnostic="identity mismatch")
        assert not sent and snapshot(stage) == initial
        account_path.write_text(account_text)
        relocated = exports / ".relocated.tar.gz.incomplete"
        stage.rename(relocated)
        _, sent = issue("relocated checkpoint bound to original output", "relocated", recursive=True,
                        resume=True, status=1, diagnostic="identity mismatch")
        assert not sent and snapshot(relocated) == initial
        relocated.rename(stage)
        selection_path = stage / "selection.json"
        encoded = selection_path.read_bytes()
        altered = json.loads(encoded)
        altered["binding"]["version"] += 1
        selection_path.write_text(json.dumps(altered))
        altered_snapshot = snapshot(stage)
        _, sent = issue("incompatible checkpoint version", "resume", recursive=True, resume=True,
                        status=1, diagnostic="identity mismatch")
        assert not sent and snapshot(stage) == altered_snapshot
        selection_path.write_bytes(encoded)
        initial = snapshot(stage)
        boxes[b"Tree.Kid"]["validity"] = 45
        _, sent = issue("descendant generation mismatch before staging", "resume", recursive=True,
                        resume=True, status=1, diagnostic="UIDVALIDITY")
        assert not sent and snapshot(stage) == initial
        del boxes[b"Tree.Kid"]["validity"]
        lock = os.open(stage, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX)
            _, sent = issue("concurrent owner refused", "resume", recursive=True, resume=True, status=1,
                            diagnostic="another writer")
            assert not sent and snapshot(stage) == initial
        finally:
            os.close(lock)
        # Arrivals in both folder membership and message membership stay outside this attempt.
        server.write_state[b"Tree.Added"] = ()
        boxes[b"Tree"]["messages"].append((4294967290, b"late"))
        _, sent = issue("installed resume retains fixed membership and skips durable bytes", "resume", recursive=True,
                        resume=True, installed=True, fault="pack-full", status=1, diagnostic="disk full")
        assert {entry["uid"] for entry in sent} == {4294967200, 7}
        assert sum(entry["sent"] for entry in sent) == len(large)
        rows = checkpoint(stage)
        assert all(r[2:] == ("complete", len(raw), digest(raw), internaldate, json.dumps(["UID", r"\Flagged"]))
                   and (stage / f"f{r[0]}-u{r[1]}.eml").read_bytes() == raw for r, raw in zip(rows, (small, large, b"")))
        assert len(rows) == 3 and all(r"\Seen" not in flags for box in server.flags_by_box.values() for flags in box.values())
        _, sent = issue("verified local retry fetches zero bytes during cancelled compression", "resume", recursive=True,
                        resume=True, fault="pack-cancel", status=130, diagnostic="interrupted")
        assert not sent
        (stage / "f1-u2.eml").write_bytes(b"!" * len(small))
        _, sent = issue("corrupt completed bytes re-fetched", "resume", recursive=True, resume=True)
        assert [e["uid"] for e in sent] == [2]
        assert subprocess.check_output(["tar", "-xOzf", exports / "resume.tar.gz", "messages/f1-u2.eml"]) == small
        boxes[b"Tree"]["messages"].pop()
        del server.write_state[b"Tree.Added"]

        stage, sent = issue("cancellation leaves only durable rows complete", "cancel", fault="cancel", status=130,
                            diagnostic="interrupted")
        assert checkpoint(stage)[0][2] == "complete" and checkpoint(stage)[1][2] == "pending"
        _, sent = issue("retry cancelled literal", "cancel", resume=True)
        assert {entry["uid"] for entry in sent} == {4294967200}
        stage, sent = issue("ENOSPC before durable completion", "disk", fault="disk", status=1, diagnostic="disk full")
        assert checkpoint(stage)[0][2] == "pending" and not (stage / "f1-u2.eml").exists()
        _, sent = issue("retry after freeing disk", "disk", resume=True)
        assert sent[0]["uid"] == 2

        size_rows = [(n * 101, small) for n in range(1, 14)]
        boxes[b"Tree"]["messages"] = list(size_rows)
        server.flags_by_box[b"Tree"] = {uid: set() for uid, _ in size_rows}
        def interrupt_inventory(server, send):
            send(b"* BYE inventory interrupted\r\n")
        server.events["FETCH", server.commands.count("FETCH") + 2] = interrupt_inventory
        stage, sent = issue("interrupt second inventory page", "inventory", status=1, diagnostic="interrupted")
        assert not sent and not checkpoint(stage)
        with contextlib.closing(sqlite3.connect(stage / "inventory.sqlite")) as db:
            assert not db.execute("SELECT * FROM inventories").fetchall()
        boxes[b"Tree"]["messages"].append((999999, small))
        mark = len(server.fetch_ranges)
        _, sent = issue("incomplete inventory restarts at first page with saved upper bound", "inventory", resume=True)
        assert [e["uid"] for e in sent] == [uid for uid, _ in size_rows] and server.fetch_ranges[mark][0] == 1
        boxes[b"Tree"]["messages"] = list(size_rows)
        stage, sent = issue("expunged captured UID recorded as missing", "missing", fault="vanish", status=1,
                            diagnostic="new output name")
        assert checkpoint(stage)[0][1:3] == (101, "missing")
        original = snapshot(stage)
        _, sent = issue("persistent missing UID retains incomplete attempt", "missing", resume=True, status=1,
                        diagnostic="captured UID 101 is missing")
        assert set(snapshot(stage)) == set(original) and checkpoint(stage)[0][2] == "missing"
        for fault in ("uid", "offset", "size", "oversize", "truncated"):
            stage, sent = issue("reject wrong FETCH " + fault, "bad-" + fault, fault=fault,
                                status=1, diagnostic="unexpected literal" if fault == "oversize" else "Download incomplete")
            assert not any(row[2] == "complete" for row in checkpoint(stage))
        boxes[b"Tree"]["messages"] = [(2, small), (4294967200, large)]
        boxes[b"Tree./etc/passwd"] = {"messages": [(90, small)]}
        boxes[b"Tree.Container.Empty"] = {"messages": []}
        server.write_state.update(dict.fromkeys((b"Tree./etc/passwd", b"Tree.Container", b"Tree.Container.Empty"), ()))
        server.sizes["write_flags"] = {b"Tree.Container": (b"\\Noselect",)}
        server.flags_by_box = {name: {uid: {r"\Flagged", "UID"} for uid, _ in box["messages"]}
                               for name, box in boxes.items()}
        stage, sent = issue("tampered compressed member fails independent hash verification", "portable",
                            recursive=True, fault="pack-corrupt", status=1, diagnostic="SHA-256 verification")
        assert len(checkpoint(stage)) == 4 and all(row[2] == "complete" for row in checkpoint(stage))
        _, sent = issue("concurrent output publication preserves winner and staged inventory", "portable",
                        recursive=True, resume=True, fault="race", status=1, diagnostic="output appeared")
        assert not sent and len(checkpoint(stage)) == 4
        (exports / "portable.tar.gz").unlink()  # Remove the fixture's own competing file.
        _, sent = issue("installed packaging retry publishes verified archive", "portable", recursive=True,
                        resume=True, installed=True)
        assert not sent
        archive = exports / "portable.tar.gz"
        listing = subprocess.check_output(["tar", "-tzf", archive], text=True).splitlines()
        assert listing[0] == "manifest.jsonl" and len(set(listing)) == len(listing) == 5
        assert all(re.fullmatch(r"messages/f[1-9][0-9]*-u[1-9][0-9]*\.eml", name) for name in listing[1:])
        subprocess.run(["gzip", "-t", archive], check=True)
        with tempfile.TemporaryDirectory(dir=work) as extracted:
            dest = Path(extracted)
            subprocess.run(["tar", "-xzf", archive, "-C", dest], check=True)
            manifest = [json.loads(line) for line in (dest / "manifest.jsonl").read_text().splitlines()]
            expected = {key: dict(value["messages"]) for key, value in boxes.items() if key != b"Other"}
            expected[b"Tree.Container"] = {}
            folders = {row["id"]: row for row in manifest if row["type"] == "folder"}
            assert {bytes.fromhex(f["raw_hex"]) for f in folders.values()} == set(expected)
            assert manifest[0] == {"type": "export", "version": 1, "recursive": True, "mailbox_encoding": "modified UTF-7"}
            actual = []
            for fid, folder in folders.items():
                raw = bytes.fromhex(folder["raw_hex"])
                assert folder["message_count"] == len(expected[raw]) and bytes.fromhex(folder["delimiter_hex"]) == b"."
                assert folder["uidvalidity"] == (None if raw == b"Tree.Container" else 44)
                if raw == b"Tree./etc/passwd":
                    assert folder["path"] == "/Tree/%2Fetc%2Fpasswd"
            for row in (r for r in manifest if r["type"] == "message"):
                raw = bytes.fromhex(folders[row["folder"]]["raw_hex"])
                original = expected[raw][row["uid"]]
                entry = dest / row["path"]
                assert entry.read_bytes() == original and entry.stat().st_mode & 0o777 == 0o600
                assert (row["size"], row["sha256"], row["internaldate"], row["flags"]) == (
                    len(original), digest(original), internaldate, ["UID", r"\Flagged"])
                actual.append((raw, row["uid"]))
            assert sorted(actual) == sorted((raw, uid) for raw, items in expected.items() for uid in items)
            if "JARO_TEST_LOG_DIR" in os.environ:
                saved = Path(os.environ["JARO_TEST_LOG_DIR"])
                shutil.copyfile(archive, saved / "synthetic-folder.tar.gz")
                shutil.copyfile(dest / "manifest.jsonl", saved / "archive-manifest.jsonl")
                (saved / "archive-members.txt").write_text("\n".join(listing) + "\n")
        _, sent = issue("existing output collision", "portable", recursive=True, resume=True,
                        status=1, diagnostic="output already exists")
        assert not sent
        foreign = work / "foreign"
        foreign.mkdir()
        sentinel = foreign / "unrelated"
        sentinel.write_text("keep")
        (exports / "link.tar.gz").symlink_to(sentinel)
        _, sent = issue("existing output symlink preserved", "link", status=1, diagnostic="output already exists")
        assert not sent and sentinel.read_text() == "keep"
        (exports / ".foreign.tar.gz.incomplete").symlink_to(foreign, target_is_directory=True)
        _, sent = issue("staging directory symlink refused", "foreign", resume=True, status=1,
                        diagnostic="cannot access output or staging")
        assert not sent and list(foreign.iterdir()) == [sentinel]
        (exports / "external").symlink_to(foreign, target_is_directory=True)
        _, sent = issue("symlinked output parent rejected", "external/nested", status=1,
                        diagnostic="cannot access output or staging")
        assert not sent and list(foreign.iterdir()) == [sentinel]
        _, sent = issue("parent traversal in requested output rejected", "../outside", status=1,
                        diagnostic="parent traversal")
        assert not sent
        stage, _ = issue("cancelled compression leaves complete records", "residual", fault="pack-cancel",
                         status=130, diagnostic="interrupted")
        (stage / "foreign-note").write_text("keep this file")
        archive_partial = stage / "archive.partial"
        archive_partial.unlink()
        archive_partial.symlink_to(sentinel)
        _, sent = issue("symlinked partial archive leaves target untouched", "residual", resume=True,
                        status=1, diagnostic="cannot access output or staging")
        assert not sent and sentinel.read_text() == "keep"
        archive_partial.unlink()
        _, sent = issue("resume after compression cancel cleans only owned members", "residual",
                        installed=True, resume=True, residual=True, diagnostic="remaining staging files retained")
        assert not sent and [p.name for p in stage.iterdir()] == ["foreign-note"]
        assert (stage / "foreign-note").read_text() == "keep this file"
        stage, _ = issue("publication archive retained on staging name replacement", "moved", fault="swap-stage")
        assert stage.is_symlink() and list(foreign.iterdir()) == [sentinel]
        assert (stage.with_suffix(".owned") / "inventory.sqlite").is_file()
        stage, _ = issue("directory sync failure after publication reported accurately", "sync", fault="publish-sync",
                         status=1, diagnostic="Verified archive published; directory sync unconfirmed")
        subprocess.run(["gzip", "-t", exports / "sync.tar.gz"], check=True)
        assert checkpoint(stage)[0][2] == "complete"
    print(f"PASS: {len(records)} public download attempts; fixture and subprocesses joined")
