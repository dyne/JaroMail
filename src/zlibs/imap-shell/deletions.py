"""Exact, consented mailbox deletion; no message or subtree emulation."""

import imaplib
from dataclasses import dataclass

from folders import FolderTree, WriteRejected, display_component, mailbox_text
import headers
from renames import confirm, identity
from rename_preview import within


class DeleteError(ValueError):
    pass


def options(words):
    flags, path = set(), None
    for word in words:
        if word in ("--yes", "--recursive") and word not in flags:
            flags.add(word)
        elif word.startswith("--") or path is not None:
            raise DeleteError("Use rmdir EXACT-PATH [--recursive] [--yes]")
        else:
            path = word
    if path is None:
        raise DeleteError("Use rmdir EXACT-PATH [--recursive] [--yes]")
    return path, "--yes" in flags, "--recursive" in flags


def snapshot(tree, path):
    # Use an independent fresh tree so relative cwd cannot retarget a consent.
    fresh = FolderTree(tree.client, tree.seconds, tree.deadline)
    source = fresh.resolve_write(path)
    if source is fresh.root or source.raw.upper() == b"INBOX":
        raise DeleteError("Cannot delete root or INBOX")
    rows = fresh.name_query("LIST", source.raw + b"*")
    found = False
    boundary = source.raw if source.delimiter and source.raw.endswith(source.delimiter) else source.raw + (source.delimiter or b"")
    for raw, delimiter, flags in rows:
        if b"\\NONEXISTENT" in (flag.upper() for flag in flags):
            continue
        if raw == source.raw:
            if (raw, delimiter, tuple(sorted(flag.upper() for flag in flags))) != identity(source):
                raise DeleteError("Delete target identity changed; retry rmdir")
            found = True
        elif source.delimiter is not None and raw.startswith(boundary):
            raise DeleteError("Folder has descendants; single-folder deletion refused")
    if not found or b"\\HASCHILDREN" in (flag.upper() for flag in source.flags):
        raise DeleteError("Folder scope is incomplete or has children; deletion refused")
    count, generation = None, None
    if source.selectable:
        try:
            with fresh.deadline(fresh.seconds):
                count = headers.examine(fresh.client, source.raw)
                generation = headers.response_number(fresh.client, "UIDVALIDITY")
        except imaplib.IMAP4.abort:
            raise
        except (ValueError, imaplib.IMAP4.error):
            pass
    return source, (count, generation)


def remove(tree, words):
    path, yes, recursive = options(words)
    if recursive:
        remove_recursive(tree, path, yes)
        return
    # Resolve the user's input once; every later observation uses this absolute
    # display path and must retain the exact original raw mailbox identity.
    source = tree.resolve_write(path)
    source, metadata = snapshot(tree, source.path)
    count, _ = metadata
    print("Delete target: " + source.path, flush=True)
    print("Messages: " + (str(count) if count is not None else "unknown"), flush=True)
    if count is None:
        print("WARNING: message count unknown; DELETE may remove messages", flush=True)
    elif count:
        print("WARNING: nonempty folder; DELETE removes all messages", flush=True)
    try:
        if not yes and not confirm(None, 1, "Delete " + source.path + "? [no] Type yes: "):
            print("Deletion cancelled", flush=True)
            return
    except OSError:
        print("Deletion cancelled: controlling terminal required; command stdin cannot confirm", flush=True)
        return
    except KeyboardInterrupt:
        print("Deletion cancelled", flush=True)
        return

    def recheck():
        refreshed, current_metadata = snapshot(tree, source.path)
        if (identity(refreshed) != identity(source) or identity(refreshed.parent) != identity(source.parent)
                or current_metadata != metadata):
            raise DeleteError("Delete target or message state changed; retry rmdir")

    parent = tree.root if source.parent.parent is None else source.parent
    tree.mutate("DELETE", None, parent, source.raw, source.delimiter, preflight=recheck)


@dataclass
class Target:
    folder: object
    metadata: tuple


def stable_identity(folder):
    # These two flags describe the changing child set, not mailbox identity.
    return folder.raw, folder.delimiter, tuple(sorted(flag.upper() for flag in folder.flags
        if flag.upper() not in (b"\\HASCHILDREN", b"\\HASNOCHILDREN")))


def listed(tree, source):
    result = {}
    for raw, delimiter, flags in tree.name_query("LIST", source.raw + b"*"):
        if not within(raw, source.raw, source.delimiter):
            continue
        if delimiter != source.delimiter or raw in result:
            raise DeleteError("Recursive scope identity is ambiguous; deletion refused")
        result[raw] = delimiter, flags
    return result


def recursive_snapshot(tree, source):
    fresh = FolderTree(tree.client, tree.seconds, tree.deadline)
    rows = listed(fresh, source)
    if not rows:
        return {}
    if source.raw not in rows:
        raise DeleteError("Recursive scope is incomplete; deletion refused")
    root = fresh.resolve_write(source.path)
    targets, pending = {}, [root]
    while pending:
        folder = pending.pop()
        if folder.raw.upper() == b"INBOX":
            raise DeleteError("Cannot delete root or INBOX")
        if folder.raw in targets or rows.get(folder.raw) != (folder.delimiter, folder.flags):
            raise DeleteError("Recursive scope changed during enumeration; deletion refused")
        if b"\\NONEXISTENT" in (flag.upper() for flag in folder.flags):
            raise DeleteError("Recursive scope contains an unavailable folder; deletion refused")
        children = fresh.children(folder, refresh=True)
        if not children and b"\\HASCHILDREN" in (flag.upper() for flag in folder.flags):
            raise DeleteError("Recursive child scope is incomplete; deletion refused")
        metadata = (None, None)
        if folder.selectable:
            try:
                with fresh.deadline(fresh.seconds):
                    count = headers.examine(fresh.client, folder.raw)
                    metadata = count, headers.response_number(fresh.client, "UIDVALIDITY")
            except imaplib.IMAP4.abort:
                raise
            except (ValueError, imaplib.IMAP4.error):
                raise DeleteError("Recursive target inaccessible; deletion refused") from None
        targets[folder.raw] = Target(folder, metadata)
        pending.extend(children.values())
    if set(targets) != set(rows):
        raise DeleteError("Recursive scope is incomplete or changed; deletion refused")
    return targets


