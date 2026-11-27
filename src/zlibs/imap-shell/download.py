"""Restartable read-only IMAP export. The frozen selection precedes the UID journal."""

import contextlib
import errno
import fcntl
import hashlib
import imaplib
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
import tempfile

import headers
import export_archive
from export_archive import DownloadError, PublishedError, open_private
from folders import read_quoted

CHUNK = 256 * 1024
MAX_UID = 2**32 - 1
VERSION = 1


def options(words):
    usage = "use download FOLDER --output PATH.tar.gz [--recursive] [--resume]"
    if not words or not words[0] or words[0].startswith("--"):
        raise DownloadError(usage)
    source, output, flags = words[0], None, set()
    args = iter(words[1:])
    for arg in args:
        if arg == "--output" and output is None:
            output = next(args, "")
        elif arg in ("--recursive", "--resume") and arg not in flags:
            flags.add(arg)
        else:
            raise DownloadError(usage)
    if not output or not output.endswith(".tar.gz") or output.endswith("/.tar.gz"):
        raise DownloadError(usage)
    path = Path(output)
    if ".." in path.parts:
        raise DownloadError("choose an output path without parent traversal")
    return source, path.absolute(), "--recursive" in flags, "--resume" in flags


def examine(tree, raw, expected=None):
    client = tree.client
    with tree.deadline(tree.seconds):
        count = headers.examine(client, raw)
        validity = headers.response_number(client, "UIDVALIDITY")
        if validity is None or not 1 <= validity <= MAX_UID or expected is not None and validity != expected:
            raise DownloadError("UIDVALIDITY missing or changed; start a fresh inventory under a new output name")
        client.untagged_responses.pop("EXPUNGE", None)
        if expected is not None:
            return count, validity, None
        next_uid = headers.response_number(client, "UIDNEXT")
        if next_uid is None:
            tail = headers.sequence_uids(client, count, count) if count else []
            if count and (len(tail) != 1 or tail[0][0] != count):
                raise DownloadError("could not capture the last UID; retry download")
            upper = tail[0][1] if tail else 0
        else:
            upper = next_uid - 1
        if not 0 <= upper <= MAX_UID:
            raise DownloadError("invalid mailbox upper UID bound")
        return count, validity, upper


def freeze(tree, root, recursive):
    folders = []
    pending = [root]
    while pending:
        node = pending.pop()
        validity = upper = None
        if node.selectable:
            _, validity, upper = examine(tree, node.raw)
        folders.append(dict(raw=node.raw.hex(), delimiter=node.delimiter.hex() if node.delimiter else None,
                            path=node.path, validity=validity, upper=upper))
        if recursive:
            pending.extend(reversed(tree.children(node, refresh=True).values()))
    return folders


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def private_file(path, optional=False):
    try:
        attributes = path.lstat()
    except FileNotFoundError:
        if optional:
            return False
        raise
    export_archive.checked_file(attributes)
    return True


