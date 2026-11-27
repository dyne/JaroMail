"""Single-mailbox server totals and bounded, read-only size measurements."""

import imaplib
import re
import sys
import contextlib
from dataclasses import dataclass, field

from folders import quoted, read_quoted
import headers


class SizeError(ValueError):
    """Safe, locally authored operator guidance."""


def same_mailbox(left, right):
    return left == right or left.upper() == right.upper() == b"INBOX"


def rev2(client):
    # Dual advertisements remain rev1 in this shell, even with UTF8=ACCEPT.
    return "IMAP4REV2" in client.capabilities and "IMAP4REV1" not in client.capabilities


def supports_size(client):
    return "STATUS=SIZE" in client.capabilities or rev2(client)


def status_size(client, raw):
    if any(char in raw for char in (b"\r", b"\n", b"\0")):
        # imaplib can only place a literal at the end of a command, whereas
        # STATUS requires its attribute list after the mailbox. EXAMINE can
        # represent these names safely and supplies the metadata fallback.
        return None
    mailbox = quoted(raw)
    try:
        status, data = client.status(mailbox, "(SIZE)")
    except imaplib.IMAP4.abort:
        raise
    except imaplib.IMAP4.error:
        return None
    if status != "OK":
        return None
    return next((size for name, size in status_values(data) if same_mailbox(name, raw)), None)


def status_values(data):
    # STATUS may contain unrelated unsolicited replies. Match raw identity,
    # including literal names, before accepting its attribute list.
    pending = None
    for item in data:
        if isinstance(item, tuple):
            prefix, name = item
            if re.fullmatch(rb"\{\d+\}", prefix):
                pending = name
            continue
        if not isinstance(item, bytes):
            continue
        try:
            if pending is not None:
                name, tail, pending = pending, item.strip(), None
            elif item.startswith(b'"'):
                name, end = read_quoted(item, 0)
                tail = item[end:].strip()
            else:
                name, _, tail = item.partition(b" ")
            if not re.fullmatch(rb"\((?:[A-Za-z]+ \d+ ?)*\)", tail):
                continue
            pairs = tail[1:-1].split()
            values = [int(pairs[i + 1]) for i in range(0, len(pairs), 2)
                      if pairs[i].upper() == b"SIZE"]
            if len(values) == 1 and values[0] <= 9223372036854775807:
                yield name, values[0]
        except ValueError:
            continue


def inventory(client, raw):
    validity, upper = None, None
    for attempt in range(3):
        count = headers.examine(client, raw)
        generation = headers.response_number(client, "UIDVALIDITY")
        if generation is None or (validity is not None and validity != generation):
            raise SizeError("Mailbox generation unavailable or changed; retry du")
        validity = generation
        client.untagged_responses.pop("EXPUNGE", None)
        try:
            if upper is None:
                next_uid = headers.response_number(client, "UIDNEXT")
                if next_uid is not None:
                    upper = next_uid - 1
                else:
                    tail = headers.sequence_uids(client, count, count) if count else []
                    if count and len(tail) != 1:
                        raise headers.MailboxChanged()
                    upper = tail[0][1] if tail else 0
            captured = []
            for start in range(1, count + 1, headers.BATCH_SIZE):
                end = min(start + headers.BATCH_SIZE - 1, count)
                rows = sorted(headers.sequence_uids(client, start, end))
                if [position for position, _ in rows] != list(range(start, end + 1)):
                    raise headers.MailboxChanged()
                captured.extend(uid for _, uid in rows if uid <= upper)
            if (len(captured) != len(set(captured)) or captured != sorted(captured)
                    or any(not 1 <= uid <= 4294967295 for uid in captured)):
                raise SizeError("Invalid UID inventory; retry du")
            return captured
        except headers.MailboxChanged:
            if attempt == 2:
                raise SizeError("Mailbox changed repeatedly; retry du")


