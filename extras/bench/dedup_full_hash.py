"""Single-pass full-hash reference for the controlled, disposable dedup corpus."""
import argparse
from contextlib import ExitStack, contextmanager
import fcntl
import hashlib
import os
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile

from dedup_corpus import FOLDERS, SUBDIRS, quote, read_meta


BLOCK = 64 * 1024


def snapshot(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mode, info.st_mtime_ns, info.st_ctime_ns)


def checked(info, expected):
    if not stat.S_ISREG(info.st_mode) or snapshot(info) != expected:
        raise RuntimeError("benchmark entry changed")


@contextmanager
def opened(directories, path, expected, check_after=True):
    directory, name = path.rsplit("/", 1)
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directories[directory])
    with os.fdopen(fd, "rb") as stream:
        checked(os.fstat(fd), expected)
        yield stream
        if check_after:
            checked(os.fstat(fd), expected)


def directory_fds(stack, mail, folders, apply):
    directories = {}
    for folder in folders:
        for rel in (folder, *(f"{folder}/{sub}" for sub in SUBDIRS)):
            fd = os.open(mail / rel, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            stack.callback(os.close, fd)
            if apply:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            directories[rel] = fd
    return directories


def compare(dirs, left, left_stat, right, right_stat, counts):
    with opened(dirs, left, left_stat) as first, opened(dirs, right, right_stat) as second:
        counts["compare-opens"] += 2
        while True:
            a, b = first.read(BLOCK), second.read(BLOCK)
            counts["compare-bytes"] += len(a) + len(b)
            if a != b:
                return False
            if not a:
                return True


def keyinfo(data):
    # Python integers preserve unsigned dev/ino and nanosecond timestamps.
    return tuple(map(int, data.split(",")))


def classify(db, dirs, folders, counts):
    db.executescript("""
        CREATE TABLE keepers(hash BLOB, size INTEGER, path TEXT, snapshot TEXT);
        CREATE INDEX by_content ON keepers(hash,size);
        CREATE TABLE pairs(keeper TEXT, keeper_stat TEXT, loser TEXT, loser_stat TEXT);
    """)
    for folder in folders:
        for sub in SUBDIRS:
            directory = f"{folder}/{sub}"
            fd = dirs[directory]
            for name in sorted(os.listdir(fd), key=os.fsencode):
                info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not stat.S_ISREG(info.st_mode):
                    continue
                path = f"{directory}/{name}"
                expected = snapshot(info)
                saved = ",".join(map(str, expected))
                digest = hashlib.sha256()
                with opened(dirs, path, expected) as stream:
                    counts["hash-opens"] += 1
                    for chunk in iter(lambda: stream.read(BLOCK), b""):
                        digest.update(chunk)
                        counts["hash-bytes"] += len(chunk)
                key = digest.digest(), info.st_size
                for winner, old in db.execute("SELECT path,snapshot FROM keepers WHERE hash=? AND size=?", key):
                    if compare(dirs, winner, keyinfo(old), path, expected, counts):
                        db.execute("INSERT INTO pairs VALUES(?,?,?,?)", (winner, old, path, saved))
                        break
                else:
                    db.execute("INSERT INTO keepers VALUES(?,?,?,?)", (*key, path, saved))
    db.commit()


def apply_pair(mail, dirs, keep, ks, lose, ls, counts):
    # The originals must stay stable. Pin both files and check the bindings
    # through the last check/unlink gap, just as the controlled workload requires.
    with ExitStack() as stack:
        for path, expected in ((keep, ks), (lose, ls)):
            stream = stack.enter_context(opened(dirs, path, expected, check_after=False))
            parent, name = path.rsplit("/", 1)
            if not os.path.samestat((mail / parent).lstat(), os.fstat(dirs[parent])):
                raise RuntimeError(f"directory changed: {parent}")
            checked(os.stat(name, dir_fd=dirs[parent], follow_symlinks=False), expected)
        info = os.fstat(stream.fileno())
        os.unlink(name, dir_fd=dirs[parent])
        counts["removed"] += 1
        if info.st_nlink == 1:
            counts["last-link-block-bytes"] += info.st_blocks * 512


def run(root, scratch, reverse, apply):
    root = root.resolve(strict=True)
    meta = read_meta(root)
    if apply and meta["role"] != "apply-clone":
        raise ValueError("apply requires a disposable clone made by dedup_corpus.py clone")
    if root == scratch.resolve() or root in scratch.resolve().parents:
        raise ValueError("use a scratch directory outside the fixture")
    folders = FOLDERS[::-1] if reverse else FOLDERS
    mail = root / "mail"
    counts = dict.fromkeys(("hash-opens", "hash-bytes", "compare-opens", "compare-bytes",
                            "proposed", "removed", "logical-bytes", "last-link-block-bytes"), 0)
    with ExitStack() as stack:
        dirs = directory_fds(stack, mail, folders, apply)
        tmp = stack.enter_context(tempfile.TemporaryDirectory(prefix="full-hash-", dir=scratch))
        db = sqlite3.connect(Path(tmp) / "index.sqlite")
        stack.callback(db.close)
        classify(db, dirs, folders, counts)
        for keep, old, lose, saved in db.execute("SELECT * FROM pairs ORDER BY rowid"):
            ks, ls = keyinfo(old), keyinfo(saved)
            if apply:
                apply_pair(mail, dirs, keep, ks, lose, ls, counts)
            else:
                sys.stdout.buffer.write(b"keep " + quote(mail / keep) + b" remove " + quote(mail / lose) + b"\n")
            counts["proposed"] += 1
            counts["logical-bytes"] += ls[2]
    print("baseline:", *(f"{key}={value}" for key, value in counts.items()), file=sys.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--reverse", action="store_true")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    run(args.root, args.scratch_dir, args.reverse, args.apply)