def observed_path(source, raw, utf8):
    if raw == source.raw:
        return source.path
    boundary = source.raw if source.raw.endswith(source.delimiter) else source.raw + source.delimiter
    suffix = raw[len(boundary):]
    if suffix.endswith(source.delimiter):
        suffix = suffix[:-len(source.delimiter)]
    return source.path.rstrip("/") + "/" + "/".join(
        display_component(mailbox_text(part, utf8)) for part in suffix.split(source.delimiter))


def report(tree, source, targets, states, reason, encoding):
    print("Recursive deletion stopped: " + reason, flush=True)
    try:
        rows = listed(tree, source) if tree.client.utf8_enabled == encoding else None
    except (ValueError, OSError, imaplib.IMAP4.error, KeyboardInterrupt):
        rows = None
    for raw, target in sorted(targets.items(), key=lambda item: item[1].folder.path):
        state = states[raw]
        observed = "unavailable" if rows is None else "present" if raw in rows else "not listed"
        if state == "unknown":
            label = "unknown"
        elif observed == "present":
            label = "surviving"
        elif observed == "unavailable":
            label = "unknown"
        else:
            label = "removed" if state == "confirmed" else "absent"
        print(label + "\t" + target.folder.path + "\t" + state + "; observed " + observed)
    if rows is not None:
        for raw in sorted(set(rows) - set(targets)):
            print("surviving\t" + observed_path(source, raw, encoding) + "\toutside confirmed scope")
    print("Review surviving paths before a fresh rmdir; no automatic replay", flush=True)


def remove_recursive(tree, path, yes):
    source = tree.resolve_write(path)
    if source is tree.root or source.raw.upper() == b"INBOX":
        raise DeleteError("Cannot delete root or INBOX")
    encoding = tree.client.utf8_enabled
    targets = recursive_snapshot(tree, source)
    if source.raw not in targets:
        raise DeleteError("Recursive target disappeared; deletion refused")
    source = targets[source.raw].folder
    ordered = sorted(targets, key=lambda raw: (-targets[raw].folder.path.count("/"), targets[raw].folder.path))
    print(f"Recursive delete targets: {len(ordered)}", flush=True)
    for raw in ordered:
        target = targets[raw]
        count = target.metadata[0]
        print("target\t" + target.folder.path + "\tmessages: " + (str(count) if count is not None else "unknown"))
    print("WARNING: DELETE removes messages; unknown counts may hide messages", flush=True)
    states, reason = dict.fromkeys(targets, "pending"), "finished"

    def recheck():
        if tree.client.utf8_enabled != encoding:
            raise DeleteError("Mailbox encoding changed; recursive deletion stopped")
        refreshed = recursive_snapshot(tree, source)
        expected = {raw for raw, state in states.items() if state == "pending"}
        missing = expected - set(refreshed)
        for raw in missing:
            folder = targets[raw].folder
            descendants = [name for name in targets if name != raw and within(name, raw, folder.delimiter)]
            if (folder.selectable or not descendants or any(states[name] not in ("confirmed", "container disappeared") and
                    not (name in missing and not targets[name].folder.selectable) for name in descendants)):
                raise DeleteError("Recursive scope drift; deletion stopped")
        if set(refreshed) != expected - missing:
            raise DeleteError("Recursive scope drift; deletion stopped")
        for raw, target in refreshed.items():
            previous = targets[raw]
            if (stable_identity(target.folder) != stable_identity(previous.folder)
                    or stable_identity(target.folder.parent) != stable_identity(previous.folder.parent)
                    or target.metadata != previous.metadata):
                raise DeleteError("Recursive target or message state drift; deletion stopped")
        for raw in missing:
            states[raw] = "container disappeared"

    class ContainerGone(Exception):
        pass

    try:
        try:
            if not yes and not confirm(None, len(ordered),
                    f"Delete {source.path} recursively ({len(ordered)} listed folders)? [no] Type yes: "):
                reason = "confirmation cancelled"
                return
        except OSError:
            reason = "controlling terminal required; command stdin cannot confirm"
            return
        except KeyboardInterrupt:
            reason = "confirmation cancelled"
            return
        for raw in ordered:
            if states[raw] != "pending":
                continue
            folder = targets[raw].folder

            def fence():
                recheck()
                if states[raw] != "pending":
                    raise ContainerGone()

            def outcome(value):
                states[raw] = value

            parent = tree.root if folder.parent.parent is None else folder.parent
            try:
                result = tree.mutate("DELETE", None, parent, folder.raw, folder.delimiter,
                                     preflight=fence, on_outcome=outcome)
            except ContainerGone:
                continue
            except WriteRejected:
                reason = "server refusal"
                break
            except DeleteError as exc:
                reason = str(exc)
                break
            except ValueError:
                reason = "recursive scope unavailable; deletion stopped"
                break
            if result != "confirmed":
                reason = "unknown outcome; not replayed"
                break
    except BaseException:
        reason = "interrupted or connection unavailable; no automatic replay"
        raise
    finally:
        report(tree, source, targets, states, reason, encoding)
        current_path = tree.current.path
        if tree.restore_navigation(current_path, (tree.current.raw, tree.current.delimiter)):
            print("Current folder unavailable; returned to " + tree.current.path, flush=True)
