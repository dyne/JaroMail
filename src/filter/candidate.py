"""Compare native policy text and a saved sample, then publish that revision."""

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


def sample_bytes(root, relative):
    parts = relative.split("/")
    if (
        len(parts) < 2
        or parts[-2] not in ("cur", "new")
        or parts[0] in ("Accounts", "Groups", "cache", "logs")
    ):
        raise ValueError("expected a Maildir occurrence")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("sample must be relative to the mail root")
    with ExitStack() as stack:
        parent = os.open(root, os.O_DIRECTORY | os.O_NOFOLLOW)
        stack.callback(os.close, parent)
        for part in parts[:-1]:
            parent = os.open(part, os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            stack.callback(os.close, parent)
        fd = os.open(
            parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent
        )
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_size > 64 << 20:
                raise ValueError("invalid sample")
            return (
                before.st_dev,
                before.st_ino,
                before.st_size,
                before.st_mtime_ns,
                before.st_ctime_ns,
            )


def main():
    p = argparse.ArgumentParser(prog="jaro policy")
    p.add_argument("runtime", type=Path)
    p.add_argument("root", type=Path)
    p.add_argument("accounts", type=Path)
    p.add_argument("action", choices=("preview", "publish"))
    p.add_argument("source", choices=("Filters.txt", "Aliases.txt", "FilterProfile.txt"))
    p.add_argument("sample")
    p.add_argument("--expect")
    a = p.parse_args()
    if (a.action == "publish") != bool(a.expect):
        p.error("publish requires --expect TOKEN")
    binding = sample_bytes(a.root, a.sample)
    candidate = sys.stdin.buffer.read((64 << 20) + 1)
    if len(candidate) > 64 << 20:
        raise ValueError("candidate exceeds bound")
    with tempfile.TemporaryDirectory(prefix="jaro-candidate-") as tmp:
        stage = Path(tmp).resolve()
        (stage / "candidate").write_bytes(candidate)
        cmd = [
            a.runtime / "bin/jaro-filter-compile",
            "--root",
            a.root,
            "--accounts-dir",
            a.accounts,
            "--candidate-file",
            stage / "candidate",
            "--candidate-source",
            a.source,
            "--sample-file",
            a.sample,
        ]
        if a.expect:
            cmd.extend(["--expect-preview", a.expect])
        result = subprocess.run(cmd, stdout=subprocess.PIPE)
        if result.returncode:
            return result.returncode
        header, data = result.stdout.split(b"\n", 1)
        token, generation, sieve_size, provenance_size = header.decode("ascii").split()
        size = int(sieve_size)
        if len(data) != size + int(provenance_size):
            raise ValueError("incomplete candidate products")
        response = {"version": 1, "token": token, "generation": generation}
        if a.action == "preview":
            script = stage / "candidate.sieve"
            script.write_bytes(data[:size])
            provenance = stage / "provenance"
            provenance.write_bytes(data[size:])
            message = a.root / a.sample
            incoming = message.parent.parent
            result = subprocess.run(
                [
                    a.runtime / "sieve/filter",
                    "--dry-run",
                    "--from",
                    incoming,
                    "--message",
                    message,
                    "--to",
                    a.root,
                    "--preview-provenance",
                    provenance,
                    "--report0",
                    generation,
                    script,
                ],
                stdout=subprocess.PIPE,
            )
            if result.returncode:
                return result.returncode
            if sample_bytes(a.root, a.sample) != binding:
                return 6
            row = result.stdout.split(b"\0")
            if len(row) != 14 or row[:2] != [b"JFD1", generation.encode()] or row[-1]:
                raise ValueError("incomplete sample decision")
            response["decision"] = {
                "rule": row[4].decode(),
                "action": row[5].decode(),
                "folder": row[6].decode(),
                "status": row[9].decode(),
                "source": os.fsdecode(row[10]),
                "line": int(row[11]),
            }
        print(json.dumps(response))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as exc:
        print(
            "policy: invalid sample, source or compiler response:",
            type(exc).__name__,
            file=sys.stderr,
        )
        sys.exit(3)
