"""Refresh notmuch under the existing native writer lock."""

import json
import os
import re
import subprocess

import operations
from profile_api import ProfileApiError


REFRESH = '''
source "$JAROWORKDIR/zlibs/bootstrap" source || exit 1
[[ -r "$MAILDIRS/notmuch-config" ]] && (( $+commands[notmuch] )) || exit 4
[[ "$(notmuch --config="$MAILDIRS/notmuch-config" config get database.mail_root)" = "$MAILDIRS" ]] || exit 4
newlock "$MAILDIRS/cache/notmuch-index" || exit $?
{ rc=0
  scan_index() { timeout --kill-after=5s 120s notmuch --config="$MAILDIRS/notmuch-config" new --quiet "$@" }
  scan_index || rc=1
  if [[ -n "$1" && $rc = 0 ]]; then
    timeout --kill-after=5s 120s python3 -I -B "$1" || rc=1
    scan_index --full-scan || rc=1
  fi
} always { unlock "$MAILDIRS/cache/notmuch-index" || rc=1 }
exit $rc
'''


def refresh(workdir, root, args):
    stale = args.get("stale", [])
    if not isinstance(stale, list):
        raise ProfileApiError("invalid_request")
    for row in stale:
        if (not isinstance(row, dict) or not isinstance(row.get("folder"), str)
                or row.get("part") not in ("new", "cur") or not operations.component(row.get("name"))
                or not isinstance(row.get("revision"), str) or not re.fullmatch("[a-f0-9]{64}", row["revision"])):
            raise ProfileApiError("invalid_resource")
        operations.folder_parts(row["folder"])
        ident = row.get("indexedId")
        if ident is not None and (not isinstance(ident, str) or "\0" in ident or len(ident.encode()) > 4096):
            raise ProfileApiError("invalid_resource")
    payload = json.dumps(stale).encode("utf-8")
    env = dict(os.environ, JAROWORKDIR=str(workdir), JAROMAILDIR=str(root), QUIET="1")
    command = ["zsh", "-fc", REFRESH, "jaro-api-index", workdir / "api/reconcile.py" if stale else ""]
    try:
        result = subprocess.run(command, input=payload, env=env, timeout=390,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return {"outcome": "failed"}
    outcomes = {0: "refreshed", 3: "busy", 4: "unavailable", 127: "unavailable"}
    return {"outcome": outcomes.get(result.returncode, "failed")}
