"""Reconcile signed, externally replaced Maildir names before notmuch new."""
from contextlib import contextmanager
import ctypes as c
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(os.environ["JAROWORKDIR"]) / "maildir"))
import operations as native


# notmuch new treats an indexed filename as immutable, even with --full-scan.
# Use libnotmuch's own MIME and Message-ID rules, including generated IDs.
lib = c.CDLL("libnotmuch.so.5")
ptr = c.c_void_p
out = c.POINTER(ptr)
b = c.c_char_p


def bind(name, *args, result=c.c_int):
    fn = getattr(lib, "notmuch_" + name)
    fn.argtypes, fn.restype = args, result
    return fn


db_open = bind("database_open_with_config", b, c.c_int, b, b, out, out)
db_destroy = bind("database_destroy", ptr)
begin = bind("database_begin_atomic", ptr)
find = bind("database_find_message_by_filename", ptr, b, out)
index = bind("database_index_file", ptr, b, ptr, out)
remove = bind("database_remove_message", ptr, b)
message_id = bind("message_get_message_id", ptr, result=b)
reindex = bind("message_reindex", ptr, ptr)


def check(code, allowed=(0,)):
    if code not in allowed:
        raise RuntimeError("notmuch status " + str(code))


@contextmanager
def database(*, write=True):
    db = ptr()
    config = os.fsencode(Path(os.environ["JAROMAILDIR"]) / "notmuch-config")
    check(db_open(None, int(write), config, None, c.byref(db), None))
    try:
        yield db
    finally:
        check(db_destroy(db))


def reconcile(root, row):
    with native.folder(root, row["folder"]) as folder:
        bucket = os.open(row["part"], native.DIR, dir_fd=folder)
        try:
            def current():
                return native.occurrence(bucket, row["folder"], row["part"], row["name"])["revision"]
            replacing = current()
            indexed_id = row.get("indexedId")
            if indexed_id is None and replacing == row["revision"]:
                return
            if indexed_id is not None and replacing != row["revision"]:
                return
            path = os.fsencode(Path(os.environ["JAROMAILDIR"]) / row["folder"] / row["part"] / row["name"])
            with database() as db:
                old, actual = ptr(), ptr()
                check(find(db, path, c.byref(old)))
                if not old:
                    return
                previous_id = message_id(old)
                if indexed_id is not None and previous_id != indexed_id.encode():
                    return
                # Closing with an open atomic section discards the probe,
                # including any filename association and indexing terms.
                check(begin(db))
                status = index(db, path, None, c.byref(actual))
                check(status, (0, 5, 6))  # success, file not mail, duplicate ID
                actual_id = message_id(actual) if actual else None
            if current() != replacing or native.revision(os.stat(path, follow_symlinks=False)) != replacing:
                raise RuntimeError("replacement changed during scan")
            with database() as db:
                old = ptr()
                check(find(db, path, c.byref(old)))
                if not old or message_id(old) != previous_id:
                    raise RuntimeError("index changed during scan")
                if actual_id == previous_id:
                    check(reindex(old, None))  # preserves logical message tags
                else:
                    # Remove only this obsolete filename. The ordinary scan
                    # adds its replacement under the frozen tags/ignore policy.
                    check(remove(db, path), (0, 6))
        finally:
            os.close(bucket)


def indexed_ids(rows):
    result = []
    with database(write=False) as db:
        for row in rows:
            path = Path(os.environ["JAROMAILDIR"]) / row["folder"] / row["part"] / row["name"]
            record = ptr()
            check(find(db, os.fsencode(path), c.byref(record)))
            result.append(os.fsdecode(message_id(record)) if record else None)
    return result


def main():
    rows = json.load(sys.stdin)
    if sys.argv[1:] == ["--lookup"]:
        print(json.dumps(indexed_ids(rows)))
        return
    root = os.open(os.environ["JAROMAILDIR"], native.DIR)
    try:
        for row in rows:
            try:
                reconcile(root, row)
            except FileNotFoundError:
                pass  # ordinary scan handles removals and moves
    finally:
        os.close(root)


if __name__ == "__main__":
    main()
