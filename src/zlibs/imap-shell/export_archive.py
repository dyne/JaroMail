"""Write and independently reread a portable folder export before publication."""

import gzip
import hashlib
import json
import os
import re
import stat
import sys
import tarfile
import tempfile
import zlib

BLOCK = 128 * 1024
PARTIAL = "archive.partial"


class DownloadError(ValueError):
    """Locally authored operator guidance."""


class PublishedError(DownloadError):
    """Publication happened before a subsequent failure."""


def checked_file(attributes):
    if (not stat.S_ISREG(attributes.st_mode) or attributes.st_nlink != 1
            or attributes.st_uid != os.geteuid() or attributes.st_mode & 0o077):
        raise DownloadError("unexpected or unsafe staging file")


def open_private(path, flags=os.O_RDONLY):
    fd = os.open(path, flags | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        checked_file(os.fstat(fd))
    except BaseException:
        os.close(fd)
        raise
    return os.fdopen(fd, "r+b" if flags & os.O_RDWR else "wb" if flags & os.O_WRONLY else "rb")


def identity_is(path, fd, parent=None):
    try:
        return os.path.samestat(os.stat(path, dir_fd=parent, follow_symlinks=False), os.fstat(fd))
    except FileNotFoundError:
        return False


def hash_stream(stream):
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(BLOCK), b""):
        digest.update(block)
    return digest.hexdigest()


def members(db):
    for folder, uid, size, digest in db.execute("SELECT folder,uid,size,sha256 FROM messages ORDER BY folder,uid"):
        # Folder numbers and UIDs have been checked against the frozen selection.
        base = f"f{folder}-u{uid}.eml"
        yield base, "messages/" + base, size, digest


def write_manifest(stream, db, selection):
    def record(**fields):
        stream.write(json.dumps(fields, separators=(",", ":"), ensure_ascii=True).encode("ascii") + b"\n")

    binding = selection["binding"]
    record(type="export", version=binding["version"], recursive=binding["recursive"],
           mailbox_encoding="UTF-8" if binding["utf8"] else "modified UTF-7")
    total = inventories = 0
    for number, f in enumerate(selection["folders"], 1):
        count = 0
        if f["validity"] is not None:
            row = db.execute("SELECT message_count FROM inventories WHERE folder=?", (number,)).fetchone()
            if not row or type(row[0]) is not int or row[0] < 0:
                raise DownloadError("folder inventory incomplete")
            count = row[0]
            inventories += 1
        record(type="folder", id=number, path=f["path"], raw_hex=f["raw"], delimiter_hex=f["delimiter"],
               uidvalidity=f["validity"], upper_uid=f["upper"], message_count=count)
        actual = 0
        rows = db.execute("""SELECT uid,state,size,sha256,internaldate,flags FROM messages
                             WHERE folder=? ORDER BY uid""", (number,))
        for uid, state, size, digest, date, flags_json in rows:
            if (f["upper"] is None or type(uid) is not int or not 1 <= uid <= f["upper"] or state != "complete"
                    or type(size) is not int or size < 0 or not isinstance(digest, str)
                    or not re.fullmatch("[0-9a-f]{64}", digest) or not isinstance(date, str)
                    or not isinstance(flags_json, str)):
                raise DownloadError("incomplete message inventory")
            flags = json.loads(flags_json)
            if not isinstance(flags, list) or not all(isinstance(flag, str) for flag in flags):
                raise DownloadError("invalid saved message flags")
            record(type="message", folder=number, uid=uid, path=f"messages/f{number}-u{uid}.eml",
                   size=size, sha256=digest, internaldate=date, flags=flags)
            actual += 1
        if actual != count:
            raise DownloadError("message inventory count mismatch")
        total += actual
    if (total != db.execute("SELECT count(*) FROM messages").fetchone()[0] or
            inventories != db.execute("SELECT count(*) FROM inventories").fetchone()[0]):
        raise DownloadError("checkpoint contains records outside the frozen selection")
    return total


def add_member(tar, name, size, source):
    item = tarfile.TarInfo(name)
    item.mode, item.size = 0o600, size
    # PAX size headers support messages larger than the USTAR size field.
    tar.addfile(item, source)
    tar.members.clear()  # tarfile otherwise retains every member on older Python.


def write_tar(stream, manifest, manifest_size, db):
    manifest.seek(0)
    with gzip.GzipFile(filename="", mode="wb", fileobj=stream, mtime=0) as compressed:
        with tarfile.open(mode="w|", fileobj=compressed, format=tarfile.PAX_FORMAT) as tar:
            add_member(tar, "manifest.jsonl", manifest_size, manifest)
            for base, name, size, _ in members(db):
                with open_private(base) as source:
                    if os.fstat(source.fileno()).st_size != size:
                        raise DownloadError("staged message size changed before packaging")
                    add_member(tar, name, size, source)


