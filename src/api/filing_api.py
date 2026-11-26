"""Apply revision-checked Maildir operations shared by native clients."""

import os
import subprocess

import operations as native
from profile_api import ProfileApiError


METHODS = ("maildir.file",)
ACTIONS = ("move", "archive", "trash", "read", "unread", "flag", "unflag")


def operation(workdir, root_path, params):
    action = params.get("action")
    items = params.get("items")
    destination = params.get("destination")
    if action not in ACTIONS or not isinstance(items, list) or not 1 <= len(items) <= 100:
        raise ProfileApiError("invalid_request")
    moving = action in ("move", "archive", "trash")
    if (moving and not isinstance(destination, str)) or (not moving and destination is not None):
        raise ProfileApiError("invalid_request")
    for row in items:
        if not isinstance(row, dict) or not isinstance(row.get("folder"), str) or row.get("part") not in ("cur", "new"):
            raise ProfileApiError("invalid_resource")
        if not native.component(row.get("name")) or not isinstance(row.get("revision"), str):
            raise ProfileApiError("invalid_resource")
        directory = row.get("directory")
        if not isinstance(directory, list) or len(directory) != 2 or not all(
            isinstance(value, str) and value.isascii() and value.isdecimal() for value in directory
        ):
            raise ProfileApiError("invalid_resource")
        ordinary(row["folder"])
    if destination is not None:
        ordinary(destination)
    return execute(root_path, workdir, params)


def ordinary(name):
    parts = native.folder_parts(name)
    if parts and parts[0] == "outbox":
        raise ValueError("the sending queue has its own operations")
    return parts


def matches(source, item):
    parent = os.fstat(source)
    if [str(parent.st_dev), str(parent.st_ino)] != item["directory"]:
        return False
    bucket = os.open(item["part"], native.DIR, dir_fd=source)
    try:
        current = native.occurrence(bucket, item["folder"], item["part"], item["name"])
        return current["revision"] == item["revision"]
    finally:
        os.close(bucket)


def apply(root, action, destination, item):
    box = item["folder"]
    ordinary(box)
    # Validate the selected Maildir as well as the native occurrence revision.
    # A hard link in a replacement directory can have the same file revision.
    with native.folder(root, box) as src:
        if not matches(src, item):
            return "stale"
        if action in ("read", "unread", "flag", "unflag"):
            suffix = item["name"].partition(":2,")[2].partition(",")[0]
            flags = set(suffix) & native.STANDARD_FLAGS
            change = "S" if action in ("read", "unread") else "F"
            if action in ("read", "flag"):
                flags.add(change)
            else:
                flags.discard(change)
            arguments = src, {**item, "folder": "."}, ".", "".join(sorted(flags))
        else:
            arguments = root, item, destination
        try:
            return native.transfer(*arguments)
        except FileNotFoundError:
            # A race after preflight may follow publication of the destination.
            return "check"


def execute(root_path, workdir, request):
    root = os.open(root_path, native.DIR)
    try:
        if request["action"] == "create":
            native.create_folder(root, request["destination"], workdir)
            return {"created": True}
        action = request["action"]
        destination = request["destination"]
        if destination is not None:
            ordinary(destination)
            try:
                if action in ("archive", "trash"):
                    native.create_folder(root, destination, workdir, existing_ok=True)
                with native.folder(root, destination):
                    pass
            except (OSError, ValueError, subprocess.SubprocessError):
                return {"outcomes": ["destination"] * len(request["items"])}
        outcomes = []
        for item in request["items"]:
            try:
                status = apply(root, action, destination, item)
            except FileNotFoundError:
                status = "stale"
            except FileExistsError:
                # A collision restoring a racing replacement can also raise
                # EEXIST after publication. Promise retention only if verified.
                try:
                    with native.folder(root, item["folder"]) as source:
                        status = "conflict" if matches(source, item) else "check"
                except (OSError, ValueError):
                    status = "check"
            except (OSError, ValueError, KeyError):
                # The native transfer checks again at publication and removal.
                # Its failure may follow a published copy: report uncertainty.
                status = "check"
            outcomes.append(status)
        return {"outcomes": outcomes}
    finally:
        os.close(root)
