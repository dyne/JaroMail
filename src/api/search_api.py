"""Read the notmuch database configured for the caller's Maildir."""

import json
import re

from profile_api import ProfileApiError, run_native


METHODS = ("search.status", "search.summary", "search.messages", "search.files", "search.busy")
STATUS = re.compile(rb"(\d+)\s+[a-f0-9-]+\s+(\d+)\s*")


def offset(params):
    value = params.get("offset")
    if type(value) is not int or not 0 <= value <= 2147483647 - 20:
        raise ProfileApiError("invalid_request")
    return value


def query(params, limit):
    value = params.get("query")
    if not isinstance(value, str) or "\0" in value or not value.strip() or len(value.encode("utf-8")) > limit:
        raise ProfileApiError("invalid_query")
    return value


def operation(workdir, root, method, params):
    busy = (root / "cache/notmuch-index.lock").exists()
    if method == "search.busy":
        return {"busy": busy}
    prefix = ["notmuch", "--config=" + str(root / "notmuch-config")]

    def run(*args):
        try:
            return run_native(prefix + list(args), root, workdir)
        except FileNotFoundError as error:
            raise ProfileApiError("index_unavailable") from error

    code, configured = run("config", "get", "database.mail_root")
    if code or configured.removesuffix(b"\n").decode("utf-8", "surrogateescape") != str(root):
        raise ProfileApiError("index_unavailable")
    if method == "search.status":
        code, output = run("count", "--exclude=false", "--lastmod", "--", "*")
    else:
        args = ["search", "--format=json", "--output=" + method.split(".")[1]]
        if method == "search.summary":
            args.extend((f"--offset={offset(params)}", "--limit=21"))
        if method == "search.messages":
            exclude = params.get("exclude")
            if type(exclude) is not bool:
                raise ProfileApiError("invalid_request")
            args.append("--exclude=true" if exclude else "--exclude=false")
            if not exclude:
                args.extend(("--sort=oldest-first", f"--offset={offset(params)}", "--limit=21"))
        if method == "search.files":
            args.append("--exclude=false")
        code, output = run(*args, "--", query(params, 96 * 1024 if method == "search.messages" else 16 * 1024))
    if code:
        available, _ = run("count", "--", "*")
        raise ProfileApiError("index_unavailable" if available else "invalid_query")
    if method == "search.status":
        fields = STATUS.fullmatch(output)
        if fields is None:
            raise ProfileApiError("invalid_response")
        return {"messages": int(fields[1]), "revision": int(fields[2]), "busy": busy}
    try:
        rows = json.loads(output.decode("utf-8", "surrogateescape"))
    except (ValueError, RecursionError) as error:
        raise ProfileApiError("invalid_response") from error
    if not isinstance(rows, list):
        raise ProfileApiError("invalid_response")
    return {"rows": rows, "busy": busy}
