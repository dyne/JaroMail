"""Operate on revision-checked occurrences under one native mail root."""

import argparse
from contextlib import contextmanager, ExitStack
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile

PARTS = {"cur", "new", "tmp"}
PRIVATE = {
    "Accounts",
    "Groups",
    "cache",
    "logs",
    "keyrings",
    ".git",
    ".recovery-claims",
    ".outbox-state",
    ".imap",
    ".mutt",
    ".notmuch",
    ".mblaze",
    ".password-store",
}
DIR = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
FILE = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
STANDARD_FLAGS = frozenset("DFPRST")
MAX_ITEMS = 10000


def require(ok, message):
    if not ok:
        raise ValueError(message)


def component(name):
    return (
        isinstance(name, str)
        and name not in ("", ".", "..")
        and "/" not in name
        and "\0" not in name
    )


def private_tree(name):
    return name in PRIVATE or name.startswith(".jaro-recovered-outbox-")


def folder_parts(name):
    if name == ".":
        return []
    parts = name.split("/")
    require(
        all(component(n) and n not in PARTS for n in parts)
        and not private_tree(parts[0]),
        "invalid folder",
    )
    return parts


@contextmanager
def folder(root, name, *, validate=True):
    with ExitStack() as stack:
        fd = root
        for part in folder_parts(name):
            fd = os.open(part, DIR, dir_fd=fd)
            stack.callback(os.close, fd)
        if validate:
            for part in PARTS:
                os.close(os.open(part, DIR, dir_fd=fd))
        yield fd



