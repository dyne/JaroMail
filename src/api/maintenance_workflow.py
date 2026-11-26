"""Revalidate bounded native selections while holding their Maildir writer locks."""
import base64
from contextlib import ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(os.environ["JAROWORKDIR"]) / "maildir"))
from filing_api import matches, native, ordinary


def create_folder(root, name, *, existing_ok=False):
    return native.create_folder(root, name, os.environ["JAROWORKDIR"], existing_ok=existing_ok)


ROOT = Path(os.environ["JAROMAILDIR"])
LIMIT = native.MAX_ITEMS


class Rejected(Exception):
    pass


def bounded(rows):
    result = []
    for row in rows:
        if len(result) == LIMIT:
            raise Rejected("limit")
        result.append(row)
    return result


def boxes_for(request):
    boxes = request.get("folders")
    if request["action"] == "legacy" and request.get("lastlog") is True:
        provider = shutil.which("maildirs_lastlog")
        if not provider:
            raise Rejected("lastlog_unavailable")
        result = subprocess.run([provider], capture_output=True, timeout=10)
        if result.returncode or len(result.stdout) > 65536 or not result.stdout.strip():
            raise Rejected("lastlog_unavailable")
        boxes = [os.fsdecode(line) for line in result.stdout.splitlines()]
    if not isinstance(boxes, list) or not 0 < len(boxes) <= 32 or len(set(boxes)) != len(boxes):
        raise Rejected("invalid_request")
    for box in boxes:
        ordinary(box)
    return boxes


def exact_pairs(root, boxes, executable):
    # Native dry-run paths are double-quoted, with every quote, backslash,
    # non-ASCII byte and control byte escaped as a hexadecimal byte.
    cmd = [executable, "-n", "dedup", "--", *boxes]
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
        try:
            report = proc.stdout.read((8 << 20) + 1)
            if len(report) > 8 << 20:
                raise Rejected("limit")
            if proc.wait():
                raise Rejected("backend_failure")
        finally:
            if proc.poll() is None:
                proc.kill()
    def occurrence(encoded):
        path = re.sub(rb"\\x([0-9a-f]{2})", lambda m: bytes.fromhex(m[1].decode()), encoded)
        row = native.lookup(root, ROOT, path)
        if row["folder"] not in boxes:
            raise Rejected("invalid_resource")
        return row
    pairs = []
    for line in report.splitlines():
        match = re.fullmatch(rb'keep "([^"]+)" remove "([^"]+)"', line)
        if not match:
            raise Rejected("invalid_response")
        pairs.append((occurrence(match[1]), occurrence(match[2])))
        if len(pairs) > LIMIT:
            raise Rejected("limit")
    return pairs


def index_revision():
    cmd = ["notmuch", "--config=" + str(ROOT / "notmuch-config")]
    if not shutil.which("notmuch"):
        raise Rejected("index_unavailable")
    root = subprocess.run([*cmd, "config", "get", "database.mail_root"], capture_output=True)
    state = subprocess.run([*cmd, "count", "--exclude=false", "--lastmod", "--", "*"], capture_output=True)
    if root.returncode or root.stdout.rstrip(b"\n") != os.fsencode(ROOT) or state.returncode:
        raise Rejected("index_unavailable")
    return state.stdout.decode("ascii").strip()


