"""Confine the installed JaroMail snapshot CLI to the host's approved directories."""
from contextlib import ExitStack
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

DIR = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
FILE = os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW


class Problem(Exception):
    pass


def name(value):
    if (not isinstance(value, str) or not value.strip() or value.startswith(".") or "/" in value
            or len(os.fsencode(value)) > 255 or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise Problem("snapshot_name")
    return value


def overlaps(left, right):
    return left == right or left in right.parents or right in left.parents


def descriptor(stack, path, flags, parent=None):
    fd = os.open(path, flags, dir_fd=parent)
    stack.callback(os.close, fd)
    return fd


def trusted(info):
    return info.st_uid in (0, os.geteuid()) and not info.st_mode & 0o022


def approved(stack, path, protected):
    if not isinstance(path, str) or not path.startswith("/") or path.startswith("//") or os.path.normpath(path) != path:
        raise Problem("snapshot_root")
    location = Path(path)
    if location == Path("/") or any(overlaps(location, p) for p in protected):
        raise Problem("snapshot_root")
    home = Path(os.environ["HOME"])
    if location == home or location in home.parents:
        raise Problem("snapshot_root")
    try:
        parent = descriptor(stack, "/", DIR)
        current = Path("/")
        for part in location.parts[1:]:
            parent = descriptor(stack, part, DIR, parent)
            current /= part
            info = os.fstat(parent)
            # Read-only system mounts and the system's sticky /tmp remain
            # usable when a user namespace maps the root UID to nobody.
            readonly = os.fstatvfs(parent).f_flag & os.ST_RDONLY
            system_tmp = current == Path("/tmp") and info.st_uid == os.stat("/").st_uid and info.st_mode & stat.S_ISVTX
            if not trusted(info) and not readonly and not system_tmp:
                raise Problem("snapshot_root")
        if not trusted(os.fstat(parent)):
            raise Problem("snapshot_root")
        return parent
    except OSError as error:
        raise Problem("snapshot_root") from error


def identity(fd):
    s = os.fstat(fd)
    return f"{s.st_dev}.{s.st_ino}"


def exclusive(fd):
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as e:
        raise Problem("snapshot_quiescence") from e


def marker(parent, entry):
    with ExitStack() as stack:
        try:
            artifact = descriptor(stack, entry, DIR, parent)
            if not trusted(os.fstat(artifact)):
                raise Problem("snapshot_invalid")
            complete = descriptor(stack, "complete", FILE, artifact)
            s = os.fstat(complete)
            if not stat.S_ISREG(s.st_mode) or s.st_size != 65:
                raise Problem("snapshot_invalid")
            text = os.read(complete, 66)
            if not re.fullmatch(b"[a-f0-9]{64}\n", text):
                raise Problem("snapshot_invalid")
            return text[:64].decode("ascii")
        except OSError as error:
            raise Problem("snapshot_invalid") from error


def catalog(config, protected):
    locations = {}
    for kind in ("storage", "recovery"):
        path = config.get(kind)
        row = {"path": path, "identity": None}
        locations[kind] = row
        with ExitStack() as stack:
            try:
                if path:
                    row["identity"] = identity(approved(stack, path, protected_for(kind, config, protected)))
            except Problem:
                pass
    return {**locations, "source": os.environ["JAROMAILDIR"]}


def protected_for(kind, config, protected):
    other = config.get("recovery" if kind == "storage" else "storage")
    return [*protected, Path(other)] if other and other != config.get(kind) else protected


def destination(parent, entry, op):
    try:
        info = os.stat(entry, dir_fd=parent, follow_symlinks=False)
    except FileNotFoundError:
        return
    if op == "create":
        raise Problem("snapshot_exists")
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid():
        raise Problem("snapshot_fresh_root")
    with ExitStack() as stack:
        dest = descriptor(stack, entry, DIR, parent)
        with os.scandir(dest) as listing:
            if next(listing, None) is not None:
                raise Problem("snapshot_fresh_root")


def snapshot(request, config, protected):
    op = request["op"]
    if op == "catalog":
        return catalog(config, protected)
    if op not in ("create", "verify", "restore"):
        raise Problem("invalid_request")
    entry = name(request.get("name"))
    dest = name(request.get("destination")) if op == "restore" else entry
    if op != "verify" and request.get("paused") is not True:
        raise Problem("snapshot_quiescence")
    with ExitStack() as stack:
        storage = approved(stack, config.get("storage"), protected_for("storage", config, protected))
        if request.get("storage") != identity(storage):
            raise Problem("snapshot_changed")
        exclusive(storage)
        locked = [storage]
        parent = storage
        if op == "restore":
            parent = approved(stack, config.get("recovery"), protected_for("recovery", config, protected))
            if request.get("recovery") != identity(parent):
                raise Problem("snapshot_changed")
            if not os.path.samestat(os.fstat(storage), os.fstat(parent)):
                exclusive(parent)
                locked.append(parent)
        source = Path(os.environ["JAROMAILDIR"])
        if op == "create":
            live = descriptor(stack, source, DIR)
            exclusive(live)
            locked.append(live)
            if any(os.path.lexists(source / p) for p in ("outbox.lock", "cache/notmuch-index.lock")):
                raise Problem("snapshot_quiescence")
        if op != "verify":
            destination(parent, dest, op)
        before = marker(storage, entry) if op != "create" else None
        if op == "restore" and request.get("manifest") != before:
            raise Problem("snapshot_changed")
        # Pin the destination parent as cwd. Native directory traversal rejects
        # symlink components, stages the restore and rechecks it before publish.
        os.fchdir(parent)
        args = [str(Path(config["storage"]) / entry), dest] if op == "restore" else [entry]
        result = subprocess.run([sys.argv[1], "snapshot", op, *args], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, pass_fds=tuple(locked))
        if result.returncode:
            errors = {"create": "snapshot_capture", "verify": "snapshot_invalid", "restore": "snapshot_restore"}
            raise Problem(errors[op])
        summary = re.fullmatch(rb"(?:snapshot: (\d+) outbox message\(s\) held under "
                               rb"(\.jaro-recovered-outbox-\d+/outbox); inspect before explicitly requeueing\n)?"
                               + b"snapshot: " + op.encode() + rb" verified (\d+) message\(s\)\n", result.stdout)
        if not summary:
            raise Problem("invalid_response")
        after = marker(storage, entry)
        if before and before != after:
            raise Problem("snapshot_changed")
        return {"op": op, "name": entry, "destination": dest if op == "restore" else None, "manifest": after,
                "count": int(summary[3]), "held": int(summary[1] or 0),
                "quarantine": summary[2].decode() if summary[2] else None}


if __name__ == "__main__":
    try:
        req, settings = json.load(sys.stdin)
        forbidden = [Path(os.environ["JAROMAILDIR"]), Path(os.environ["JAROWORKDIR"]),
                     Path(sys.argv[1]).parent, Path(settings["auth"])]
        print(json.dumps(snapshot(req, settings, forbidden)))
    except Problem as error:
        print(json.dumps({"error": str(error)}))
    except (OSError, ValueError, TypeError, KeyError):
        print(json.dumps({"error": "snapshot_root"}))
