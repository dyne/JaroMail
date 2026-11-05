#!/usr/bin/env python3
"""Verify the exact minimal source extraction without network access."""

import hashlib
import json
from pathlib import Path, PurePosixPath


def verify(root):
    manifest = json.loads((root / "sources.json").read_text())
    if manifest["schema_version"] != 2:
        raise ValueError("unsupported source manifest version")
    directory = PurePosixPath(manifest["directory"])
    if directory.is_absolute() or ".." in directory.parts:
        raise ValueError("unsafe extraction directory")
    expected = {"README.md", "sources.json", "verify.py"}
    for entry in manifest["files"]:
        relative = PurePosixPath(entry["path"])
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise ValueError("unsafe upstream source path: " + entry["path"])
        name = (directory / relative).as_posix()
        if name in expected:
            raise ValueError("duplicate upstream source path: " + name)
        expected.add(name)

    actual = set()
    parents = {str(p) for name in expected for p in PurePosixPath(name).parents
               if str(p) != "."}
    for target in root.rglob("*"):
        name = target.relative_to(root).as_posix()
        if target.is_symlink():
            raise ValueError("unexpected symlink: " + name)
        if target.is_dir():
            if name not in parents:
                raise ValueError("unexpected vendor directory: " + name)
        elif target.is_file():
            actual.add(name)
        else:
            raise ValueError("unexpected vendor entry: " + name)
    if actual != expected:
        raise ValueError("extra or missing vendor files: " + repr(sorted(actual ^ expected)))

    for entry in manifest["files"]:
        target = root.joinpath(*directory.parts, *PurePosixPath(entry["path"]).parts)
        data = target.read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("upstream source differs: " + entry["path"])
    print("PASS: {} pinned upstream files, {} bytes; no extra vendor payload".format(
        len(manifest["files"]), sum(entry["size"] for entry in manifest["files"])))


if __name__ == "__main__":
    try:
        verify(Path(__file__).resolve().parent)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit("FAIL: " + str(error))