def selection(root, r, executable):
    action, destination = r["action"], r.get("destination")
    boxes, items, keepers = [], [], []
    generation = None
    if action in ("archive", "merge"):
        ordinary(destination)
        with native.folder(root, destination):
            pass
    if action == "archive":
        query = r.get("query")
        if not isinstance(query, str) or not query.strip() or len(query.encode()) > 4096 or "\0" in query:
            raise Rejected("invalid_query")
        generation = index_revision()
        items = bounded(native.backup(root, ROOT, query, destination))
        if generation != index_revision():
            raise Rejected("stale_maintenance")
        boxes = sorted({item["folder"] for item in items})
    else:
        boxes = boxes_for(r)
        if action == "merge":
            if len(boxes) != 1 or boxes[0] in (".", destination) or destination.startswith(boxes[0] + "/"):
                raise Rejected("invalid_request")
            if r.get("remove") and boxes[0] in ("incoming", "known", "sent"):
                raise Rejected("invalid_request")
            items = bounded(native.messages(root, boxes[0]))
        elif action == "legacy":
            if not shutil.which("formail"):
                raise Rejected("formail_unavailable")
            items = bounded(native.duplicates(root, boxes))
        elif action == "exact":
            pairs = exact_pairs(root, boxes, executable)
            keepers = [keeper for keeper, _ in pairs]
            items = [loser for _, loser in pairs]
        else:
            raise Rejected("invalid_request")
    scope = list(dict.fromkeys([*boxes, *([destination] if destination is not None else [])]))
    stamps = []
    for box in scope:
        ordinary(box)
        with native.folder(root, box) as fd:
            # Bind directory identities and arrivals/renames, including an
            # empty destination or merge source. Ignore reads and atime.
            for part in (".", "new", "cur", "tmp"):
                info = os.stat(part, dir_fd=fd, follow_symlinks=False)
                stamps.append([info.st_dev, info.st_ino, info.st_mtime_ns])
    return boxes, items, keepers, scope, stamps, generation


def digest(request, plan):
    keys = ("action", "folders", "lastlog", "destination", "query", "remove")
    content = [{key: request.get(key) for key in keys}, plan]
    return hashlib.sha256(json.dumps(content, sort_keys=True).encode()).hexdigest()


def lock_boxes(root, scope, stack):
    for box in sorted(scope):
        fd = stack.enter_context(native.folder(root, box))
        for part in (".", "cur", "new"):
            handle = os.open(part, native.DIR, dir_fd=fd)
            stack.callback(os.close, handle)
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)


def present(row):
    return {key: row[key] for key in ("folder", "part", "name")}


def remove_exact(root, winner, duplicate, changed_links):
    # The native dry run classified these paths before we resolved their
    # occurrence revisions. Check the two opened files again before removal.
    with ExitStack() as stack:
        streams, selected = [], []
        for row in (winner, duplicate):
            box = stack.enter_context(native.folder(root, row["folder"]))
            part = os.open(row["part"], native.DIR, dir_fd=box)
            stack.callback(os.close, part)
            file = stack.enter_context(os.fdopen(os.open(row["name"], native.FILE, dir_fd=part), "rb"))
            expected = changed_links.get(row["revision"], row["revision"])
            if native.revision(os.fstat(file.fileno())) != expected:
                raise ValueError("occurrence changed")
            selected.append((part, row, expected))
            streams.append(file)
        while True:
            chunk = streams[0].read(1 << 20)
            if chunk != streams[1].read(1 << 20):
                raise ValueError("message bytes changed")
            if not chunk:
                break
        for part, row, expected in selected:
            current = native.occurrence(part, row["folder"], row["part"], row["name"])
            if current["revision"] != expected:
                raise ValueError("occurrence changed")
        before = os.fstat(streams[1].fileno())
        outcome = native.transfer(root, {**duplicate, "revision": selected[1][2]}, None, remove=True)
        if before.st_nlink > 1:
            after = os.fstat(streams[1].fileno())
            if native.signature(after) != native.signature(before) or after.st_nlink != before.st_nlink - 1:
                raise ValueError("linked occurrence changed")
            changed_links[duplicate["revision"]] = native.revision(after)
        return outcome


