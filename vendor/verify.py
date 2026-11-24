#!/usr/bin/env python3
"""Verify the pinned Sieve, abook and SQLite source inputs offline."""

import hashlib
import json
from pathlib import Path, PurePosixPath
import re


def abook_payload(manifest):
    if manifest["schema_version"] != 2 or manifest["directory"] != "abook/upstream":
        raise ValueError("unsupported abook source manifest")
    upstream, fork = manifest["upstream"], manifest["fork"]
    if not re.fullmatch(re.escape(upstream["version"]) + r"\+jaromail\.[1-9][0-9]*", fork["version"]):
        raise ValueError("invalid abook fork version")
    if not re.fullmatch(r"[0-9a-f]{64}", upstream["sha256"]):
        raise ValueError("invalid abook release archive digest")
    commits = fork["adopted_upstream_commits"]
    if len(set(commits)) != len(commits) or any(
        not re.fullmatch(r"[0-9a-f]{40}", commit)
        for commit in (upstream["git_commit"], fork["reviewed_upstream_tip"], *commits)
    ):
        raise ValueError("invalid abook upstream commit list")

    pristine = {entry["path"]: entry for entry in manifest["files"]}
    modified = {}
    for entry in fork["modified_files"]:
        name = entry["path"]
        if name in modified or name not in pristine or name in {"COPYING", "AUTHORS"} or entry == pristine[name]:
            raise ValueError("invalid abook fork override: " + name)
        modified[name] = entry
    return dict(manifest, files=[modified.get(entry["path"], entry) for entry in manifest["files"]])


def verify(root):
    manifest = json.loads((root / "sources.json").read_text())
    if manifest["schema_version"] != 2:
        raise ValueError("unsupported source manifest version")
    abook = abook_payload(json.loads((root / "abook/sources.json").read_text()))
    sqlite = json.loads((root / "sqlite/sources.json").read_text())
    if sqlite["schema_version"] != 1 or sqlite["directory"] != "sqlite/upstream":
        raise ValueError("unsupported SQLite source manifest")
    expected = {"README.md", "sources.json", "verify.py", "abook/README.md", "abook/sources.json",
                "sqlite/README.md", "sqlite/sources.json"}
    entries = []
    for source in (manifest, manifest["dovecot_notices"], manifest["dovecot_maildir"], abook, sqlite):
        directory = PurePosixPath(source["directory"])
        if directory.is_absolute() or ".." in directory.parts:
            raise ValueError("unsafe extraction directory")
        for entry in source["files"]:
            relative = PurePosixPath(entry["path"])
            if relative.is_absolute() or ".." in relative.parts or not relative.parts:
                raise ValueError("unsafe upstream source path: " + entry["path"])
            name = (directory / relative).as_posix()
            if name in expected:
                raise ValueError("duplicate upstream source path: " + name)
            expected.add(name)
            entries.append((name, entry))

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

    for name, entry in entries:
        target = root / name
        data = target.read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("upstream source differs: " + entry["path"])
    print("PASS: {} pinned upstream files ({} Sieve, {} Dovecot notices, 1 Dovecot Maildir source, {} abook, {} SQLite), {} bytes; no extra vendor payload".format(
        len(entries), len(manifest["files"]), len(manifest["dovecot_notices"]["files"]), len(abook["files"]), len(sqlite["files"]), sum(entry["size"] for _, entry in entries)))
    print("abook fork {} ({} files modified from archive {})".format(
        abook["fork"]["version"], len(abook["fork"]["modified_files"]), abook["upstream"]["version"]))


if __name__ == "__main__":
    try:
        verify(Path(__file__).resolve().parent)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit("FAIL: " + str(error))
