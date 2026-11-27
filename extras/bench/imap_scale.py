"""Measure the public installed IMAP shell against the on-demand loopback dataset."""
import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from imap_scale_server import BUSY, GROUPS, HEADER, LARGE, LEAVES, MIB, SMALL, Server, uid


def utc():
    return datetime.now(timezone.utc).isoformat()


def sample_stage(stage):
    total, partial = 0, None
    with contextlib.suppress(FileNotFoundError):
        for path in stage.iterdir():
            with contextlib.suppress(FileNotFoundError):
                st = path.stat()
                total += st.st_size
                if path.name == "archive.partial":
                    partial = (st.st_dev, st.st_ino)
    return total, partial


def measured(entry, work, saved, label, commands, server, output=None):
    # wait4's per-invocation rusage includes waited descendants of the launcher
    # on Linux. Never reuse cumulative RUSAGE_CHILDREN or include the server.
    begin, start = utc(), time.monotonic()
    peak_stage = 0
    first_output = partial_inode = None
    cancel_sent = None
    stage = output.with_name("." + output.name + ".incomplete") if output else None
    with tempfile.TemporaryFile() as stdin, (saved / (label + ".stdout")).open("wb") as stdout, \
            (saved / (label + ".stderr")).open("wb") as stderr:
        stdin.write((commands + "\nquit\n").encode())
        stdin.seek(0)
        p = subprocess.Popen([entry, "-q", "-a", "scale", "imap", "shell"], cwd=work,
                             stdin=stdin, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            while time.monotonic() - start < 120:
                if stage:
                    staged, partial = sample_stage(stage)
                    peak_stage = max(peak_stage, staged)
                    partial_inode = partial or partial_inode
                    if first_output is None:
                        with contextlib.suppress(FileNotFoundError):
                            first_output = output.stat().st_size
                else:
                    peak_stage = max(peak_stage, sum(sample_stage(p)[0] for p in work.glob(".*.incomplete")))
                if server.request_cancel.is_set() and cancel_sent is None:
                    cancel_sent = time.monotonic() - start
                    os.killpg(p.pid, signal.SIGINT)
                child, status, usage = os.wait4(p.pid, os.WNOHANG)
                if child:
                    p.returncode = os.waitstatus_to_exitcode(status)
                    break
                time.sleep(.003)
            else:
                raise TimeoutError(label)
        finally:
            if p.returncode is None:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(p.pid, signal.SIGKILL)
                p.wait(timeout=5)
    # A process may publish and exit between two filesystem observations.
    if output and output.is_file():
        st = output.stat()
        first_output = st.st_size if first_output is None else first_output
        assert first_output == st.st_size
        linked_stage = partial_inode == (st.st_dev, st.st_ino)
    else:
        linked_stage = None
    return dict(label=label, command=commands, started=begin, finished=utc(), elapsed_s=round(time.monotonic()-start, 4),
                exit=p.returncode, peak_rss_kib=usage.ru_maxrss // (1024 if sys.platform == "darwin" else 1),
                commands=dict(server.commands), command_count=sum(server.commands.values()),
                client_to_server=server.rx, server_to_client=server.tx, header_literal_bytes=server.header_bytes,
                body_literal_bytes=sum(row[2] for row in server.transfers), inventory_rows=server.inventory_rows,
                size_rows=server.size_rows, sampled_peak_stage_bytes=peak_stage, first_visible_output_bytes=first_output,
                output_matches_staged_inode=linked_stage, cancel_signal_elapsed_s=cancel_sent)


def verify_archive(output, lengths, saved, label):
    names = [f"messages/f1-u{uid(i)}.eml" for i in (1, 2)]
    subprocess.run(["gzip", "-t", output], check=True)
    listing = subprocess.check_output(["tar", "-tzf", output], text=True).splitlines()
    assert listing == ["manifest.jsonl", *names]
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        subprocess.run(["tar", "-xzf", output, "-C", temp], check=True)
        root = Path(temp)
        rows = [json.loads(line) for line in (root / "manifest.jsonl").read_text().splitlines()]
        assert [r["type"] for r in rows] == ["export", "folder", "message", "message"]
        assert rows[1]["message_count"] == 2 and rows[1]["uidvalidity"] == 7
        assert bytes.fromhex(rows[1]["raw_hex"]) == (SMALL if lengths[1] == MIB else LARGE).encode()
        for pos, (name, n) in enumerate(zip(names, lengths), 1):
            # Independent recipe: header, then byte 0..255 repeated from zero.
            expected = hashlib.sha256(HEADER)
            block = bytes(range(256)) * 512
            repeat, tail = divmod(n - len(HEADER), len(block))
            for _ in range(repeat):
                expected.update(block)
            expected.update(block[:tail])
            got = hashlib.sha256()
            with (root / name).open("rb") as eml:
                for chunk in iter(lambda: eml.read(131072), b""):
                    got.update(chunk)
            row = rows[pos + 1]
            assert (row["uid"], row["path"], row["size"], row["sha256"], row["flags"]) == (
                uid(pos), name, n, expected.hexdigest(), [r"\Flagged"])
            assert got.digest() == expected.digest() and (root / name).stat().st_size == n
        shutil.copyfile(root / "manifest.jsonl", saved / (label + ".manifest.jsonl"))
    shutil.copyfile(output, saved / (label + ".tar.gz"))
    with output.open("rb") as stream:
        h = hashlib.sha256()
        for chunk in iter(lambda: stream.read(131072), b""):
            h.update(chunk)
    return h.hexdigest()


def exercise(entry, work):
    saved = Path(os.environ.get("JARO_TEST_LOG_DIR", work)) / "imap-scale"
    saved.mkdir(mode=0o700)
    busy_path = "/" + BUSY.replace(".", "/")
    metrics = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        (Path(os.environ["JAROMAILDIR"]) / "Accounts/scale").write_text(
            f"name fixture\nemail scale@example.invalid\nlogin fixture\npassword fixture-only\n"
            f"proto imap\nimap 127.0.0.1\nimap_port {port}\ntransport plain\ncert check\n")

        def run(label, commands, *, fast=True, cancel=False, output=None):
            with Server(listener, fast=fast, interrupt=cancel) as server:
                result = measured(entry, work, saved, label, commands, server, output)
            metrics.append(result)
            transcript = dict(commands=server.wire, transfers=server.transfers)
            (saved / (label + ".wire.json")).write_text(json.dumps(transcript) + "\n")
            (saved / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
            assert result["exit"] == (130 if cancel else 0), (label, (saved / (label + ".stderr")).read_text()[-600:])
            assert not set(server.commands).difference(("CAPABILITY", "LOGIN", "LOGOUT", "LIST", "EXAMINE", "FETCH", "UID", "STATUS"))
            return server, result, (saved / (label + ".stdout")).read_text()

        nav, _, out = run("navigation", "folders\ncd Archive\nfolders\ncd G000\nfolders\ncd B000\npwd")
        assert out.splitlines() == ["Archive [container]", "Exports [container]",
                                    *(f"G{i:03} [container]" for i in range(GROUPS)),
                                    *(f"B{i:03}" for i in range(LEAVES)), busy_path]
        assert [a for c, a in nav.wire if c == "LIST"] == ['"" "%"', '"" "Archive.%"', '"" "Archive.G000.%"']
        assert not nav.transfers and not nav.inventory_rows and not nav.header_bytes

        headers, _, out = run("headers", f"cd {busy_path}\nls --messages --limit 25")
        assert [int(line.split("\t", 1)[0]) for line in out.splitlines()[1:]] == [
            uid(i) for i in range(1_000_000, 999_975, -1)]
        assert headers.inventory_rows == 25 and headers.header_bytes == 25 * len(HEADER)
        assert headers.commands["FETCH"] == headers.commands["UID"] == 3 and not headers.transfers

        fast, _, out = run("du-fast", "du /Archive --depth 2 --top 3")
        expected = GROUPS * sum(100 * (1024 + i) for i in range(LEAVES)) + (1_000_000 - 100) * 1024
        assert f"TOTAL\t{expected} mailbox bytes\tserver-reported\tcomplete; 10000/10000" in out
        assert out.split("SUBTREE_COVERAGE\n", 1)[1].splitlines()[0].startswith(busy_path + "\t1024000000\t")
        assert not fast.transfers and not fast.header_bytes and not fast.commands["FETCH"] and not fast.commands["UID"]
        assert sum("RETURN (STATUS (SIZE))" in a for _, a in fast.wire) == GROUPS + 1

        fallback, _, out = run("du-fallback", "du /Archive/G099 --depth 1 --top 3", fast=False)
        group_bytes = sum(100 * (1024 + i) for i in range(LEAVES))
        assert f"TOTAL\t{group_bytes} mailbox bytes\tmeasured\tcomplete; 100/100" in out
        assert out.split("SUBTREE_COVERAGE\n", 1)[1].splitlines()[0].startswith("/Archive/G099/B099\t112300\t")
        assert not fallback.header_bytes and not fallback.transfers
        assert fallback.inventory_rows == fallback.size_rows == 10_000 and not fallback.commands["STATUS"]
        assert all(m["sampled_peak_stage_bytes"] == 0 for m in metrics)

        peaks = []
        for label, folder, n in (("small", SMALL, MIB), ("large", LARGE, 128 * MIB)):
            output = work / (label + ".tar.gz")
            command = f"download /{folder.replace('.', '/')} --output {output.name}"
            server, result, _ = run(label, command, output=output)
            peaks.append(result["peak_rss_kib"])
            assert result["body_literal_bytes"] == n + 4096
            assert max(t[2] for t in server.transfers) == 256 * 1024
            result["archive_sha256"] = verify_archive(output, (4096, n), saved, label)
            assert not output.with_name("." + output.name + ".incomplete").exists()
            if label == "large":
                assert result["output_matches_staged_inode"] and result["sampled_peak_stage_bytes"] >= n
        assert peaks[1] < 96 * 1024 and peaks[1] - peaks[0] < 24 * 1024, ("export RSS KiB", peaks)

        output = work / "restart.tar.gz"
        command = "download /Exports/Large --output restart.tar.gz"
        interrupted, result, _ = run("cancel", command, output=output, cancel=True)
        stage = work / ".restart.tar.gz.incomplete"
        assert not output.exists() and result["first_visible_output_bytes"] is None
        assert interrupted.transfers[-1] == [uid(2), 8 * MIB, 1024]
        assert (stage / "message.partial").stat().st_size == 8 * MIB
        with contextlib.closing(sqlite3.connect(stage / "inventory.sqlite")) as db:
            assert db.execute("SELECT uid,state FROM messages ORDER BY uid").fetchall() == [
                (uid(1), "complete"), (uid(2), "pending")]
        assert (stage / f"f1-u{uid(1)}.eml").stat().st_size == 4096
        result["retained_stage_bytes"] = sample_stage(stage)[0]
        resumed, result, _ = run("resume", command + " --resume", output=output)
        assert {t[0] for t in resumed.transfers} == {uid(2)} and resumed.transfers[0][1] == 0
        assert result["body_literal_bytes"] == 128 * MIB and result["output_matches_staged_inode"]
        result["archive_sha256"] = verify_archive(output, (4096, 128 * MIB), saved, "resume")
        assert result["archive_sha256"] == next(m["archive_sha256"] for m in metrics if m["label"] == "large")
        assert not stage.exists()
    (saved / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    dataset = dict(folders=2 + GROUPS + GROUPS * LEAVES + 2,
                   logical_messages=(GROUPS * LEAVES - 1) * 100 + 1_000_000 + 4,
                   largest_mailbox=1_000_000, large_message_bytes=128 * MIB, uid_stride=4093,
                   platform=platform.platform(), python=platform.python_version(), launcher=str(entry),
                   staging="sampled visible regular-file lengths every ~3ms; excludes unnamed temporary files and filesystem overhead",
                   rss="wait4 per launcher and its waited descendants, maximum resident process size in KiB")
    (saved / "dataset.json").write_text(json.dumps(dataset, indent=2) + "\n")
    print(f"PASS: {len(metrics)} installed scale cases, {dataset['folders']} folders, {dataset['logical_messages']} logical messages")


if __name__ == "__main__":
    exercise(*map(Path, sys.argv[1:]))