def fetch_sizes(client, uids):
    original_read = client.read
    def no_literal(size):
        raise headers.LiteralLimit("unexpected literal in size metadata")
    client.read = no_literal
    try:
        try:
            data = headers.request(client, client.uid, "FETCH", ",".join(map(str, uids)), "(UID RFC822.SIZE)")
        except ValueError:
            return {}  # Tagged NO leaves this captured batch unmeasured.
    finally:
        client.read = original_read
    values = {}
    invalid = set()
    for item in data:
        if not isinstance(item, bytes):
            continue
        uid = re.search(rb"\bUID (\d+)\b", item, re.I)
        size = re.search(rb"\bRFC822.SIZE (\d+)\b", item, re.I)
        if uid is None or size is None:
            continue
        uid = int(uid[1])
        if uid in values:
            invalid.add(uid)
        if uid in uids:
            values[uid] = int(size[1])
    return {uid: value for uid, value in values.items() if uid not in invalid}


@dataclass
class Measurement:
    total: object = None
    source: str = "unknown"
    complete: bool = False
    measured: int = 0
    captured: int = 0

    @property
    def coverage(self):
        if self.source == "measured":
            state = "complete" if self.complete else "partial"
            return f"{state}; {self.measured}/{self.captured} messages"
        return "complete" if self.complete else "unknown"


def measure(client, folder, seconds, deadline, exact=False, reported=None):
    if not folder.selectable:
        raise SizeError("Choose a selectable folder for du")
    with deadline(seconds):
        if not exact and supports_size(client):
            total = reported if reported is not None else status_size(client, folder.raw)
            if total is not None:
                return Measurement(total, "server-reported", True)
        try:
            captured = inventory(client, folder.raw)
        except ValueError as exc:
            if isinstance(exc, SizeError):
                raise
            raise SizeError("Folder inaccessible or size inventory unavailable; retry du") from None
        total, measured = 0, 0
        for start in range(0, len(captured), headers.BATCH_SIZE):
            values = fetch_sizes(client, captured[start:start + headers.BATCH_SIZE])
            total += sum(values.values())
            measured += len(values)
        client.untagged_responses.pop("EXPUNGE", None)
    return Measurement(total, "measured", measured == len(captured), measured, len(captured))


def show(client, folder, seconds, deadline, exact=False):
    result = measure(client, folder, seconds, deadline, exact)
    label = "server-reported" if result.source == "server-reported" else f"measured for UID inventory; {result.coverage}"
    print(f"{folder.path}\t{result.total} bytes\t{label}")


def options(words):
    path, exact, recursive, depth, top = None, False, False, None, None
    seen = set()
    pending = iter(words)
    for word in pending:
        if word.startswith("--"):
            if word in seen:
                raise SizeError("Invalid du options; use help")
            seen.add(word)
            if word == "--exact":
                exact = True
            elif word == "--recursive":
                recursive = True
            elif word in ("--depth", "--top"):
                value = next(pending, "")
                if len(value) > 9 or not value.isascii() or not value.isdecimal() or (word == "--top" and int(value) == 0):
                    raise SizeError("Invalid du options; --depth needs a nonnegative integer and --top a positive integer")
                if word == "--depth":
                    depth = int(value)
                else:
                    top = int(value)
                recursive = True
            else:
                raise SizeError("Invalid du options; use help")
        elif path is None:
            path = word
        else:
            raise SizeError("Invalid du options; use help")
    return path, exact, recursive, depth, top


@dataclass
class ScanRow:
    folder: object
    result: Measurement = field(default_factory=Measurement)
    children: list = field(default_factory=list)
    extent: bool = False
    reported: object = None


def identity(folder):
    return mailbox_key(folder.raw)


def mailbox_key(raw):
    return b"INBOX" if raw.upper() == b"INBOX" else raw


