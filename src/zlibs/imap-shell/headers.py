"""Bounded read-only header pages for the private terminal."""

from email import policy
from email.parser import BytesHeaderParser
from email.utils import parsedate_to_datetime
import contextlib
import re
import unicodedata

from folders import quoted
import display

HEADER_OCTETS = 8192
PAGE_SIZE = 50
BATCH_SIZE = 10


class LiteralLimit(RuntimeError):
    pass


class PagingError(ValueError):
    """Only locally authored, safe operator guidance belongs here."""


class MailboxChanged(ValueError):
    pass


def safe_text(value, limit=160):
    text = "".join(" " if char.isspace() else
                   "�" if unicodedata.category(char).startswith("C") else char
                   for char in str(value))
    text = " ".join(text.split())
    return text[:limit] + ("…" if len(text) > limit else "")


def size_text(size):
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024 or unit == "TiB":
            return f"{int(value)} B" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024


def row(metadata, literal):
    uid = re.search(rb"\bUID (\d+)\b", metadata, re.I)
    size = re.search(rb"\bRFC822.SIZE (\d+)\b", metadata, re.I)
    internal = re.search(rb'\bINTERNALDATE "([^"]*)"', metadata, re.I)
    if uid is None or size is None:
        raise ValueError("missing message metadata")
    message = BytesHeaderParser(policy=policy.default).parsebytes(literal)
    truncated = len(literal) >= HEADER_OCTETS
    def field(name):
        try:
            header = message.get(name, "")
            if getattr(header, "defects", ()):
                return "[malformed]"
            value = safe_text(header)
            return value or "[missing]"
        except (ValueError, LookupError, UnicodeError):
            return "[malformed]"
    date = None
    try:
        date = parsedate_to_datetime(str(message.get("Date", "")))
    except (ValueError, TypeError, OverflowError):
        pass
    if date is not None:
        date = date.isoformat(sep=" ")
    elif internal is not None:
        date = safe_text(internal.group(1).decode("ascii", "replace")) + " [INTERNALDATE]"
    else:
        date = "[missing date]"
    marker = " [headers truncated]" if truncated else ""
    return int(uid.group(1)), (f"{int(uid.group(1))}\t{size_text(int(size.group(1)))}\t"
                               f"{safe_text(date)}\t{field('From')}\t{field('Subject')}{marker}")


def examine(client, raw):
    # imaplib emits EXAMINE for readonly=True. Raw names must stay byte exact.
    if any(char in raw for char in (b"\r", b"\n", b"\0")):
        client.literal = raw
        mailbox = None
    else:
        mailbox = quoted(raw)
    status, data = client.select(mailbox, readonly=True)
    if status != "OK" or not data or not data[0].isdigit():
        raise ValueError("EXAMINE failed")
    return int(data[0])


def response_rows(data):
    """Join imaplib's literal prefixes and metadata suffixes per FETCH reply."""
    rows, fragments, literal = [], [], None
    def finish():
        metadata = b" ".join(fragments)
        section = re.search(rb"\bBODY\[HEADER\.FIELDS[^\]]*\](?:<\d+>)?", metadata, re.I)
        if section is None:
            return  # Unsolicited FLAGS updates are not requested header rows.
        value = literal
        if value is None:
            if not re.match(rb'\s+(?:NIL|"")(?=\s|\))', metadata[section.end():], re.I):
                raise ValueError("missing header value")
            value = b""
        rows.append(row(metadata, value))
    for item in data:
        if item is None:
            continue
        prefix = item[0] if isinstance(item, tuple) else item
        if not isinstance(prefix, bytes):
            raise ValueError("invalid FETCH response")
        if re.match(rb"^\d+ \(", prefix):
            if fragments:
                finish()
            fragments, literal = [], None
        elif not fragments:
            raise ValueError("orphan FETCH segment")
        if isinstance(item, tuple):
            if literal is not None or not isinstance(item[1], bytes):
                raise ValueError("unexpected header literal")
            # Literal payload never becomes metadata, even if it contains UID
            # or SIZE-looking text. Only imaplib's protocol segments are joined.
            literal = item[1]
            prefix = re.sub(rb"\{\d+\}$", b"", prefix)
        fragments.append(prefix)
    if fragments:
        finish()
    return rows


def request(client, method, *args):
    try:
        status, data = method(*args)
    except BaseException:
        # A partial protocol read is not safe to reuse, even for LOGOUT.
        with contextlib.suppress(OSError):
            client.shutdown()
        raise
    if status != "OK":
        raise ValueError("FETCH failed")
    return data


def response_number(client, name):
    _, data = client.response(name)
    if not data or not isinstance(data[0], bytes) or not data[0].isdigit():
        return None
    return int(data[0])


