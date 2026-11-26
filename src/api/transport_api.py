"""Native connection and delivery jobs for caller-owned sessions."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from profile_api import ProfileApiError, operation as profile, run_native, text


KINDS = ("transport.fetch", "transport.connect", "outbox.list", "outbox.dispatch",
         "outbox.submit", "outbox.validate", "outbox.completed")
ACCOUNT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._+@-]{0,191}")
IDENT = re.compile(r"[a-f0-9]{64}")
EXPECT = {
    "send": {"eligible", "prepared"},
    "retry": {"inflight", "uncertain"},
    "archive": {"inflight", "uncertain", "accepted", "verified", "complete"},
}
KEYRING = (
    'source "$JAROWORKDIR/zlibs/bootstrap" source || exit 1; check_keyring; '
    'export PASS SECRET_TOOL GNOMEKEY PASSWORD_STORE_DIR; exec "$@"'
)
VALIDATE = ('import sys;sys.path.insert(0,sys.argv[1]);from submission import request;'
            'request(sys.argv[2],sys.argv[3])')
MAX_OUTPUT = 1024 * 1024


def account(args):
    ident = text(args, "account")
    if not ACCOUNT.fullmatch(ident) or ident == "README.txt":
        raise ProfileApiError("invalid_request")
    return ident


def launcher(workdir):
    configured = os.environ.get("JARO_API_LAUNCHER")
    if configured and Path(configured).is_absolute():
        return Path(configured)
    return workdir / "bin/jaro"


def capture(args, root, workdir, *, timeout=300):
    env = dict(os.environ, JAROWORKDIR=str(workdir), JAROMAILDIR=str(root), QUIET="0")
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=err, timeout=timeout)
        out.seek(0)
        err.seek(0)
        raw, diagnostics = out.read(MAX_OUTPUT + 1), err.read(MAX_OUTPUT + 1)
        if len(raw) + len(diagnostics) > MAX_OUTPUT:
            raise ProfileApiError("limit")
    return (result.returncode, raw.decode("utf-8", "replace"),
            re.sub(r"\x1b\[[0-9;]*m", "", diagnostics.decode("utf-8", "replace")))


def connect(workdir, root, args):
    ident = account(args)
    record = profile(workdir, root, "account.get", {"id": ident})
    fields = record["fields"]
    if fields["proto"] != "imap":
        return {"status": "unsupported"}
    argv = [launcher(workdir), "-a", ident]
    code, _, _ = capture([*argv, "isonline", fields["imap"], fields["imap_port"]], root, workdir)
    if code:
        return {"status": "unreachable"}
    wrapped = ["zsh", "-fc", KEYRING, "jaro-api-imap", *argv]
    code, folder_list, diagnostic = capture([*wrapped, "imap", "listfolders"], root, workdir)
    if code:
        if re.search(r"Password input aborted|no password found in keyring", diagnostic):
            return {"status": "credential"}
        return {"status": "authentication" if "tagged command rejected" in diagnostic else "failed"}
    code, _, diagnostic = capture([*wrapped, "imap", "getsize"], root, workdir)
    result = {"status": "connected", "folders": [line for line in folder_list.rstrip("\n").split("\n") if line]}
    size = re.search(r"^.*? \.\s+(\d+) bytes\s*$", diagnostic, re.M) if not code else None
    if size and int(size[1]) <= 2**53 - 1:
        result["bytes"] = int(size[1])
    return result


def fetch(workdir, root, args):
    code, _, diagnostic = capture([launcher(workdir), "-a", account(args), "fetch"], root, workdir)
    index = "unreported"
    if re.search(r"\(\*\) Updating notmuch indexes\s*$", diagnostic, re.M):
        index = "attempted" if code else "refreshed"
    if not code:
        result = "complete"
    elif re.search(r"^This filter generation needs migration; run jaro fresh-filters explicitly$", diagnostic, re.M):
        result = "migration"
    elif code == 8 and re.search(r"fetchmail check failed with status 8|lock|already running", diagnostic):
        result = "transport_busy"
    elif "Invalid password for user" in diagnostic:
        result = "authentication"
    elif "Host unreachable:" in diagnostic:
        result = "unreachable"
    elif re.search(r"Password input aborted|no password found in keyring", diagnostic):
        result = "credential"
    else:
        result = "certificate" if "Invalid or unknown certificate" in diagnostic else "failed"
    return {"status": result, "index": index}


def outbox(workdir, root, kind, args):
    cli = launcher(workdir)
    if kind == "outbox.list":
        command, data, timeout = [cli, "outbox", "list"], b"", 10
    elif kind == "outbox.completed":
        path = text(args, "receipt")
        if not path.startswith("new/") or len(path.split("/")) != 2 or path[4:] in ("", ".", ".."):
            raise ProfileApiError("invalid_request")
        command, data, timeout = [workdir / "bin/jaro-outbox", "completed", root, path], b"", 10
    elif kind in ("outbox.submit", "outbox.validate"):
        ident = account(args)
        value = args.get("payload")
        if not isinstance(value, dict) or value.get("version") != 1:
            raise ProfileApiError("invalid_request")
        data = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()
        if len(data) > 64 * 1024 * 1024:
            raise ProfileApiError("limit")
        if kind == "outbox.validate":
            command = [sys.executable, "-I", "-B", "-c", VALIDATE, workdir / "outbox", ident,
                       workdir / "bin/jaro-filter-compile"]
        else:
            command = [cli, "-a", ident, "outbox", "submit"]
        timeout = 120
    else:
        action = text(args, "action")
        prefix = [] if action == "archive" else ["-a", account(args)]
        if action == "bulk":
            command = [cli, *prefix, "send"]
        else:
            ident, expected = text(args, "id"), text(args, "expected")
            if not IDENT.fullmatch(ident) or expected not in EXPECT.get(action, ()):
                raise ProfileApiError("invalid_request")
            command = [cli, *prefix, "outbox", action, ident, "--expect", expected]
            if action == "retry":
                if args.get("confirmation") is not True:
                    raise ProfileApiError("invalid_request")
                command.append("--confirm-duplicate")
        data, timeout = b"", 300
    code, output = run_native(command, root, workdir, data, writing=kind in ("outbox.dispatch", "outbox.submit"),
                              timeout=timeout)
    return {"code": code, "stdout": output.decode("utf-8", errors="replace")}


def operation(workdir, root, kind, args):
    try:
        if kind == "transport.connect":
            return connect(workdir, root, args)
        if kind == "transport.fetch":
            return fetch(workdir, root, args)
        return outbox(workdir, root, kind, args)
    except subprocess.TimeoutExpired as error:
        raise ProfileApiError("timeout") from error
