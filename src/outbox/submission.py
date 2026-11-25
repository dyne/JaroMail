"""Native enqueue-or-lookup and immutable SMTP envelope metadata."""

import argparse
import base64
import fcntl
import hashlib
import json
import os
from contextlib import ExitStack
from pathlib import Path
import re
import secrets
import stat
import subprocess
import sys

REQUEST_LIMIT = 64 * 1024 * 1024
META_LIMIT = 1024 * 1024
ENTRY = re.compile(r"jaro-v1-([0-9a-f]{64})-([0-9]{1,3})-([0-9a-f]{64})")
ACCOUNT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._+@-]{0,191}")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(data):
    return json.dumps(
        data, separators=(",", ":"), sort_keys=True, ensure_ascii=True
    ).encode()


def directory(stack, parent, name, *, private=False, create=False):
    if create:
        try:
            os.mkdir(name, 0o700, dir_fd=parent)
        except FileExistsError:
            pass
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_DIRECTORY, dir_fd=parent)
    stack.callback(os.close, fd)
    info = os.fstat(fd)
    if private:
        require(
            info.st_uid == os.geteuid() and not (info.st_mode & 0o077),
            "unsafe metadata directory",
        )
    if create:
        os.fsync(parent)
    return fd


def read(parent, name, limit, *, private=False):
    fd = os.open(name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=parent)
    with os.fdopen(fd, "rb") as handle:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode), "expected regular file")
        if private:
            require(
                info.st_uid == os.geteuid() and not (info.st_mode & 0o077),
                "unsafe receipt",
            )
        data = handle.read(limit + 1)
        require(len(data) <= limit, "file exceeds bound")
        return data


def publish(stage, destination, name, data):
    temporary = ".enqueue-" + secrets.token_hex(16)
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=stage)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.link(
            temporary,
            name,
            src_dir_fd=stage,
            dst_dir_fd=destination,
            follow_symlinks=False,
        )
        os.fsync(destination)
    finally:
        os.unlink(temporary, dir_fd=stage)
        os.fsync(stage)


def request(account, compiler):
    raw = sys.stdin.buffer.read(REQUEST_LIMIT + 1)
    require(len(raw) <= REQUEST_LIMIT, "oversized submission")
    obj = json.loads(raw)
    require(
        type(obj) is dict
        and set(obj) == {"version", "key", "envelopes"}
        and type(obj["version"]) is int
        and obj["version"] == 1,
        "invalid submission version or fields",
    )
    require(
        type(obj["key"]) is str and re.fullmatch(r"[A-Za-z0-9._:-]{1,160}", obj["key"]),
        "invalid key",
    )
    items = obj["envelopes"]
    require(type(items) is list and 1 <= len(items) <= 128, "expected 1..128 envelopes")
    metadata, bodies, verified = [], [], set()
    for item in items:
        require(
            type(item) is dict and set(item) == {"mime", "recipients"},
            "invalid envelope",
        )
        recipients = item["recipients"]
        require(
            type(recipients) is list and 1 <= len(recipients) <= 256,
            "invalid recipients",
        )
        for address in recipients:
            require(
                type(address) is str
                and 0 < len(address) <= 320
                and not any(ord(c) < 33 or ord(c) == 127 for c in address)
                and not address.startswith("-"),
                "invalid recipient",
            )
            if address not in verified:
                require(
                    len(verified) < 1024
                    and subprocess.run(
                        [compiler, "--check-address", address],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    ).returncode
                    == 0,
                    "invalid mailbox",
                )
                verified.add(address)
        require(type(item["mime"]) is str, "invalid MIME encoding")
        data = base64.b64decode(item["mime"], validate=True)
        require(
            data and (b"\n\n" in data or b"\r\n\r\n" in data), "invalid MIME message"
        )
        metadata.append({"sha256": digest(data), "recipients": recipients})
        bodies.append(data)
    manifest = {"version": 1, "account": account, "envelopes": metadata}
    require(len(encode(manifest)) <= META_LIMIT, "oversized envelope metadata")
    return digest(obj["key"].encode()), manifest, bodies


def basename(key, manifest, index):
    return f"jaro-v1-{key}-{index}-{digest(encode(manifest))}"


def accounts_directory(root):
    return Path(os.environ.get("JAROACCOUNT") or root / "Accounts").resolve(strict=True)


