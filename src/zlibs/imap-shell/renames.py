"""Session-local reviewed rename mappings and serial, fenced application."""

import os
import secrets
from urllib.parse import unquote_to_bytes

from folders import WriteRejected
from rename_preview import PreviewError, PreviewTree, build, within


def identity(folder):
    return folder.raw, folder.delimiter, tuple(sorted(flag.upper() for flag in folder.flags))


def mapping_key(pair):
    return (pair.source.path, identity(pair.source), identity(pair.source.parent),
            pair.target_path, identity(pair.parent), pair.raw, pair.delimiter, pair.members)


def matches(preview, component):
    text = unquote_to_bytes(component).decode("utf-8")
    return (text.startswith(preview.prefix) and text.endswith(preview.suffix)
            and len(text) >= len(preview.prefix) + len(preview.suffix))


def confirm(identifier, count, prompt=None):
    # Command stdin is never a confirmation source, even when it is a pipe.
    fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)
    with os.fdopen(fd, "rb", buffering=0) as terminal:
        if not os.isatty(terminal.fileno()):
            raise OSError("controlling terminal required")
        os.write(terminal.fileno(), (prompt or f"Apply {identifier}: rename {count} folder subtrees? Type yes: ").encode())
        answer, oversized = bytearray(), False
        while True:
            char = terminal.read(1)
            if not char or char == b"\n":
                return char == b"\n" and not oversized and answer == b"yes"
            if len(answer) < 16:
                answer.extend(char)
            else:
                oversized = True


def recheck(tree, preview, pair, expected_membership):
    if tree.client.utf8_enabled != preview.utf8:
        raise PreviewError("mailbox encoding changed")
    fresh = PreviewTree(tree.client, tree.seconds, tree.deadline)
    parent = fresh.resolve_write(preview.source_parent.path)
    if identity(parent) != identity(preview.source_parent):
        raise PreviewError("source parent identity changed")
    membership = {(folder.raw, folder.delimiter) for folder in fresh.children(parent).values()
                  if matches(preview, folder.component)}
    if membership != expected_membership:
        raise PreviewError("matching membership changed")
    source = fresh.resolve_write(pair.source.path)
    target_parent, raw, delimiter = fresh.destination(pair.target_path)
    if (identity(source) != identity(pair.source) or identity(target_parent) != identity(pair.parent)
            or raw != pair.raw or delimiter != pair.delimiter):
        raise PreviewError("source or destination identity changed")
    rows = fresh.name_query("LIST", source.raw + b"*")
    members = tuple(sorted({name for name, sep, flags in rows if sep == source.delimiter
                           and b"\\NONEXISTENT" not in (flag.upper() for flag in flags)
                           and within(name, source.raw, sep)}))
    if members != pair.members:
        raise PreviewError("source subtree membership changed")
    rows = fresh.name_query("LIST", raw + b"*")
    if any(within(name, raw, delimiter) and b"\\NONEXISTENT" not in (flag.upper() for flag in flags)
           for name, sep, flags in rows):
        raise PreviewError("destination subtree collision")


class Session:
    def __init__(self):
        self.prefix, self.counter, self.saved = secrets.token_hex(8), 0, None

    def preview(self, tree, source, template):
        self.saved = None
        preview = build(tree, source, template)
        self.counter += 1
        identifier = self.prefix + "-" + str(self.counter)
        self.saved = identifier, preview
        preview.show()
        print("Preview ID: " + identifier, flush=True)
        return not preview.blocked

    def apply(self, tree, identifier):
        saved, self.saved = self.saved, None
        if saved is None or identifier != saved[0]:
            print("Apply rejected: stale or unknown preview ID; create a fresh preview", flush=True)
            return False
        preview = saved[1]
        states = ["pending"] * len(preview.mappings)
        reason = "finished"
        print(f"Apply {identifier}: {len(states)} folder subtrees", flush=True)
        try:
            if preview.blocked or not states:
                reason = "preview is blocked or empty"
                return False
            try:
                accepted = confirm(identifier, len(states))
            except OSError:
                reason = "controlling terminal required; command stdin cannot confirm"
                return False
            except KeyboardInterrupt:
                reason = "confirmation cancelled"
                return False
            if not accepted:
                reason = "confirmation cancelled"
                return False
            if tree.client.utf8_enabled != preview.utf8:
                reason = "mailbox encoding changed"
                return False
            try:
                refreshed = build(tree, preview.source_pattern, preview.template)
                if (refreshed.blocked or identity(refreshed.source_parent) != identity(preview.source_parent)
                        or [mapping_key(pair) for pair in refreshed.mappings] !=
                           [mapping_key(pair) for pair in preview.mappings]):
                    reason = "preview drift; create a fresh preview"
                    return False
            except ValueError:
                reason = "preview drift; create a fresh preview"
                return False
            membership = {(pair.source.raw, pair.source.delimiter) for pair in preview.mappings}
            for index, pair in enumerate(preview.mappings):
                def outcome(value):
                    states[index] = {"confirmed": "completed", "rejected": "failed", "unknown": "unknown"}[value]
                try:
                    parent = tree.root if pair.parent.parent is None else pair.parent
                    result = tree.mutate("RENAME", pair.source, parent, pair.raw, pair.delimiter,
                                         preflight=lambda: recheck(tree, preview, pair, membership),
                                         on_outcome=outcome)
                except ValueError as exc:
                    reason = "server refusal" if isinstance(exc, WriteRejected) else "preflight drift"
                    break
                if result != "confirmed":
                    reason = "unknown outcome; not replayed"
                    break
                membership.remove((pair.source.raw, pair.source.delimiter))
                if identity(pair.parent) == identity(preview.source_parent) and matches(
                        preview, pair.target_path.rsplit("/", 1)[-1]):
                    membership.add((pair.raw, pair.delimiter))
        except BaseException:
            # The outcome callback records a tagged result before reconciliation
            # or navigation can fail. Unstarted pairs remain pending.
            reason = "apply interrupted; no automatic replay"
            raise
        finally:
            print("Apply stopped: " + reason)
            for pair, state in zip(preview.mappings, states):
                print(state + "\t" + pair.source.path + "\t->\t" + pair.target_path)
            print("Fresh preview required before another apply", flush=True)
        return reason == "finished"