def operate(root, request, executable):
    plan = selection(root, request, executable)
    revision = digest(request, plan)
    boxes, items, keepers, scope, _, _ = plan
    if request["op"] == "preview":
        rows = [dict(present(row), **({"keep": present(keepers[i])} if keepers else {}))
                for i, row in enumerate(items)]
        result = dict(revision=revision, rows=rows, folders=boxes)
        if request["action"] == "archive" and items:
            first = subprocess.run(["notmuch", "--config=" + str(ROOT / "notmuch-config"), "search",
                                    "--output=messages", "--limit=1", "--format=text0", "--", request["query"]],
                                   check=True, stdout=subprocess.PIPE)
            if index_revision() != plan[-1]:
                raise Rejected("stale_maintenance")
            if first.stdout:
                result["indexed"] = True
                result["rows"] = items
        if len(json.dumps(result)) > (8 << 20) - 1024:
            raise Rejected("limit")
        return result
    if request.get("expect") != revision:
        raise Rejected("stale_maintenance")
    action = request["action"]
    if (action in ("exact", "legacy") or request.get("remove")) and request.get("paused") is not True:
        raise Rejected("pause_writers")
    outcomes, removed = [], False
    with ExitStack() as stack:
        lock_boxes(root, scope, stack)
        if digest(request, selection(root, request, executable)) != revision:
            raise Rejected("stale_maintenance")
        changed_links = {}
        for i, row in enumerate(items):
            try:
                if action == "exact":
                    outcome = remove_exact(root, keepers[i], row, changed_links)
                else:
                    outcome = native.transfer(root, row, request.get("destination"), remove=action == "legacy")
            except FileExistsError:
                outcome = "conflict"
            except (ValueError, OSError):
                outcome = "check"
            outcomes.append(outcome)
        if action == "merge" and request.get("remove") and all(o == "moved" for o in outcomes):
            try:
                native.empty_source(root, boxes[0])
                removed = True
            except (ValueError, OSError):
                pass
    return {"outcomes": outcomes, "removed": removed}


def deliver(root, request):
    remember = request["op"] == "remember"
    box = "remember" if remember else request["destination"]
    ordinary(box)
    with ExitStack() as stack:
        if remember:
            row = request["occurrence"]
            ordinary(row["folder"])
            source = stack.enter_context(native.folder(root, row["folder"]))
            if not matches(source, row):
                raise Rejected("invalid_resource")
            part = os.open(row["part"], native.DIR, dir_fd=source)
            stack.callback(os.close, part)
            payload = stack.enter_context(tempfile.TemporaryFile())
            with os.fdopen(os.open(row["name"], native.FILE, dir_fd=part), "rb") as stream:
                if native.revision(os.fstat(stream.fileno())) != row["revision"]:
                    raise Rejected("invalid_resource")
                shutil.copyfileobj(stream, payload, 1 << 20)
                if not matches(source, row):
                    raise Rejected("invalid_resource")
            payload.seek(0)
            source_input = {"stdin": payload}
            create_folder(root, box, existing_ok=True)
        else:
            data = base64.b64decode(request["data"], validate=True)
            if not data or len(data) > 16 << 20:
                raise Rejected("invalid_request")
            source_input = {"input": data}
        dest = stack.enter_context(native.folder(root, box))
        lock_boxes(root, [box], stack)
        # Pin the validated destination while using the same delivery function
        # as both installed CLI commands, including its mblaze fallback.
        script = ('source "$JAROWORKDIR/zlibs/bootstrap" source || exit 1; '
                  'source "$JAROWORKDIR/zlibs/maildirs" || exit 1; '
                  'MAILDIRS="/proc/self/fd/$1"; deliver "."')
        subprocess.run(["zsh", "-fc", script, "webmail-deliver", str(dest)], **source_input,
                       pass_fds=(dest,), stdout=subprocess.DEVNULL, check=True)
        return {"destination": box, "count": 1}


def main():
    request = json.load(sys.stdin)
    root = os.open(ROOT, native.DIR)
    try:
        if request["op"] in ("remember", "deliver"):
            return deliver(root, request)
        return operate(root, request, sys.argv[1])
    finally:
        os.close(root)


if __name__ == "__main__":
    try:
        print(json.dumps(main()))
    except BlockingIOError:
        print(json.dumps({"error": "busy"}))
    except Rejected as e:
        print(json.dumps({"error": str(e)}))
    except (KeyError, OSError, TypeError, ValueError, subprocess.SubprocessError):
        print(json.dumps({"error": "maintenance_failed"}))