def create_stage(stage, selection):
    # Publish the immutable selection and initial database together. A failed
    # setup cannot be mistaken for an attempt with a complete folder list.
    with tempfile.TemporaryDirectory(prefix=".jaro-download-", dir=stage.parent) as tmp:
        directory = Path(tmp)
        encoded = json.dumps(selection, separators=(",", ":")).encode("ascii")
        with (directory / "selection.json").open("xb") as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        database = directory / "inventory.sqlite"
        os.close(os.open(database, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600))
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.executescript("""
                CREATE TABLE selection (digest TEXT NOT NULL);
                CREATE TABLE inventories (folder INTEGER PRIMARY KEY, message_count INTEGER NOT NULL);
                CREATE TABLE messages (
                    folder INTEGER NOT NULL, uid INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'pending',
                    size INTEGER, sha256 TEXT, internaldate TEXT, flags TEXT,
                    PRIMARY KEY (folder, uid)
                ) WITHOUT ROWID;
            """)
            with db:
                db.execute("INSERT INTO selection VALUES (?)", (hashlib.sha256(encoded).hexdigest(),))
        sync_dir(directory)
        # Serialize publication between cooperating fresh downloads, including
        # the no-replace check. Never keep a parent-wide lock over network I/O.
        parent = os.open(stage.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(parent, fcntl.LOCK_EX)
            if os.path.lexists(stage):
                raise DownloadError("staging already exists; repeat with --resume")
            os.rename(directory, stage)
            os.fsync(parent)
        finally:
            os.close(parent)


@contextlib.contextmanager
def stage_lock(stage):
    descriptor = os.open(stage, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        attrs = os.fstat(descriptor)
        if attrs.st_uid != os.geteuid() or attrs.st_mode & 0o077:
            raise DownloadError("unsafe staging directory ownership or permissions")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise DownloadError("another writer is using this staging directory") from None
        if not os.path.samestat(attrs, stage.lstat()):
            raise DownloadError("staging directory changed")
        yield descriptor
    finally:
        os.close(descriptor)


def inventory(tree, db, number, folder):
    raw, upper = bytes.fromhex(folder["raw"]), folder["upper"]
    for attempt in range(3):
        count, _, _ = examine(tree, raw, folder["validity"])
        try:
            # Roll back all pages if interrupted. The next attempt enumerates
            # the current positions again under the original, immutable bound.
            with db:
                previous = captured = 0
                for start in range(1, count + 1, headers.BATCH_SIZE):
                    end = min(count, start + headers.BATCH_SIZE - 1)
                    with tree.deadline(tree.seconds):
                        rows = sorted(headers.sequence_uids(tree.client, start, end))
                    if [pos for pos, _ in rows] != list(range(start, end + 1)):
                        raise headers.MailboxChanged()
                    for _, uid in rows:
                        if not previous < uid <= MAX_UID:
                            raise DownloadError("invalid UID inventory")
                        previous = uid
                        if uid <= upper:
                            db.execute("INSERT INTO messages(folder, uid) VALUES (?, ?)", (number, uid))
                            captured += 1
                    if previous >= upper:
                        break
                db.execute("INSERT INTO inventories VALUES (?, ?)", (number, captured))
            return
        except headers.MailboxChanged:
            if attempt == 2:
                raise DownloadError("mailbox changed repeatedly during inventory; repeat with --resume") from None


# Tokenize only the attributes requested by the export. A flag named UID, or
# UID-looking text in a raw message, must never satisfy the UID check.
ATTRIBUTE = re.compile(rb'(UID|RFC822.SIZE) ([0-9]+)|INTERNALDATE "([^"\r\n]+)"|FLAGS \(([^()]*)\)|BODY\[\]<([0-9]+)> ', re.I)


def body_row(parts, uid, offset, span):
    attributes = {}
    body = None
    pos = re.match(rb"^[1-9][0-9]* \(", parts)
    if not pos or not parts.endswith(b")"):
        raise DownloadError("malformed body FETCH response")
    index = pos.end()
    while index < len(parts) - 1:
        match = ATTRIBUTE.match(parts, index)
        if not match:
            raise DownloadError("unexpected body FETCH attribute")
        index = match.end()
        if match[1]:
            key, value = match[1].upper(), int(match[2])
        elif match[3] is not None:
            key, value = b"DATE", match[3].decode("ascii")
        elif match[4] is not None:
            key, value = b"FLAGS", match[4].decode("ascii").split()
        else:
            key, value = b"BODY", int(match[5])
            if parts[index:index + 1] == b'"':
                body, index = read_quoted(parts, index)
            else:
                marker = re.match(rb"\{([0-9]+)\}", parts[index:])
                if not marker:
                    raise DownloadError("invalid partial body literal")
                body = int(marker[1])
                index += marker.end()
        if key in attributes:
            raise DownloadError("duplicate body FETCH attribute")
        attributes[key] = value
        if index < len(parts) - 1:
            if parts[index:index + 1] != b" ":
                raise DownloadError("invalid FETCH attribute separator")
            index += 1
    if (index != len(parts) - 1 or set(attributes) != {b"UID", b"RFC822.SIZE", b"DATE", b"FLAGS", b"BODY"}
            or attributes[b"UID"] != uid or attributes[b"BODY"] != offset):
        raise DownloadError("body FETCH identity or offset mismatch")
    size = attributes[b"RFC822.SIZE"]
    nbytes = body if isinstance(body, int) else len(body)
    if size >= 2**63 or offset > size or nbytes != min(span, size - offset):
        raise DownloadError("body FETCH length disagrees with RFC822.SIZE")
    return size, attributes[b"DATE"], attributes[b"FLAGS"], body


def fetch_chunk(tree, uid, offset):
    client = tree.client
    reader = client.read
    literal_read = False

    def bounded_read(size):
        nonlocal literal_read
        if literal_read or size > CHUNK:
            raise headers.LiteralLimit("body literal exceeds request")
        literal_read = True
        return reader(size)

    client.read = bounded_read
    try:
        with tree.deadline(tree.seconds):
            data = headers.request(client, client.uid, "FETCH", str(uid),
                                   f"(UID RFC822.SIZE INTERNALDATE FLAGS BODY.PEEK[]<{offset}.{CHUNK}>)")
    finally:
        client.read = reader
    # imaplib splits a literal into (response prefix, payload) plus a suffix.
    # An unsolicited flag update may appear as a separate response row.
    body_response = None
    rows = iter(data)
    for row in rows:
        if row is None:
            continue
        literal = None
        if isinstance(row, tuple):
            prefix, literal = row
            suffix = next(rows, None)
            if not isinstance(suffix, bytes):
                raise DownloadError("incomplete body FETCH response")
            row = prefix + suffix
        if re.fullmatch(rb"[0-9]+ \((?:UID [0-9]+ )?FLAGS \([^()]*\)(?: UID [0-9]+)?\)", row, re.I):
            continue
        if body_response is not None:
            raise DownloadError("duplicate body FETCH response")
        size, date, flags, body = body_row(row, uid, offset, CHUNK)
        if isinstance(body, int):
            if literal is None or len(literal) != body:
                raise DownloadError("incomplete body FETCH literal")
            body = literal
        elif literal is not None:
            raise DownloadError("unexpected body literal")
        body_response = size, date, flags, body
    return body_response


def message_file(stage, folder, uid):
    return stage / f"f{folder}-u{uid}.eml"


def verified_file(path, size, digest):
    if not private_file(path, optional=True) or path.stat().st_size != size:
        return False
    check = hashlib.sha256()
    with open_private(path) as reader:
        for piece in iter(lambda: reader.read(CHUNK), b""):
            check.update(piece)
    return check.hexdigest() == digest


def stage_message(tree, db, stage, number, uid, saved_digest):
    dest = message_file(stage, number, uid)
    pending = stage / "message.partial"
    private_file(pending, optional=True)
    private_file(dest, optional=True)
    offset, size = 0, None
    check = hashlib.sha256()
    # At most one bounded reply is outstanding; a failed disk write prevents
    # the next request. Keep partial evidence and restart only this message.
    with open_private(pending, os.O_WRONLY | os.O_CREAT) as sink:
        sink.truncate(0)
        while size is None or offset < size:
            fetched = fetch_chunk(tree, uid, offset)
            if fetched is None:
                with db:
                    db.execute("UPDATE messages SET state='missing' WHERE folder=? AND uid=?", (number, uid))
                raise DownloadError(f"captured UID {uid} is missing; start a fresh inventory under a new output name")
            reported, date, flags, piece = fetched
            if size is None:
                size, internaldate, original_flags = reported, date, flags
                free = os.fstatvfs(sink.fileno())
                if size > free.f_bavail * free.f_frsize:
                    raise DownloadError("insufficient staging space; free space and repeat with --resume")
            elif reported != size or internaldate != date:
                raise DownloadError("message metadata changed during FETCH")
            sink.write(piece)
            check.update(piece)
            offset += len(piece)
            if offset and offset % (64 * 1024 * 1024) == 0:
                print(f"Download: UID {uid}, {offset}/{size} bytes", file=sys.stderr)
        if saved_digest is not None and check.hexdigest() != saved_digest:
            raise DownloadError("retrieved bytes differ from the captured message; keep this incomplete attempt")
        sink.flush()
        os.fsync(sink.fileno())
    os.replace(pending, dest)
    sync_dir(stage)
    with db:
        db.execute("""UPDATE messages SET state='complete', size=?, sha256=?,
                      internaldate=COALESCE(internaldate, ?), flags=COALESCE(flags, ?)
                      WHERE folder=? AND uid=?""",
                   (size, check.hexdigest(), internaldate, json.dumps(original_flags), number, uid))


def stage_bodies(tree, stage, db, folders):
    captured = 0
    for number, folder in enumerate(folders, 1):
        if folder["validity"] is None:
            continue
        completed = db.execute("SELECT message_count FROM inventories WHERE folder=?", (number,)).fetchone()
        if completed is None:
            inventory(tree, db, number, folder)
        elif completed != db.execute("SELECT count(*) FROM messages WHERE folder=?", (number,)).fetchone():
            raise DownloadError("saved UID inventory is incomplete; start a fresh inventory under a new output name")
    for number, folder in enumerate(folders, 1):
        if folder["validity"] is None:
            continue
        examine(tree, bytes.fromhex(folder["raw"]), folder["validity"])
        messages = db.execute("SELECT uid, state, size, sha256 FROM messages WHERE folder=? ORDER BY uid", (number,))
        for uid, state, size, digest in messages:
            if type(uid) is not int or not 1 <= uid <= folder["upper"]:
                raise DownloadError("invalid saved UID inventory")
            if state != "complete" or not verified_file(message_file(stage, number, uid), size, digest):
                stage_message(tree, db, stage, number, uid, digest)
            captured += 1
            if captured == 1 or captured % 100 == 0:
                print(f"Download: {captured} messages staged", file=sys.stderr)
    print(f"Download: {captured} messages staged", file=sys.stderr)


@contextlib.contextmanager
def working_directory(fd):
    # The shell helper is single-threaded. Pin relative SQLite and file opens
    # to the locked directory, even if another process moves its public name.
    previous = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fchdir(fd)
        yield
    finally:
        os.fchdir(previous)
        os.close(previous)


@contextlib.contextmanager
def output_parent(path):
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    current = os.open(path.anchor, flags)
    try:
        for component in path.parts[1:]:
            following = os.open(component, flags, dir_fd=current)
            os.close(current)
            current = following
        with working_directory(current):
            yield current
    finally:
        os.close(current)


def download(tree, args, account_identity):
    source, output, recursive, resume = options(args)
    with output_parent(output.parent) as parent_fd:
        export(tree, source, output, recursive, resume, account_identity, parent_fd)


def export(tree, source, output, recursive, resume, account_identity, parent_fd):
    if os.path.lexists(output.name):
        raise DownloadError("output already exists; choose a new output name")
    stage_name = Path("." + output.name + ".incomplete")
    root = tree.resolve_write(source)
    if root is tree.root or not root.selectable and not recursive:
        raise DownloadError("choose a mailbox, or use --recursive for a container")
    identity = hashlib.sha256(json.dumps(account_identity, separators=(",", ":")).encode()).hexdigest()
    binding = dict(version=VERSION, account=identity, source=root.raw.hex(),
                   delimiter=root.delimiter.hex() if root.delimiter else None,
                   utf8=tree.client.utf8_enabled, recursive=recursive, output=str(output))
    exists = os.path.lexists(stage_name)
    if resume and not exists:
        raise DownloadError("no staging directory for this output")
    if not resume:
        if exists:
            raise DownloadError("staging already exists; repeat with --resume")
        create_stage(stage_name, dict(binding=binding, folders=freeze(tree, root, recursive)))
    with stage_lock(stage_name) as stage_fd, working_directory(stage_fd):
        stage = Path(".")
        selection_path = stage / "selection.json"
        with open_private(selection_path) as stream:
            selection_stat = os.fstat(stream.fileno())
            encoded = stream.read()
        selection = json.loads(encoded)
        if selection["binding"] != binding:
            raise DownloadError("checkpoint identity mismatch; use the original account, folder, options and output")
        folders = selection["folders"]
        # Complete this read-only generation fence for every frozen mailbox
        # before even opening SQLite (which can recover a hot journal).
        if resume:
            for folder in folders:
                if folder["validity"] is not None:
                    examine(tree, bytes.fromhex(folder["raw"]), folder["validity"])
        database = stage / "inventory.sqlite"
        private_file(database)
        db_stat = database.lstat()
        for suffix in ("-journal", "-wal", "-shm"):
            private_file(Path(str(database) + suffix), optional=True)
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute("PRAGMA trusted_schema=OFF")
            if db.execute("SELECT digest FROM selection").fetchall() != [(hashlib.sha256(encoded).hexdigest(),)]:
                raise DownloadError("inventory and frozen selection do not match")
            db.execute("PRAGMA synchronous=FULL")
            stage_bodies(tree, stage, db, folders)
            # Pin one consistent database view for the manifest, both tar passes
            # and cleanup. Staging writes have already committed at this point.
            db.execute("BEGIN")
            export_archive.publish(db, selection, output, parent_fd, stage_name, stage_fd)
            try:
                export_archive.verify_location(output, parent_fd, stage_name, stage_fd)
                export_archive.clean_messages(db)
            except (OSError, DownloadError, KeyboardInterrupt):
                print("Verified archive published; changed staging retained", file=sys.stderr)
                return
        try:
            export_archive.verify_location(output, parent_fd, stage_name, stage_fd)
            for path, expected in ((selection_path, selection_stat), (database, db_stat)):
                if not os.path.samestat(path.lstat(), expected):
                    raise DownloadError("checkpoint changed after publication")
                path.unlink()
            export_archive.verify_location(output, parent_fd, stage_name, stage_fd)
            os.rmdir(stage_name, dir_fd=parent_fd)
            os.fsync(parent_fd)
        except (OSError, DownloadError, KeyboardInterrupt):
            print("Verified archive published; remaining staging files retained", file=sys.stderr)


def run(tree, args, identity):
    try:
        download(tree, args, identity)
    except DownloadError:
        raise
    except (imaplib.IMAP4.abort, TimeoutError, ConnectionError):
        print("Download interrupted; repeat with --resume if staging exists", file=sys.stderr)
        raise
    except OSError as exc:
        if exc.errno in (errno.ENOSPC, errno.EDQUOT):
            raise DownloadError("staging disk full; free space and repeat with --resume") from None
        raise DownloadError("cannot access output or staging files; check path, permissions and space") from None
    except sqlite3.Error as exc:
        if getattr(exc, "sqlite_errorcode", None) == getattr(sqlite3, "SQLITE_FULL", 13):
            raise DownloadError("checkpoint disk full; free space and repeat with --resume") from None
        raise DownloadError("cannot read or update the checkpoint; incomplete staging retained") from None
    except (ValueError, KeyError, imaplib.IMAP4.error):
        raise DownloadError("mailbox or checkpoint unavailable; incomplete staging retained") from None
