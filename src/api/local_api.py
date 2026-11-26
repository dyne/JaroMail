"""Small native operations used by the Mobile presentation layer."""

import base64
from functools import partial
import json
import os
import re
import stat
import sys

import operations
from profile_api import ProfileApiError, run_native, text
from transport_api import ACCOUNT, account, launcher


METHODS = ("account.aliases", "account.headers", "contacts.extract",
           "contacts.check-address", "contacts.convert", "doctor.inspect")
FORMATS = ("vcard", "mutt", "csv", "abook", "ldif", "pine", "allcsv", "palmcsv")
HEADERS = (
    'source "$JAROWORKDIR/zlibs/bootstrap" source || exit 1; account="$1"; '
    'read_account || exit 1; print -rn -- "$my_hdr"'
)
REPLAY = 'source "$JAROWORKDIR/zlibs/bootstrap" source || exit $?; save_replay extract'


def aliases(root):
    try:
        fd = os.open(root / "Aliases.txt", operations.FILE)
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ProfileApiError("invalid_resource")
            raw = stream.read(1024 * 1024 + 1)
    except FileNotFoundError:
        return {"aliases": []}
    except OSError as error:
        raise ProfileApiError("invalid_resource") from error
    if len(raw) > 1024 * 1024:
        raise ProfileApiError("invalid_resource")
    addresses = dict.fromkeys(line.strip() for line in raw.decode("utf-8", errors="replace").split("\n"))
    return {"aliases": [a for a in addresses if not a.startswith("#") and re.fullmatch(r"[^\s<>@]+@[^\s<>@]+", a)]}


def operation(workdir, root, method, params):
    if method == "account.aliases":
        return aliases(root)
    if method == "account.headers":
        ident = account(params)
        code, output = run_native(["zsh", "-fc", HEADERS, "jaro-api-headers", ident], root, workdir)
        if code:
            raise ProfileApiError("backend_failure")
        return {"headers": output.decode("utf-8", errors="replace")}
    if method == "doctor.inspect":
        selected = params.get("account")
        args = [launcher(workdir)]
        if selected is not None:
            if not isinstance(selected, str) or not ACCOUNT.fullmatch(selected):
                raise ProfileApiError("invalid_request")
            args.extend(("-a", selected))
        code, output = run_native([*args, "doctor", "--json"], root, workdir)
        if code not in (0, 1, 2):
            raise ProfileApiError("backend_failure")
        return json.loads(output)
    if method == "contacts.check-address":
        addr = text(params, "address")
        code, _ = run_native([workdir / "bin/jaro-filter-compile", "--check-address", addr], root, workdir)
        return {"valid": code == 0}
    if method == "contacts.convert":
        input_format, output_format = params.get("from"), params.get("to")
        if input_format not in FORMATS or output_format not in ("abook", "ldif"):
            raise ProfileApiError("invalid_request")
        data = text(params, "text").encode()
        code, output = run_native([workdir / "bin/abook", "--convert", "--informat", input_format,
                                   "--outformat", output_format], root, workdir, data)
        if code:
            raise ProfileApiError("invalid_contact")
        return {"text": output.decode("utf-8", errors="replace")}
    scope = params.get("scope")
    if scope not in ("sender", "recipient", "all"):
        raise ProfileApiError("invalid_request")
    encoded = params.get("headers")
    if not isinstance(encoded, str):
        raise ProfileApiError("invalid_request")
    try:
        headers = base64.b64decode(encoded, validate=True)
    except ValueError as error:
        raise ProfileApiError("invalid_request") from error
    if len(headers) > 128 * 1024:
        raise ProfileApiError("limit")
    args = [] if scope == "all" else ["-x", "From" if scope == "sender" else "To:Cc"]
    code, output = run_native([workdir / "bin/fetchaddr", *args, "-a", "-0"], root, workdir, headers)
    if code:
        raise ProfileApiError("backend_failure")
    fields = output.decode("utf-8", errors="replace").split("\0")
    if fields.pop() or len(fields) % 2:
        raise ProfileApiError("invalid_response")
    return {"fields": fields}


def remember(workdir, root, args):
    value = args.get("text")
    if not isinstance(value, str):
        raise ProfileApiError("invalid_request")
    data = value.encode()
    code, raw = run_native(["zsh", "-fc", REPLAY], root, workdir, data, writing=True, timeout=None)
    if code or raw != data:
        raise ProfileApiError("backend_failure")
    return {"saved": True}


def lookup(workdir, root, args):
    rows = args.get("rows")
    if not isinstance(rows, list):
        raise ProfileApiError("invalid_request")
    for row in rows:
        if (not isinstance(row, dict) or row.get("part") not in ("cur", "new")
                or not isinstance(row.get("folder"), str) or not operations.component(row.get("name"))):
            raise ProfileApiError("invalid_resource")
        operations.folder_parts(row["folder"])
    cmd = [sys.executable, "-I", "-B", workdir / "api/reconcile.py", "--lookup"]
    code, raw = run_native(cmd, root, workdir, json.dumps(rows).encode(), output_limit=8 * 1024 * 1024)
    if code:
        raise ProfileApiError("backend_failure")
    return {"ids": json.loads(raw)}


def handlers(workdir, root):
    return {"contacts.remember": partial(remember, workdir, root),
            "search.lookup": partial(lookup, workdir, root)}
