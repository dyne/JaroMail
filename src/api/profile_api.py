"""Expose the native account and group record editor through the API."""

import json
import os
import subprocess
import sys
import tempfile


METHODS = (
    "account.list", "account.get", "account.put", "account.credential",
    "group.list", "group.get", "group.put", "group.delete",
)
MAX_RECORD = 1024 * 1024


class ProfileApiError(Exception):
    pass


def text(params, key):
    value = params.get(key)
    if not isinstance(value, str) or not value or "\0" in value:
        raise ProfileApiError("invalid_request")
    return value


def run_native(args, root, workdir, data=b"", *, writing=False, timeout=8, output_limit=MAX_RECORD):
    env = dict(os.environ, JAROWORKDIR=str(workdir), JAROMAILDIR=str(root), QUIET="1")
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run(args, input=data, stdout=output, stderr=subprocess.DEVNULL,
                                    env=env, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise ProfileApiError("write_uncertain" if writing else "timeout") from error
        output.seek(0)
        raw = output.read(output_limit + 1)
        if len(raw) > output_limit:
            raise ProfileApiError("limit")
    return result.returncode, raw


def operation(workdir, root, method, params):
    kind, action = method.split(".")
    if kind == "account":
        directory = os.environ.get("JAROACCOUNT") or root / "Accounts"
    else:
        directory = root / "Groups"
    args = [sys.executable, "-I", "-B", workdir / "profile/records.py", kind, workdir, directory, action]
    data = b""
    writing = action in ("put", "delete", "credential")
    if action != "list":
        args.append(text(params, "id"))
    if action == "credential":
        service = text(params, "service")
        if service not in ("imap", "smtp"):
            raise ProfileApiError("invalid_request")
        args.extend(("--service", service))
        data = text(params, "secret").encode("utf-8")
        if len(data) > 4096 or b"\n" in data or b"\r" in data:
            raise ProfileApiError("invalid_request")
    if writing:
        args.extend(("--expect", text(params, "revision")))
    if action == "put":
        fields = params.get("fields")
        if not isinstance(fields, dict):
            raise ProfileApiError("invalid_request")
        data = json.dumps(fields, ensure_ascii=False).encode("utf-8")
        if len(data) > MAX_RECORD:
            raise ProfileApiError("limit")
    code, raw = run_native(args, root, workdir, data, writing=writing)
    if code:
        if kind == "account":
            invalid = "unsupported_account" if action == "get" else "invalid_account"
            codes = {4: invalid, 6: "stale_account", 7: "keyring_unavailable"}
        else:
            codes = {1: "contact_conflict", 4: "invalid_contact", 6: "contact_conflict"}
        raise ProfileApiError(codes.get(code, "backend_failure"))
    try:
        value = json.loads(raw)
    except (ValueError, RecursionError) as error:
        raise ProfileApiError("invalid_response") from error
    if not isinstance(value, dict) or value.get("version") != 1:
        raise ProfileApiError("invalid_response")
    return value