def show_tree(tree, folder, exact=False, depth=None, top=None):
    rows = {identity(folder): ScanRow(folder)}
    pending = [(identity(folder), 0)]
    interrupted, completed, scanned = False, False, 0
    use_list_status = not exact and supports_size(tree.client) and (
        "LIST-STATUS" in tree.client.capabilities or rev2(tree.client))
    try:
        while pending:
            key, level = pending.pop()
            row = rows[key]
            boundary = depth is not None and level >= depth
            row.extent = boundary or (row.folder is not tree.root and row.folder.delimiter is None)
            if row.folder.selectable:
                try:
                    row.result = measure(tree.client, row.folder, tree.seconds, tree.deadline, exact, row.reported)
                except (ValueError, UnicodeError):
                    row.result = Measurement()
                scanned += 1
                print(f"du: scanned {scanned} folders so far", file=sys.stderr, flush=True)
            if boundary:
                continue
            try:
                children = tree.children(row.folder, refresh=True, status_sizes=use_list_status)
            except imaplib.IMAP4.abort:
                raise
            except (ValueError, UnicodeError, imaplib.IMAP4.error):
                print(f"du: descendants unavailable for {row.folder.path}", file=sys.stderr)
                continue
            reports = {}
            for raw, size in status_values(tree.latest_status):
                reports.setdefault(mailbox_key(raw), size)
            for child in children.values():
                child_key = identity(child)
                if child_key in rows:
                    continue  # Each raw mailbox contributes once, including INBOX aliases.
                reported = reports.get(child_key)
                extent = child.delimiter is None or (depth is not None and level + 1 >= depth)
                rows[child_key] = ScanRow(child, extent=extent, reported=reported)
                row.children.append(child_key)
            row.extent = True
            pending.extend((child, level + 1) for child in reversed(row.children))
        completed = True
    except BaseException as exc:
        interrupted = isinstance(exc, (KeyboardInterrupt, headers.LiteralLimit))
        # Any abnormal network exit may leave an unfinished response. Close
        # before rendering or outer LOGOUT/recovery can touch that stream.
        with contextlib.suppress(OSError):
            tree.client.shutdown()
        raise
    finally:
        # Every discovered but unscanned row remains unknown. A partial LIST
        # retains extent=False, so undiscovered descendants cannot look empty.
        print_tree(rows, identity(folder), depth, top, interrupted, not completed)
        print(f"du: scanned {scanned} folders; {'finished' if completed else 'incomplete'}", file=sys.stderr, flush=True)


def print_tree(rows, root, depth, top, interrupted, incomplete):
    aggregates = {}
    for key, row in reversed(list(rows.items())):
        selectable = row.folder.selectable
        result = row.result
        total = (result.total or 0) if selectable else 0
        complete = int(selectable and result.complete)
        folders = int(selectable)
        sources = {result.source} if selectable else set()
        measured, captured = result.measured, result.captured
        metadata_folders = int(selectable and result.source == "measured")
        extent = row.extent
        for child in row.children:
            values = aggregates[child]
            total += values[0]
            complete += values[1]
            folders += values[2]
            sources.update(values[3])
            measured += values[4]
            captured += values[5]
            metadata_folders += values[6]
            extent = extent and values[7]
        aggregates[key] = total, complete, folders, sources, measured, captured, metadata_folders, extent
    print("Mailbox bytes; shared/labeled copies may count twice. Subtree totals cover the requested depth; partial totals are lower bounds.")
    if interrupted:
        print("Scan interrupted; incomplete results retained.")
    elif incomplete:
        print("Scan failed; incomplete results retained.")
    print("PATH\tDIRECT_BYTES\tSUBTREE_BYTES\tDIRECT_SOURCE\tDIRECT_COVERAGE\tSUBTREE_SOURCE\tSUBTREE_COVERAGE")
    def aggregate_labels(key):
        total, complete, folders, sources, measured, captured, metadata_folders, extent = aggregates[key]
        source = "+".join(sorted(sources)) or "none"
        coverage = "complete" if complete == folders and extent else "partial"
        coverage += f"; {complete}/{folders} known folders complete"
        if not extent:
            coverage += "; descendants unknown"
        if metadata_folders:
            coverage += f"; {measured}/{captured} measured messages ({metadata_folders} metadata folders)"
        return total, source, coverage
    selected = sorted(rows, key=lambda key: rows[key].folder.path)
    if top is not None:
        selected = sorted((key for key in rows if rows[key].folder.selectable),
                          key=lambda key: (rows[key].result.total is None,
                                           -(rows[key].result.total or 0), rows[key].folder.path))[:top]
    for key in selected:
        row = rows[key]
        direct = row.result.total if row.folder.selectable else "-"
        direct = "unknown" if direct is None else direct
        source = row.result.source if row.folder.selectable else "container"
        coverage = row.result.coverage if row.folder.selectable else "n/a"
        total, subtree_source, subtree_coverage = aggregate_labels(key)
        print(f"{row.folder.path}\t{direct}\t{total}\t{source}\t{coverage}\t{subtree_source}\t{subtree_coverage}")
    total, source, coverage = aggregate_labels(root)
    scope = "all descendants" if depth is None else f"depth {depth}"
    print(f"TOTAL\t{total} mailbox bytes\t{source}\t{coverage}\t{scope}")
