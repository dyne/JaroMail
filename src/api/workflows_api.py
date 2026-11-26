"""Run allowlisted native workflows with bounded request and result values."""

from functools import partial
import json
import sys

from profile_api import ProfileApiError, run_native


POLICY = ("catalog", "preview", "publish", "dry-run", "apply", "explain", "refresh-policy",
          "export", "check-import", "import")
MAINTENANCE = ("preview", "apply", "remember", "deliver")
SNAPSHOTS = ("catalog", "create", "verify", "restore")
REPLAY = ("extract", "parse", "notmuch", "search", "imap", "last")
STATS = ("folder", "domain", "email", "name")


def json_record(args, root, workdir, data, *, failure="backend_failure", **options):
    code, output = run_native(args, root, workdir, data, **options)
    if code:
        raise ProfileApiError(failure)
    try:
        value = json.loads(output)
    except (ValueError, RecursionError) as error:
        raise ProfileApiError("invalid_response") from error
    if not isinstance(value, dict):
        raise ProfileApiError("invalid_response")
    return value


def worker(workdir, root, family, request):
    allowed = {"policy": POLICY, "maintenance": MAINTENANCE, "snapshot": SNAPSHOTS}[family]
    op = request.get("op")
    data = request.get("request")
    if op not in allowed or not isinstance(data, dict):
        raise ProfileApiError("invalid_request")
    cli = workdir / "bin/jaro"
    cmd = [sys.executable, "-I", "-B", workdir / f"api/{family}_workflow.py"]
    if family == "policy":
        cmd.extend((op, cli))
        payload = data
        writing = op in ("publish", "apply", "import", "refresh-policy")
    elif family == "snapshot":
        settings = request.get("settings")
        if not isinstance(settings, dict) or not isinstance(settings.get("auth"), str):
            raise ProfileApiError("invalid_request")
        cmd.append(cli)
        payload = [{**data, "op": op}, settings]
        writing = op in ("create", "restore")
    else:
        cmd.append(cli)
        payload = {**data, "op": op}
        writing = op != "preview"
    deadline = None if writing or (family == "snapshot" and op == "verify") else 8
    if family == "snapshot" and op in ("catalog", "verify"):
        deadline = None
    return json_record(cmd, root, workdir, json.dumps(payload, separators=(",", ":")).encode(),
                       writing=writing, timeout=deadline, output_limit=8 * 1024 * 1024,
                       failure="maintenance_failed" if family == "maintenance" else "backend_failure")


def replay(workdir, root, request):
    producer = request.get("producer")
    if producer not in REPLAY:
        raise ProfileApiError("invalid_request")
    return json_record([workdir / "bin/jaro", "replay-record", producer], root, workdir, b"",
                       output_limit=2 * 1024 * 1024, failure="missing_replay")


def stats(workdir, root, request):
    dimension, rows = request.get("dimension"), request.get("rows")
    if dimension not in STATS or not isinstance(rows, list) or len(rows) > 100000:
        raise ProfileApiError("invalid_request")
    if any(not isinstance(row, str) or "\0" in row or len(row.encode("utf-8", "surrogateescape")) > 4096 for row in rows):
        raise ProfileApiError("limit")
    data = b"".join(row.encode("utf-8", "surrogateescape") + b"\0" for row in rows)
    if len(data) > 8 * 1024 * 1024:
        raise ProfileApiError("limit")
    code, output = run_native([workdir / "bin/jaro", "stats0", dimension], root, workdir, data,
                              output_limit=10 * 1024 * 1024)
    if code:
        raise ProfileApiError("backend_failure")
    fields = output.decode("utf-8", "surrogateescape").split("\0")
    if len(fields) < 2 or fields[0] != "JST1" or fields.pop() or len(fields[1:]) % 2:
        raise ProfileApiError("invalid_response")
    counts = []
    for i in range(1, len(fields), 2):
        if not fields[i + 1].isascii() or not fields[i + 1].isdigit():
            raise ProfileApiError("invalid_response")
        counts.append(dict(category=fields[i], count=int(fields[i + 1])))
    return {"counts": counts}


def handlers(workdir, root):
    result = {family: partial(worker, workdir, root, family) for family in ("policy", "maintenance", "snapshot")}
    result["replay.get"] = partial(replay, workdir, root)
    result["stats.count"] = partial(stats, workdir, root)
    return result