def sequence_uids(client, start, end):
    original_read = client.read
    def no_literal(size):
        raise LiteralLimit("unexpected literal in UID inventory")
    client.read = no_literal
    try:
        data = request(client, client.fetch, f"{start}:{end}", "(UID)")
    finally:
        client.read = original_read
    if client.untagged_responses.pop("EXPUNGE", None):
        raise MailboxChanged()
    result = []
    for item in data:
        if item is None:
            continue
        if not isinstance(item, bytes):
            raise ValueError("unexpected inventory literal")
        position = re.match(rb"(\d+) \(", item)
        uid = re.search(rb"\bUID (\d+)\b", item, re.I)
        if position and uid:
            result.append((int(position[1]), int(uid[1])))
        elif b"FLAGS" not in item.upper():
            raise ValueError("missing inventory UID")
    if len(result) > end - start + 1:
        raise ValueError("too many inventory rows")
    return result


def boundary_sequence(client, count, before):
    # Binary search over extant positions, never over possibly enormous UID
    # gaps. Restart on observed EXPUNGE so shifted positions are not trusted.
    low, high = 1, count
    while low <= high:
        middle = (low + high) // 2
        values = sequence_uids(client, middle, middle)
        if len(values) != 1 or values[0][0] != middle:
            raise MailboxChanged()
        if values[0][1] < before:
            low = middle + 1
        else:
            high = middle - 1
    return high


def fetch_headers(client, uids):
    original_read = client.read
    literal_count = 0
    def bounded_read(size):
        nonlocal literal_count
        literal_count += 1
        if size > HEADER_OCTETS or literal_count > len(uids):
            raise LiteralLimit("header literal exceeds limit")
        return original_read(size)
    client.read = bounded_read
    try:
        data = request(client, client.uid, "FETCH", ",".join(map(str, uids)),
            f"(UID RFC822.SIZE INTERNALDATE BODY.PEEK[HEADER.FIELDS (DATE FROM SUBJECT)]<0.{HEADER_OCTETS}>)")
    finally:
        client.read = original_read
    rows = response_rows(data)
    if len(rows) > len(uids) or len({uid for uid, _ in rows}) != len(rows) or any(uid not in uids for uid, _ in rows):
        raise ValueError("unexpected header UID")
    return rows


class Pager:
    def __init__(self):
        self.reset()

    def reset(self):
        self.cursor = None

    def show(self, client, folder, seconds, deadline, limit=PAGE_SIZE, more=False):
        saved = self.cursor if more else None
        if more and (saved is None or saved["raw"] != folder.raw):
            self.reset()
            raise PagingError("Run ls in this folder to start a header page")
        if not more:
            self.reset()
        anchor_validity, anchor_upper = None, None
        with deadline(seconds):
            for attempt in range(3):
                count = examine(client, folder.raw)
                validity = response_number(client, "UIDVALIDITY")
                upper = response_number(client, "UIDNEXT")
                if validity is None:
                    raise PagingError("UIDVALIDITY unavailable; run ls to retry")
                if saved is not None and validity != saved["validity"]:
                    self.reset()
                    raise PagingError("Mailbox replaced (UIDVALIDITY changed); run ls to refresh")
                if anchor_validity is not None and validity != anchor_validity:
                    raise PagingError("Mailbox replaced during listing; run ls to refresh")
                anchor_validity = validity
                client.untagged_responses.pop("EXPUNGE", None)
                try:
                    if saved is not None:
                        limit, upper = saved["limit"], saved["upper"]
                        end = boundary_sequence(client, count, saved["before"]) if count and saved["before"] > 1 else 0
                    else:
                        if upper is None:
                            tail = sequence_uids(client, count, count) if count else []
                            if count and not tail:
                                raise MailboxChanged()
                            upper = tail[0][1] if tail else 0
                        else:
                            upper -= 1
                        if anchor_upper is None:
                            anchor_upper = upper
                        upper = anchor_upper
                        end = boundary_sequence(client, count, upper + 1) if attempt and count else count
                    inventory = []
                    for start in range(max(1, end - limit + 1), end + 1, BATCH_SIZE):
                        values = sequence_uids(client, start, min(start + BATCH_SIZE - 1, end))
                        inventory.extend(uid for _, uid in values if uid <= upper and
                                         (saved is None or uid < saved["before"]))
                    inventory = sorted(set(inventory), reverse=True)
                    rows = []
                    for start in range(0, len(inventory), BATCH_SIZE):
                        rows.extend(fetch_headers(client, inventory[start:start + BATCH_SIZE]))
                    # UID FETCH can observe expunges safely: the fixed UID list
                    # cannot admit a replacement message or newly arrived UID.
                    client.untagged_responses.pop("EXPUNGE", None)
                    break
                except MailboxChanged:
                    if attempt == 2:
                        self.reset()
                        raise PagingError("Mailbox changed repeatedly; run ls to refresh")
            self.cursor = {"raw": folder.raw, "validity": validity, "upper": upper,
                           "before": min(inventory) if inventory else 0, "limit": limit}
        if rows:
            display.header_page(sorted(rows, reverse=True))
        elif more:
            print("No more messages")