def resolve_account(root, account):
    accounts = accounts_directory(root)
    path = accounts / account
    if not path.exists():
        path = accounts / (account + ".txt")
    path = path.resolve(strict=True)
    require(
        path.parent == accounts and ACCOUNT.fullmatch(path.name) and path.is_file(),
        "unknown account",
    )
    module = Path(__file__).resolve().parents[1] / "zlibs/accounts"
    result = subprocess.run(
        [
            "zsh",
            "-fc",
            'error() { :; }; source "$1" && account="$2" && _account_load_file "$3"',
            "validate-account",
            module,
            account,
            path,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    require(result.returncode == 0, "invalid account configuration")
    info = accounts.stat()
    return {"file": path.name, "directory": [info.st_dev, info.st_ino]}


def live_names(folders, key):
    names = {}
    for folder in ("new", "cur"):
        with os.scandir(folders[folder]) as entries:
            for entry in entries:
                if entry.name.startswith("jaro-v1-" + key + "-"):
                    name = entry.name.split(":2,", 1)[0]
                    require(name not in names, "ambiguous queue occurrence")
                    names[name] = (folder, entry.name)
    return names


def submitted(args, stack, root, journal, submissions):
    key, manifest, bodies = request(
        args.account, args.helper.with_name("jaro-filter-compile")
    )
    # The lock is kernel-owned and survives no process. A disconnected
    # producer can retry with the original bytes even after partial publication.
    fcntl.flock(submissions, fcntl.LOCK_EX)
    receipt = key + ".json"
    try:
        existing = read(submissions, receipt, META_LIMIT, private=True)
    except FileNotFoundError:
        manifest["route"] = resolve_account(args.root, args.account)
        require(len(encode(manifest)) <= META_LIMIT, "oversized receipt")
        publish(submissions, submissions, receipt, encode(manifest))
    else:
        manifest["route"] = json.loads(existing)["route"]
        require(
            existing == encode(manifest),
            "submission key already identifies different input",
        )
        os.fsync(submissions)
    outbox = directory(stack, root, "outbox")
    folders = {part: directory(stack, outbox, part) for part in ("new", "cur", "tmp")}
    occurrences = live_names(folders, key)
    result = []
    for index, data in enumerate(bodies):
        name = basename(key, manifest, index)
        identity = digest(name.encode())
        marker = identity + ".published"
        try:
            require(
                read(submissions, marker, 0, private=True) == b"",
                "invalid publication marker",
            )
            published = True
        except FileNotFoundError:
            published = False
        selected = occurrences.get(name)
        claimed = False
        if not selected and not published:
            for parent, path in ((journal, identity), (outbox, ".recovery-claims")):
                try:
                    if parent == outbox:
                        claims = directory(stack, parent, path, private=True)
                        os.stat(identity, dir_fd=claims, follow_symlinks=False)
                    else:
                        os.stat(path, dir_fd=parent, follow_symlinks=False)
                    claimed = True
                    break
                except FileNotFoundError:
                    continue
        if not selected and not published and not claimed:
            # A worker can only start transport after binding() verifies the
            # durable marker. No marker + no occurrence is safe to publish.
            publish(folders["tmp"], folders["new"], name, data)
            selected = ("new", name)
        if selected:
            folder, filename = selected
            require(
                digest(read(folders[folder], filename, REQUEST_LIMIT))
                == manifest["envelopes"][index]["sha256"],
                "queued message changed",
            )
            os.fsync(folders[folder])
            if not published:
                publish(submissions, submissions, marker, b"")
            report = subprocess.run(
                [args.helper, "inspect", args.root, folder + "/" + filename],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            fields = report.stdout.split()
            state = (
                fields[1].decode("ascii")
                if report.returncode == 0 and len(fields) == 3
                else "held"
            )
        else:
            report = subprocess.run(
                [args.helper, "completed", args.root, "new/" + name],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            state = (
                "complete"
                if report.returncode == 0
                and report.stdout.strip() == digest(data).encode()
                else "held"
            )
        result.append({"id": identity, "state": state})
    print(json.dumps({"version": 1, "envelopes": result}))


def binding(args, journal, submissions):
    name = args.entry.split(":2,", 1)[0]
    match = ENTRY.fullmatch(name)
    require(match is not None, "invalid submission occurrence")
    key, index, revision = match.groups()
    identity = digest(name.encode())
    fcntl.flock(submissions, fcntl.LOCK_SH)
    raw = read(submissions, key + ".json", META_LIMIT, private=True)
    require(
        digest(raw) == revision
        and read(submissions, identity + ".published", 0, private=True) == b"",
        "unverified submission metadata",
    )
    manifest = json.loads(raw)
    index = int(index)
    require(
        index < len(manifest["envelopes"]) and basename(key, manifest, index) == name,
        "invalid envelope ordinal",
    )
    info = manifest["envelopes"][index]
    with ExitStack() as stack:
        record = directory(stack, journal, identity, private=True)
        require(
            read(record, "identity", 80, private=True)
            == ("JO1 " + info["sha256"] + "\n").encode(),
            "changed envelope",
        )
    route = manifest["route"]
    accounts = accounts_directory(args.root)
    directory_info = accounts.stat()
    require(ACCOUNT.fullmatch(route["file"]), "invalid bound account")
    selected = (accounts / route["file"]).lstat()
    require(
        route["directory"] == [directory_info.st_dev, directory_info.st_ino]
        and stat.S_ISREG(selected.st_mode),
        "bound account is unavailable",
    )
    sys.stdout.buffer.write(
        b"\0".join(
            s.encode()
            for s in [
                route["file"],
                str(selected.st_dev),
                str(selected.st_ino),
                *info["recipients"],
            ]
        )
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=["submit", "binding"])
    parser.add_argument("root", type=Path)
    parser.add_argument("helper", type=Path)
    parser.add_argument("value")
    args = parser.parse_args()
    create = args.operation == "submit"
    if create:
        args.account = args.value
        require(ACCOUNT.fullmatch(args.account), "invalid account")
    else:
        args.entry = args.value
    with ExitStack() as stack:
        root = directory(stack, None, args.root)
        journal = directory(stack, root, ".outbox-state", private=True, create=create)
        submissions = directory(
            stack, journal, "submissions", private=True, create=create
        )
        if create:
            submitted(args, stack, root, journal, submissions)
        else:
            binding(args, journal, submissions)


if __name__ == "__main__":
    try:
        main()
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        UnicodeError,
        RecursionError,
    ) as exc:
        # Validation diagnostics never include user-supplied MIME or keys.
        print(
            "outbox: submission rejected (" + type(exc).__name__ + ")", file=sys.stderr
        )
        sys.exit(1)
