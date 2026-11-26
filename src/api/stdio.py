"""Serve the version 1 local API on the caller's stdin and stdout."""

from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys

MAX_REQUEST = 64 * 1024
MAX_REPLY = 1024 * 1024


class ApiError(Exception):
    pass


@contextmanager
def open_root(path):
    fd = os.open(path, operations.DIR)
    try:
        if os.fstat(fd).st_uid != os.getuid():
            raise ApiError("root_unavailable")
        yield fd
    finally:
        os.close(fd)


def contact_records(helper, root, operation, *args):
    with subprocess.Popen(
        [helper, root, operation, *args],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    ) as child:
        data = child.stdout.read(MAX_REPLY + 1)
        if len(data) > MAX_REPLY:
            child.kill()
            raise ApiError("limit")
        if child.wait():
            raise ApiError("invalid_contact")
    fields = data.decode("utf-8", errors="replace").split("\0")
    if len(fields) < 3 or fields.pop() != "" or fields[0] != "JCT1":
        raise ApiError("invalid_response")
    generation = fields[1]
    if not generation.isascii() or not generation.isdecimal():
        raise ApiError("invalid_response")
    return generation, fields[2:]


def contacts_list(helper, root, book):
    generation, fields = contact_records(helper, root, "list", book)
    contacts = []
    index = 0
    while index < len(fields):
        ident, revision, name, nickname, notes, _date, count = fields[index:index + 7]
        index += 7
        end = index + int(count)
        if end < index or end >= len(fields):
            raise ApiError("invalid_response")
        addresses = fields[index:end]
        extra = int(fields[end])
        index = end + 1 + 2 * extra
        if extra < 0 or index > len(fields):
            raise ApiError("invalid_response")
        contacts.append(dict(id=ident, revision=revision, name=name, nickname=nickname,
                             notes=notes, addresses=addresses))
    return dict(generation=generation, contacts=contacts)


def dispatch(root, workdir, methods, method, params):
    if method not in methods:
        raise ApiError("unsupported_method")
    if method == "capabilities":
        return dict(protocol=1, methods=methods)
    if method == "maildir.list":
        with open_root(root) as fd:
            return dict(folders=operations.bounded(operations.inventory(fd)))
    if method == "maildir.create":
        name = params.get("folder")
        if not isinstance(name, str) or not name or len(os.fsencode(name)) > 4096:
            raise ApiError("invalid_resource")
        with open_root(root) as fd:
            try:
                operations.create_folder(fd, name, workdir)
            except FileExistsError:
                raise ApiError("folder_exists") from None
            except (NotADirectoryError, ValueError):
                raise ApiError("invalid_resource") from None
        return dict(folder=name)
    helper = workdir / "bin/jaro-contact-records"
    if method == "contacts.books":
        generation, books = contact_records(helper, root, "books")
        return dict(generation=generation, books=books)
    book = params.get("book")
    if not isinstance(book, str) or not book or "\0" in book or len(os.fsencode(book)) > MAX_REQUEST:
        raise ApiError("invalid_request")
    return contacts_list(helper, root, book)


def reply(message_id, *, result=None, code=None):
    value = dict(id=message_id)
    if code:
        value["error"] = dict(code=code)
    else:
        value["result"] = result
    line = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    if len(line) >= MAX_REPLY:
        reply(message_id, code="limit")
        return
    sys.stdout.buffer.write(line + b"\n")
    sys.stdout.buffer.flush()


def serve(workdir, root):
    methods = ["capabilities", "maildir.list"]
    if sys.platform == "linux" and hasattr(ctypes.CDLL(None), "renameat2"):
        methods.append("maildir.create")
    if os.access(workdir / "bin/jaro-contact-records", os.X_OK):
        methods.extend(("contacts.books", "contacts.list"))
    while line := sys.stdin.buffer.readline(MAX_REQUEST + 1):
        if len(line) > MAX_REQUEST or not line.endswith(b"\n"):
            print("jaro api: incomplete or oversized request", file=sys.stderr)
            return 2
        message_id = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ApiError("invalid_request")
            ident, method, params = request.get("id"), request.get("method"), request.get("params", {})
            if type(ident) is not int or not 0 <= ident <= 2**53 - 1:
                raise ApiError("invalid_request")
            message_id = ident
            if not isinstance(method, str) or not isinstance(params, dict):
                raise ApiError("invalid_request")
            result = dispatch(root, workdir, methods, method, params)
        except ApiError as error:
            reply(message_id, code=str(error))
        except (UnicodeError, json.JSONDecodeError, RecursionError):
            reply(message_id, code="invalid_request")
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f"jaro api: {type(error).__name__}", file=sys.stderr)
            reply(message_id, code="backend_failure")
        else:
            reply(message_id, result=result)
    return 0


if __name__ == "__main__":
    runtime, mail_root = map(Path, sys.argv[1:])
    sys.path.insert(0, str(runtime / "maildir"))
    import operations

    try:
        sys.exit(serve(runtime, mail_root))
    except BrokenPipeError:
        sys.exit(1)
