"""Revision-checked account directives and native recipient group files."""

import argparse
import email.utils
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess
import sys


LIMIT = 1024 * 1024
ID = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._+@-]{0,191}")
SCALARS = (
    "name",
    "email",
    "login",
    "proto",
    "imap",
    "smtp",
    "imap_port",
    "smtp_port",
    "transport",
    "auth",
    "cert",
)
GROUP_MODES = {"individual", "cc", "carboncopy", "bcc"}


class Conflict(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def get_bytes(directory, name):
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    with os.fdopen(fd, "rb") as stream:
        st = os.fstat(fd)
        require(stat.S_ISREG(st.st_mode) and st.st_size <= LIMIT, "invalid record")
        data = stream.read(LIMIT + 1)
        require(len(data) == st.st_size, "record changed while reading")
        return data


def account_fields(runtime, name, data):
    # The native parser owns quoting, historical aliases, defaults and validation.
    script = """
        error() { :; }
        source "$1" || exit 1
        account="$2"; shift 2
        _account_load_file /dev/stdin || exit 1
        for field in "$@"; do print -rN -- "${(P)field}"; done
        print -rN -- "${#folders}" "${folders[@]}" "${#exclude}" "${exclude[@]}"
    """
    proc = subprocess.run(
        [
            "zsh",
            "-fc",
            script,
            "read-account",
            runtime / "zlibs/accounts",
            name,
            *SCALARS,
        ],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    require(proc.returncode == 0, "invalid account")
    values = proc.stdout.decode().split("\0")
    require(values.pop() == "", "incomplete account fields")
    require(len(values) >= len(SCALARS) + 2, "incomplete account fields")
    result = dict(zip(SCALARS, values[: len(SCALARS)]))
    pos = len(SCALARS)
    for key in ("folders", "exclude"):
        count = int(values[pos])
        result[key] = values[pos + 1 : pos + count + 1]
        pos += count + 1
    require(pos == len(values), "incomplete native parser result")
    return result


def group_fields(data):
    lines = data.decode().splitlines()
    mode = "individual"
    if lines and lines[0].startswith("#mode "):
        mode = lines[0].split(maxsplit=1)[1].strip()
    require(mode in GROUP_MODES, "unsupported group mode")
    return {
        "mode": mode,
        "members": [s for s in lines if s.strip() and not s.startswith("#")],
    }


def single_line(value):
    require(
        type(value) is str
        and len(value) <= 4096
        and all(ord(char) >= 32 and ord(char) != 127 for char in value),
        "invalid single-line value",
    )
    return value


def mailbox(runtime, value):
    require(
        value
        and not value.startswith("-")
        and "/" not in value
        and subprocess.run(
            [runtime / "bin/jaro-filter-compile", "--check-address", value],
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0,
        "invalid mailbox",
    )


def group_edit(runtime, old, changes):
    require(
        changes and not (changes.keys() - {"mode", "members"}), "unknown group field"
    )
    fields = dict(group_fields(old), **changes)
    require(fields["mode"] in GROUP_MODES, "unsupported group mode")
    members = fields["members"]
    require(
        isinstance(members, list) and len(members) <= 1024, "invalid group membership"
    )
    seen = set()
    for entry in members:
        single_line(entry)
        parsed = email.utils.getaddresses([entry])
        require(len(parsed) == 1, "expected one group mailbox per member")
        address = parsed[0][1]
        mailbox(runtime, address)
        require(address.lower() not in seen, "duplicate group member")
        seen.add(address.lower())
    lines = old.decode().splitlines(keepends=True)
    if lines and lines[0].startswith("#mode "):
        lines.pop(0)
    result = [f"#mode {fields['mode']}\n"]
    remaining = iter(members)
    for line in lines:
        if not line.strip() or line.startswith("#"):
            result.append(line)
        else:
            item = next(remaining, None)
            if item is not None:
                result.append(item + "\n")
    if result and not result[-1].endswith("\n"):
        result[-1] += "\n"
    result.extend(s + "\n" for s in remaining)
    return "".join(result).encode()


def account_edit(runtime, old, changes):
    require(
        changes and not (changes.keys() - set(SCALARS) - {"folders", "exclude"}),
        "unknown account field",
    )
    lines = []
    for line in old.decode().splitlines(keepends=True):
        parts = line.lstrip().split(maxsplit=1)
        if not parts or parts[0] not in changes:
            lines.append(line)
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    for key, value in changes.items():
        if key in ("folders", "exclude"):
            require(type(value) is list and len(value) <= 1024, "invalid folder list")
            values = value
        else:
            values = [value]
        for item in values:
            single_line(item)
        if key == "email":
            mailbox(runtime, value)
        if key in ("auth", "transport", "cert", "proto"):
            require(re.fullmatch(r"[A-Za-z0-9_-]+", value), "invalid transport field")
        encoded = [
            '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"' for v in values
        ]
        lines.append(key + " " + " ".join(encoded) + "\n")
    return "".join(lines).encode()


def revision(data):
    return hashlib.sha256(data).hexdigest() if data is not None else "absent"


def record_name(directory, kind, identity):
    require(ID.fullmatch(identity) and identity != "README.txt", "invalid record ID")
    if kind == "account" and not identity.endswith(".txt"):
        for name in (identity, identity + ".txt"):
            try:
                os.stat(name, dir_fd=directory, follow_symlinks=False)
                return name
            except FileNotFoundError:
                pass
    return identity


def read_existing(directory, name):
    try:
        return get_bytes(directory, name)
    except FileNotFoundError:
        return None


def replace(directory, name, old, new):
    stage = ".edit-" + secrets.token_hex(16)
    fd = os.open(stage, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=directory)
    try:
        with os.fdopen(fd, "wb") as dest:
            dest.write(new)
            dest.flush()
            os.fsync(dest.fileno())
        if read_existing(directory, name) != old:
            raise Conflict()
        if old is None:
            try:
                os.link(
                    stage,
                    name,
                    src_dir_fd=directory,
                    dst_dir_fd=directory,
                    follow_symlinks=False,
                )
            except FileExistsError as exc:
                raise Conflict() from exc
        else:
            os.replace(stage, name, src_dir_fd=directory, dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        try:
            os.unlink(stage, dir_fd=directory)
        except FileNotFoundError:
            pass


def read_input():
    data = sys.stdin.buffer.read(LIMIT + 1)
    require(len(data) <= LIMIT, "input exceeds bound")
    value = json.loads(data)
    require(type(value) is dict, "expected object")
    return value


def main():
    p = argparse.ArgumentParser()
    p.add_argument("kind", choices=("account", "group"))
    p.add_argument("runtime", type=Path)
    p.add_argument("directory", type=Path)
    p.add_argument("action", choices=("list", "get", "put", "delete", "credential"))
    p.add_argument("id", nargs="?")
    p.add_argument("--expect")
    p.add_argument("--service", choices=("imap", "smtp"))
    a = p.parse_args()
    write = a.action not in ("get", "list")
    require(
        (a.action == "list") == (a.id is None) and write == bool(a.expect),
        "missing ID or revision",
    )
    require(a.action != "delete" or a.kind == "group", "unsupported action")
    require(
        (a.action == "credential") == bool(a.service)
        and (not a.service or a.kind == "account"),
        "invalid service",
    )
    if a.kind == "group" and a.action == "put" and a.expect == "absent":
        a.directory.mkdir(mode=0o700, exist_ok=True)
    try:
        directory = os.open(a.directory, os.O_DIRECTORY | os.O_NOFOLLOW)
    except FileNotFoundError:
        if a.kind == "group" and a.action == "list":
            print(json.dumps({"version": 1, "ids": []}))
            return 0
        raise
    try:
        fcntl.flock(directory, fcntl.LOCK_EX if write else fcntl.LOCK_SH)
        if a.action == "list":
            with os.scandir(directory) as entries:
                ids = sorted(
                    e.name
                    for e in entries
                    if ID.fullmatch(e.name)
                    and e.name != "README.txt"
                    and e.is_file(follow_symlinks=False)
                )
            print(json.dumps({"version": 1, "ids": ids}))
            return 0
        name = record_name(directory, a.kind, a.id)
        data = read_existing(directory, name)
        if write and revision(data) != a.expect:
            raise Conflict()
        if data is None and a.action != "put":
            raise FileNotFoundError()
        if a.action == "credential":
            secret = sys.stdin.buffer.read(4097)
            require(
                0 < len(secret) <= 4096 and not any(c in secret for c in (0, 10, 13)),
                "invalid credential",
            )
            result = subprocess.run(
                [
                    "zsh",
                    "-fc",
                    """
                source "$1/zlibs/bootstrap" source || exit 3
                account="$2"; read_account "$account" || exit 3
                host="${(P)3}"
                [[ "$OS" == GNU ]] && check_keyring && [[ "$PASS:$SECRET_TOOL:$GNOMEKEY" != 0:0:0 ]] || exit 7
                password=$(cat) || exit 7
                store_password || exit 7
            """,
                    "store-credential",
                    a.runtime,
                    name,
                    a.service,
                ],
                input=secret,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if result.returncode:
                print("keyring unavailable or credential write failed", file=sys.stderr)
                return 7
            print(json.dumps({"version": 1, "revision": a.expect, "stored": True}))
            return 0
        if a.action == "delete":
            os.unlink(name, dir_fd=directory)
            os.fsync(directory)
            print(json.dumps({"version": 1, "revision": "absent"}))
            return 0
        if a.action == "put":
            changes = read_input()
            edited = (
                account_edit(a.runtime, data or b"", changes)
                if a.kind == "account"
                else group_edit(a.runtime, data or b"", changes)
            )
            require(len(edited) <= LIMIT, "record exceeds bound")
        else:
            edited = data
        fields = (
            account_fields(a.runtime, a.id, edited)
            if a.kind == "account"
            else group_fields(edited)
        )
        if a.action == "put":
            replace(directory, name, data, edited)
        print(
            json.dumps(
                {
                    "version": 1,
                    "id": a.id,
                    "revision": revision(edited),
                    "fields": fields,
                }
            )
        )
    finally:
        os.close(directory)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Conflict:
        print("stale record revision", file=sys.stderr)
        sys.exit(6)
    except (OSError, ValueError, TypeError, KeyError, RecursionError) as exc:
        print("record unavailable or invalid", file=sys.stderr)
        sys.exit(4)