def create_folder(root, name, workdir, *, existing_ok=False):
    parts = folder_parts(name)
    require(parts and parts[0] != "outbox" and len(parts) < 64, "invalid folder")
    if not parts:
        raise FileExistsError()
    with ExitStack() as stack:
        parent = root
        for index, part in enumerate(parts):
            try:
                child = os.open(part, DIR, dir_fd=parent)
            except FileNotFoundError:
                break
            parent = child
            stack.callback(os.close, child)
        else:
            if existing_ok:
                with folder(parent, "."):
                    return
            raise FileExistsError()
        # Neither CLI mkdir nor POSIX rename protects an existing empty target.
        # Build with the native command in an undiscoverable Maildir tmp subtree,
        # then publish the first missing component with Linux RENAME_NOREPLACE.
        with tempfile.TemporaryDirectory(prefix=".jaro-folder-", dir=f"/proc/self/fd/{parent}") as stage:
            staging = Path(stage) / "tmp"
            staging.mkdir(mode=0o700)
            target = staging.joinpath(*parts[index:])
            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            make = 'source "$1/zlibs/bootstrap" source || exit 1; ' \
                   'source "$1/zlibs/maildirs" || exit 1; maildirmake "$2"'
            subprocess.run(["zsh", "-fc", make, "jaro-api-mkdir", str(workdir), str(target)], check=True,
                           pass_fds=(parent,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            new = os.open(target, DIR)
            try:
                with folder(new, "."):
                    pass
            finally:
                os.close(new)
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.renameat2(-100, os.fsencode(staging / part), parent, os.fsencode(part), 1) != 0:
                code = ctypes.get_errno()
                if code == errno.EEXIST and existing_ok:
                    with folder(root, name):
                        return
                raise OSError(code, os.strerror(code))
            os.fsync(parent)

def inventory(fd, name=".", depth=0):
    require(depth < 64, "folder depth exceeded")
    try:
        is_maildir = all(
            stat.S_ISDIR(os.stat(part, dir_fd=fd, follow_symlinks=False).st_mode)
            for part in PARTS
        )
    except FileNotFoundError:
        is_maildir = False
    if is_maildir:
        yield name
    with os.scandir(fd) as entries:
        for item in entries:
            child = item.name
            if (
                child in PARTS
                or (name == "." and private_tree(child))
                or not item.is_dir(follow_symlinks=False)
            ):
                continue
            try:
                sub = os.open(child, DIR, dir_fd=fd)
            except (FileNotFoundError, NotADirectoryError):
                continue
            try:
                yield from inventory(
                    sub, child if name == "." else name + "/" + child, depth + 1
                )
            finally:
                os.close(sub)


def revision(info):
    return hashlib.sha256(
        repr(
            (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)
        ).encode()
    ).hexdigest()


def occurrence(fd, box, part, name):
    info = os.stat(name, dir_fd=fd, follow_symlinks=False)
    require(stat.S_ISREG(info.st_mode), "not a regular occurrence")
    return {"folder": box, "part": part, "name": name, "revision": revision(info)}


def messages(root, box):
    with folder(root, box) as fd:
        for part in ("new", "cur"):
            parent = os.open(part, DIR, dir_fd=fd)
            try:
                with os.scandir(parent) as entries:
                    for e in entries:
                        if not e.name.startswith(".") and e.is_file(
                            follow_symlinks=False
                        ):
                            try:
                                yield occurrence(parent, box, part, e.name)
                            except FileNotFoundError:
                                continue
            finally:
                os.close(parent)


def signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def drop_original(src, name, original, tmp):
    # Capture the selected name before unlinking. A racing replacement is
    # restored without overwriting another arrival. If restoration collides,
    # the captured file remains visible to the operator in Maildir tmp.
    claim = ".jaro-refile-" + secrets.token_hex(16)
    os.rename(name, claim, src_dir_fd=src, dst_dir_fd=tmp)
    try:
        require(
            signature(os.stat(claim, dir_fd=tmp, follow_symlinks=False))
            == signature(original),
            "source replaced",
        )
        os.fsync(src)
    except BaseException:
        os.link(claim, name, src_dir_fd=tmp, dst_dir_fd=src, follow_symlinks=False)
        os.fsync(src)
        os.unlink(claim, dir_fd=tmp)
        raise
    os.unlink(claim, dir_fd=tmp)
    os.fsync(tmp)


def transfer(root, item, destination, flags=None, remove=False, *, copy_only=False):
    box, part, name, expected = (
        item[key] for key in ("folder", "part", "name", "revision")
    )
    require(
        part in ("new", "cur") and component(name) and not name.startswith("."),
        "invalid occurrence",
    )
    if flags is not None:
        require(
            isinstance(flags, str) and set(flags) <= STANDARD_FLAGS,
            "invalid Maildir flags",
        )
        require(box == destination, "flags apply in the source folder")
    with ExitStack() as stack:
        source = stack.enter_context(folder(root, box))

        def sub(parent, child):
            fd = os.open(child, DIR, dir_fd=parent)
            stack.callback(os.close, fd)
            return fd

        src = sub(source, part)
        tmp = sub(source, "tmp")
        try:
            fd = os.open(name, FILE | getattr(os, "O_NOATIME", 0), dir_fd=src)
        except PermissionError:
            # A readable shared message can belong to a different user.
            fd = os.open(name, FILE, dir_fd=src)
        handle = stack.enter_context(os.fdopen(fd, "rb"))
        original = os.fstat(handle.fileno())
        require(
            stat.S_ISREG(original.st_mode) and revision(original) == expected,
            "stale occurrence",
        )
        if remove:
            drop_original(src, name, original, tmp)
            return "removed"
        dest = stack.enter_context(folder(root, destination))
        if (
            not copy_only
            and flags is None
            and os.path.samestat(os.fstat(source), os.fstat(dest))
        ):
            return "unchanged"
        target = sub(dest, "cur")
        stage = sub(dest, "tmp")
        if flags is not None:
            basename, _, info = name.partition(":2,")
            current, comma, extension = info.partition(",")
            keywords = set(current) - STANDARD_FLAGS
            output = (
                basename
                + ":2,"
                + "".join(sorted(keywords | set(flags)))
                + comma
                + extension
            )
            if output == name and part == "cur":
                return "unchanged"
        else:
            suffix = ":2," + name.split(":2,", 1)[1] if ":2," in name else ""
            output = (
                name
                if name.startswith("jaro-v1-")
                else "jaro-refile-"
                + (secrets.token_hex(16) if copy_only else expected)
                + suffix
            )
        pending = ".jaro-copy-" + secrets.token_hex(16)
        fd = os.open(pending, os.O_EXCL | os.O_CREAT | os.O_WRONLY, 0o600, dir_fd=stage)
        try:
            with os.fdopen(fd, "wb") as out:
                shutil.copyfileobj(handle, out, 1024 * 1024)
                out.flush()
                # Dovecot reads the received date from mtime. Set the timestamp
                # on the private copy before linking it into the destination.
                os.utime(out.fileno(), ns=(original.st_atime_ns, original.st_mtime_ns))
                require(
                    os.fstat(out.fileno()).st_mtime_ns == original.st_mtime_ns,
                    "destination cannot represent the original received date",
                )
                os.fsync(out.fileno())
            require(
                revision(os.fstat(handle.fileno())) == expected,
                "source changed during copy",
            )
            # A destination collision is an explicit partial outcome; never replace it.
            os.link(
                pending,
                output,
                src_dir_fd=stage,
                dst_dir_fd=target,
                follow_symlinks=False,
            )
            os.fsync(target)
        finally:
            os.unlink(pending, dir_fd=stage)
            os.fsync(stage)
        if copy_only:
            return "copied"
        drop_original(src, name, original, tmp)
        return "moved"


def lookup(root, root_path, path):
    relative = Path(os.fsdecode(path)).relative_to(root_path)
    require(len(relative.parts) >= 2, "invalid indexed path")
    box = str(relative.parent.parent)
    part, name = relative.parts[-2:]
    require(part in ("cur", "new") and component(name), "invalid indexed occurrence")
    with folder(root, box) as fd:
        p = os.open(part, DIR, dir_fd=fd)
        try:
            return occurrence(p, box, part, name)
        finally:
            os.close(p)


def backup(root, path, query, destination):
    require(0 < len(query) <= 4096, "invalid query")
    proc = subprocess.Popen(
        [
            "notmuch",
            "--config=" + str(path / "notmuch-config"),
            "search",
            "--output=files",
            "--format=text0",
            "--",
            query,
        ],
        stdout=subprocess.PIPE,
    )
    try:
        data = proc.stdout.read((8 << 20) + 1)
        require(len(data) <= 8 << 20, "indexed selection exceeds bound")
        require(proc.wait() == 0, "notmuch search failed")
    finally:
        proc.stdout.close()
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    if data:
        hits = data.split(b"\0")
        require(
            hits.pop() == b"" and len(hits) <= MAX_ITEMS, "invalid or oversized result"
        )
        for hit in hits:
            item = lookup(root, path, hit)
            if item["folder"] != destination:
                yield item
        return
    for box in inventory(root):
        if box == "." or "/" in box or box == destination:
            continue
        for item in messages(root, box):
            with folder(root, box) as fd:
                sd = os.open(item["part"], DIR, dir_fd=fd)
                try:
                    handle = os.fdopen(os.open(item["name"], FILE, dir_fd=sd), "rb")
                finally:
                    os.close(sd)
                with handle:
                    rc = subprocess.run(
                        ["grep", "-iq", "--", query], stdin=handle
                    ).returncode
            require(rc in (0, 1), "invalid raw-text backup query")
            if rc == 0:
                yield item


def duplicates(root, boxes):
    require(shutil.which("formail"), "formail unavailable")
    if boxes == ["lastlog"]:
        require(shutil.which("maildirs_lastlog"), "lastlog provider unavailable")
        proc = subprocess.run(
            ["maildirs_lastlog"], stdout=subprocess.PIPE, check=True, timeout=10
        )
        boxes = [os.fsdecode(s) for s in proc.stdout.splitlines()]
    require(
        0 < len(boxes) <= 1024 and len(set(boxes)) == len(boxes),
        "invalid ordered folders",
    )
    with tempfile.TemporaryDirectory(prefix="jaro-formail-") as stage:
        for box in boxes:
            with folder(root, box) as fd:
                for item in messages(root, box):
                    sd = os.open(item["part"], DIR, dir_fd=fd)
                    try:
                        stream = os.fdopen(os.open(item["name"], FILE, dir_fd=sd), "rb")
                    finally:
                        os.close(sd)
                    with stream:
                        rc = subprocess.run(
                            ["formail", "-D", "128000000", stage + "/ids"], stdin=stream
                        ).returncode
                    require(rc in (0, 1), "formail failed")
                    if rc == 0:
                        yield item


def bounded(items):
    rows = []
    for item in items:
        require(len(rows) < MAX_ITEMS, "selection exceeds bound")
        rows.append(item)
    return rows


def empty_source(root, box):
    parts = folder_parts(box)
    require(
        parts and box not in {"outbox", "sent", "known", "incoming"},
        "protected source folder",
    )
    parent_name = "/".join(parts[:-1]) or "."
    with folder(root, parent_name, validate=False) as parent, folder(root, box) as src:
        require(set(os.listdir(src)) == PARTS, "source still contains other entries")
        handles = []
        try:
            for part in sorted(PARTS):
                fd = os.open(part, DIR, dir_fd=src)
                handles.append(fd)
                require(not os.listdir(fd), "source received new mail")
            for part in sorted(PARTS):
                os.rmdir(part, dir_fd=src)
            require(
                os.path.samestat(
                    os.stat(parts[-1], dir_fd=parent, follow_symlinks=False),
                    os.fstat(src),
                ),
                "source folder changed",
            )
            os.rmdir(parts[-1], dir_fd=parent)
            os.fsync(parent)
        except BaseException:
            for part in PARTS:
                try:
                    os.mkdir(part, 0o700, dir_fd=src)
                except FileExistsError:
                    pass
            raise
        finally:
            for fd in handles:
                os.close(fd)


def main():
    p = argparse.ArgumentParser(prog="jaro maildir")
    p.add_argument("root", type=Path)
    commands = p.add_subparsers(dest="op", required=True)
    refile = commands.add_parser("refile")
    refile.add_argument("source", type=Path)
    refile.add_argument("destination")
    refile.add_argument("--copy", action="store_true")
    commands.add_parser("folders")
    commands.add_parser("messages").add_argument("folder")
    commands.add_parser("legacy-preview").add_argument("folders", nargs="+")
    backup_parser = commands.add_parser("backup-preview")
    backup_parser.add_argument("destination")
    backup_parser.add_argument("query")
    apply = commands.add_parser("apply")
    apply.add_argument("destination")
    apply.add_argument("--flags")
    merge = commands.add_parser("merge")
    merge.add_argument("folder")
    merge.add_argument("destination")
    merge.add_argument("--apply", action="store_true")
    merge.add_argument("--remove-empty", action="store_true")
    commands.add_parser("legacy-remove").add_argument(
        "--confirm-message-id", action="store_true", required=True
    )
    a = p.parse_args()
    fd = os.open(a.root, DIR)
    try:
        if a.op == "refile":
            item = lookup(fd, a.root, a.source)
            transfer(fd, item, a.destination, copy_only=a.copy)
            return 0
        if a.op in ("folders", "messages"):
            items = inventory(fd) if a.op == "folders" else messages(fd, a.folder)
            for item in items:
                print(json.dumps(item), flush=True)
            return 0
        if a.op == "legacy-preview":
            items = bounded(duplicates(fd, a.folders))
        elif a.op == "backup-preview":
            with folder(fd, a.destination):
                pass
            items = bounded(backup(fd, a.root, a.query, a.destination))
        elif a.op == "merge":
            with folder(fd, a.destination):
                pass
            require(
                a.folder != a.destination
                and not a.destination.startswith(a.folder + "/")
                and a.folder != ".",
                "overlapping folders",
            )
            require(not a.remove_empty or a.apply, "removal requires apply")
            items = bounded(messages(fd, a.folder))
        else:
            raw = sys.stdin.buffer.read((8 << 20) + 1)
            require(len(raw) <= 8 << 20, "oversized selection")
            request = json.loads(raw)
            if a.op == "legacy-remove":
                require(
                    isinstance(request, dict) and set(request) == {"folders", "items"},
                    "expected preview selection",
                )
                current = {
                    tuple(row[k] for k in ("folder", "part", "name", "revision"))
                    for row in bounded(duplicates(fd, request["folders"]))
                }
                items = request["items"]
                require(
                    all(
                        tuple(row[k] for k in ("folder", "part", "name", "revision"))
                        in current
                        for row in items
                    ),
                    "duplicate selection changed",
                )
            else:
                items = request
            require(
                isinstance(items, list) and len(items) <= MAX_ITEMS, "invalid selection"
            )
        if a.op.endswith("preview") or (a.op == "merge" and not a.apply):
            print(json.dumps({"version": 1, "items": items}))
            return 0
        results = []
        failed = False
        for index, item in enumerate(items):
            try:
                status = transfer(
                    fd,
                    item,
                    getattr(a, "destination", None),
                    getattr(a, "flags", None),
                    a.op == "legacy-remove",
                )
            except (ValueError, KeyError, OSError, TypeError):
                status = "error"
                failed = True
            results.append({"index": index, "status": status})
        removed = False
        if a.op == "merge" and a.remove_empty and not failed:
            try:
                empty_source(fd, a.folder)
                removed = True
            except (OSError, ValueError):
                failed = True
        print(json.dumps({"version": 1, "results": results, "removed_source": removed}))
        return 7 if failed else 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
    except (
        ValueError,
        OSError,
        subprocess.SubprocessError,
        TypeError,
        KeyError,
        RecursionError,
    ):
        print("maildir: request or selected resource unavailable", file=sys.stderr)
        sys.exit(3)