def check_member(tar, name, size, digest):
    item = tar.next()
    if (item is None or item.type != tarfile.REGTYPE or item.name != name or item.size != size
            or item.mode != 0o600 or item.linkname or item.uid != 0 or item.gid != 0
            or set(item.pax_headers) - {"size"}):
        raise DownloadError("archive member list or metadata failed verification")
    with tar.extractfile(item) as source:
        if hash_stream(source) != digest:
            raise DownloadError("archive member failed SHA-256 verification")
    tar.members.clear()


def verify_tar(stream, manifest_size, manifest_digest, db):
    stream.seek(0)
    with gzip.GzipFile(fileobj=stream, mode="rb") as uncompressed:
        with tarfile.open(mode="r:", fileobj=uncompressed) as tar:
            check_member(tar, "manifest.jsonl", manifest_size, manifest_digest)
            for _, name, size, digest in members(db):
                check_member(tar, name, size, digest)
            # The complete trailer consists of two zero blocks plus record
            # padding; reading to gzip EOF also verifies its CRC and length.
            uncompressed.seek(tar.offset)
            tail = uncompressed.read(tarfile.RECORDSIZE + 2 * tarfile.BLOCKSIZE + 1)
            if (not 2 * tarfile.BLOCKSIZE <= len(tail) <= tarfile.RECORDSIZE + tarfile.BLOCKSIZE
                    or len(tail) % tarfile.BLOCKSIZE or any(tail)):
                raise DownloadError("archive trailer failed verification")


def verify_location(output, parent_fd, stage_name, stage_fd):
    if not identity_is(output.parent, parent_fd) or not identity_is(stage_name, stage_fd, parent_fd):
        raise DownloadError("output parent or staging directory changed; incomplete attempt retained")


def publish(db, selection, output, parent_fd, stage_name, stage_fd):
    verify_location(output, parent_fd, stage_name, stage_fd)
    with tempfile.TemporaryFile(dir=".") as manifest, open_private(PARTIAL, os.O_RDWR | os.O_CREAT) as stream:
        # Validate before truncation so a stale symlink or hard link is harmless.
        stream.truncate(0)
        count = write_manifest(manifest, db, selection)
        size = manifest.tell()
        manifest.seek(0)
        digest = hash_stream(manifest)
        print(f"Download: packaging {count} messages", file=sys.stderr)
        try:
            write_tar(stream, manifest, size, db)
            stream.flush()
            os.fsync(stream.fileno())
            verify_tar(stream, size, digest, db)
        except (tarfile.TarError, EOFError, gzip.BadGzipFile, zlib.error):
            raise DownloadError("archive could not be read and verified; repeat with --resume") from None
        verify_location(output, parent_fd, stage_name, stage_fd)
        if not identity_is(PARTIAL, stream.fileno(), stage_fd):
            raise DownloadError("verified archive was replaced; incomplete attempt retained")
        # Hard linking is an atomic no-replace publication on this filesystem.
        # On Linux link the verified open inode directly, even if its name moves.
        reference = f"/proc/self/fd/{stream.fileno()}"
        try:
            if sys.platform.startswith("linux") and os.path.exists(reference):
                os.link(reference, output.name, dst_dir_fd=parent_fd, follow_symlinks=True)
            else:
                os.link(PARTIAL, output.name, src_dir_fd=stage_fd, dst_dir_fd=parent_fd, follow_symlinks=False)
        except FileExistsError:
            raise DownloadError("output appeared during packaging; incomplete staging retained") from None
        except (OSError, KeyboardInterrupt):
            if identity_is(output.name, stream.fileno(), parent_fd):
                raise PublishedError("Verified archive published; directory sync unconfirmed; staging retained") from None
            raise
        try:
            os.fsync(parent_fd)
        except (OSError, KeyboardInterrupt):
            raise PublishedError("Verified archive published; directory sync unconfirmed; staging retained") from None
        if not identity_is(output.name, stream.fileno(), parent_fd):
            raise DownloadError("published path changed; staging retained")
        print(f"Download verified and published: {count} messages", file=sys.stderr)
        # Only the verified name in our pinned staging directory is ours.
        try:
            if identity_is(PARTIAL, stream.fileno(), stage_fd):
                os.unlink(PARTIAL, dir_fd=stage_fd)
        except (OSError, KeyboardInterrupt):
            raise PublishedError("Verified archive published; staging cleanup interrupted") from None


def clean_messages(db):
    # Work from the verified inventory, never recursively delete a directory
    # tree or sweep foreign names. Retain any replaced or modified member.
    for base, _, size, digest in members(db):
        with open_private(base) as stream:
            if (os.fstat(stream.fileno()).st_size != size or hash_stream(stream) != digest
                    or not identity_is(base, stream.fileno())):
                raise DownloadError("staging changed after publication")
            os.unlink(base)
