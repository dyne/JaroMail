"""Shallow LIST discovery and reversible display paths, independent of SELECT."""

import base64
import contextlib
from dataclasses import dataclass, field
import re
import sys
from urllib.parse import unquote_to_bytes
import unicodedata


def mailbox_text(raw, utf8):
    if utf8:
        return raw.decode("utf-8")
    result = []
    position = 0
    while position < len(raw):
        end = raw.find(b"&", position)
        if end < 0:
            result.append(raw[position:].decode("ascii"))
            break
        result.append(raw[position:end].decode("ascii"))
        closing = raw.find(b"-", end)
        if closing < 0:
            raise ValueError("unterminated modified UTF-7")
        encoded = raw[end + 1:closing]
        if encoded:
            encoded = encoded.replace(b",", b"/")
            result.append(base64.b64decode(encoded + b"=" * (-len(encoded) % 4),
                                           validate=True).decode("utf-16-be"))
        else:
            result.append("&")
        position = closing + 1
    return "".join(result)


def display_component(text):
    if not text:
        raise ValueError("empty folder component")
    result = []
    for char in text:
        if char in '/%*?[]\'"\\' or char.isspace() or unicodedata.category(char) in ("Cc", "Cf") or text in (".", ".."):
            result.extend("%%%02X" % byte for byte in char.encode("utf-8"))
        else:
            result.append(char)
    return "".join(result)


def canonical_component(text):
    if re.search(r"%(?![0-9a-fA-F]{2})", text):
        raise ValueError("invalid display escape")
    return display_component(unquote_to_bytes(text).decode("utf-8"))


def quoted(raw):
    return b'"' + raw.replace(b"\\", b"\\\\").replace(b'"', b'\\"') + b'"'


def mailbox_bytes(text, utf8):
    if utf8:
        return text.encode("utf-8")
    result, pending = bytearray(), []
    def flush():
        if pending:
            encoded = "".join(pending).encode("utf-16-be")
            result.extend(b"&" + base64.b64encode(encoded).rstrip(b"=").replace(b"/", b",") + b"-")
            pending.clear()
    for char in text:
        if " " <= char <= "~":
            flush()
            result.extend(b"&-" if char == "&" else char.encode("ascii"))
        else:
            pending.append(char)
    flush()
    return bytes(result)


class WriteRejected(ValueError):
    pass


class WriteUnknown(Exception):
    pass


def write_mailboxes(client, command, operands):
    """Send one command, with synchronized literals at any operand position.

    imaplib's public methods only support a literal as the final operand.
    Read the tagged result directly so synchronized BAD remains recoverable.
    No exception from a possibly sent command is retried here.
    """
    pieces = []
    literals = []
    for raw in operands:
        if b"\0" in raw:
            raise ValueError("NUL is not a mailbox character")
        if b"\r" in raw or b"\n" in raw:
            pieces.append(b"{" + str(len(raw)).encode() + b"}")
            literals.append((b" ".join(pieces), raw))
            pieces = []
        else:
            pieces.append(quoted(raw))
    tail = b" ".join(pieces)
    first = literals[0][0] if literals else tail
    try:
        client._check_bye()
        tag = client._command(command, first)
        for index, (_, raw) in enumerate(literals):
            while client._get_response():
                if client.tagged_commands[tag]:
                    break
            if client.tagged_commands[tag]:
                break
            suffix = literals[index + 1][0] if index + 1 < len(literals) else tail
            client.send(raw + (b" " + suffix if suffix else b"") + b"\r\n")
        status, _ = client._get_tagged_response(tag)
        client._check_bye()
    except BaseException:
        # An unfinished literal may consume a later LOGOUT as mailbox bytes.
        # Close before unwinding; only a completed tagged result is reusable.
        with contextlib.suppress(OSError):
            client.shutdown()
        raise WriteUnknown("tagged completion unavailable") from None
    if status != "OK":
        raise WriteRejected(command + " rejected")


def read_quoted(raw, position):
    if raw[position:position + 1] != b'"':
        raise ValueError("expected quoted string")
    value = bytearray()
    position += 1
    while position < len(raw):
        char = raw[position]
        position += 1
        if char == 34:
            return bytes(value), position
        if char == 92:
            if position == len(raw) or raw[position] not in (34, 92):
                raise ValueError("invalid quoted escape")
            char = raw[position]
            position += 1
        if char in (0, 10, 13):
            raise ValueError("control in quoted string")
        value.append(char)
    raise ValueError("unterminated quoted string")


def parse_list(data):
    """imaplib preserves mailbox literals as (response prefix, literal) tuples."""
    result = []
    after_literal = False
    for item in data:
        if item in (None, b""):
            continue
        literal = None
        if isinstance(item, tuple):
            raw, literal = item
        else:
            raw = item
        if after_literal and raw[:1] == b" ":
            # Unrequested LIST return data may follow a mailbox literal. It
            # carries no identity used by this browser (RFC 9051 section 6.3.9).
            continue
        after_literal = literal is not None
        match = re.match(br"\(([^()]*)\) +", raw)
        if not match:
            raise ValueError("invalid LIST flags")
        flags = tuple(match[1].split())
        position = match.end()
        if raw[position:position + 3].upper() == b"NIL":
            delimiter, position = None, position + 3
        else:
            delimiter, position = read_quoted(raw, position)
            if len(delimiter) != 1:
                raise ValueError("invalid hierarchy delimiter")
        if raw[position:position + 1] != b" ":
            raise ValueError("missing mailbox")
        position += 1
        tail = raw[position:]
        if literal is not None:
            marker = re.fullmatch(br"\{([0-9]+)\}", tail)
            if not marker or int(marker[1]) != len(literal):
                raise ValueError("invalid mailbox literal")
            mailbox = literal
        elif tail.startswith(b'"'):
            mailbox, end = read_quoted(raw, position)
            if raw[end:] and not raw[end:].lstrip().startswith(b"("):
                raise ValueError("unexpected LIST data")
        else:
            mailbox, separator, extension = tail.partition(b" ")
            if not mailbox or re.search(br'[\x00-\x20(){}"\\]', mailbox):
                raise ValueError("invalid mailbox atom")
            if separator and not extension.lstrip().startswith(b"("):
                raise ValueError("unexpected LIST data")
        result.append((mailbox, delimiter, flags))
    return result


@dataclass
class Folder:
    raw: bytes
    delimiter: object
    flags: tuple
    component: str
    parent: object = None
    children: object = field(default=None, repr=False)

    @property
    def selectable(self):
        return not {b"\\NOSELECT", b"\\NONEXISTENT"}.intersection(flag.upper() for flag in self.flags)

    @property
    def path(self):
        if self.parent is None:
            return "/"
        return self.parent.path.rstrip("/") + "/" + self.component


class FolderTree:
    def __init__(self, client, seconds, deadline, reconnect=None):
        self.client, self.seconds, self.deadline = client, seconds, deadline
        self.reconnect = reconnect
        self.root = Folder(b"", None, (b"\\Noselect",), "")
        self.current = self.root
        self.latest_status = []

    def children(self, node, refresh=False, status_sizes=False):
        self.latest_status = []
        if node.children is not None and not refresh:
            return node.children
        if node is not self.root and node.delimiter is None:
            node.children = {}
            return node.children
        prefix = b""
        if node is not self.root:
            prefix = node.raw if node.raw.endswith(node.delimiter) else node.raw + node.delimiter
        pattern = prefix + b"%"
        with self.deadline(self.seconds):
            if any(char in pattern for char in (b"\r", b"\n", b"\0")):
                self.client.literal = pattern
                status, data = self.client.list('""', None)
            elif status_sizes:
                self.client.untagged_responses.pop("STATUS", None)
                # Read the completed tagged result directly: _simple_command
                # turns BAD into an exception indistinguishable from errors
                # before a synchronized response. Only tagged NO/BAD permits
                # retrying the ordinary LIST on this same connection.
                self.client._check_bye()
                tag = self.client._command("LIST", '""', quoted(pattern), "RETURN (STATUS (SIZE))")
                status, data = self.client._get_tagged_response(tag)
                self.client._check_bye()
                if status in ("NO", "BAD"):
                    self.client.untagged_responses.pop("LIST", None)
                    self.client.untagged_responses.pop("STATUS", None)
                    status, data = self.client.list('""', quoted(pattern))
                else:
                    status, data = self.client._untagged_response(status, data, "LIST")
                    self.latest_status = self.client.untagged_responses.pop("STATUS", [])
            else:
                status, data = self.client.list('""', quoted(pattern))
        if status != "OK":
            raise ValueError("LIST failed")
        children = {}
        for raw, delimiter, flags in parse_list(data):
            if not raw.startswith(prefix):
                continue
            component = raw[len(prefix):]
            # LIST patterns containing mailbox wildcards can return unrelated
            # mailboxes. Verify the exact parent and depth after the response.
            if node is not self.root and delimiter != node.delimiter:
                continue
            if delimiter is not None:
                component = component[:-len(delimiter)] if component.endswith(delimiter) else component
                if delimiter in component:
                    continue
            if not component:
                continue
            display = display_component(mailbox_text(component, self.client.utf8_enabled))
            child = Folder(raw, delimiter, flags, display, node)
            if display in children and (children[display].raw, children[display].delimiter) != (raw, delimiter):
                raise ValueError("ambiguous folder identity")
            children[display] = child
        node.children = dict(sorted(children.items()))
        return node.children

    def cd(self, path):
        node = self.root if path.startswith("/") else self.current
        for component in path.split("/"):
            if component in ("", "."):
                continue
            if component == "..":
                node = node.parent or self.root
                continue
            component = canonical_component(component)
            if component not in self.children(node):
                raise ValueError("folder does not exist")
            node = self.children(node)[component]
        self.current = node

    def resolve_write(self, path):
        components = [] if path.startswith("/") else [part for part in self.current.path.split("/") if part]
        for component in path.split("/"):
            if component in ("", "."):
                continue
            if component == "..":
                if components:
                    components.pop()
                continue
            components.append(component)
        node = self.root
        for component in components:
            if not component:
                continue
            children = self.children(node, refresh=True)
            component = canonical_component(component)
            if component not in children:
                raise ValueError("folder does not exist")
            node = children[component]
            if b"\\NONEXISTENT" in (flag.upper() for flag in node.flags):
                raise ValueError("folder does not exist")
        return node

    def destination(self, path):
        parent_path, separator, leaf = path.rpartition("/")
        if not leaf or leaf in (".", ".."):
            raise ValueError("destination must name a folder")
        parent = self.resolve_write((parent_path or "/") if separator else ".")
        children = self.children(parent, refresh=True)
        component = canonical_component(leaf)
        if component in children:
            raise ValueError("destination exists")
        delimiter = self.destination_delimiter(parent, children)
        text = unquote_to_bytes(component).decode("utf-8")
        if "\0" in text:
            raise ValueError("NUL is not a mailbox character")
        raw = mailbox_bytes(text, self.client.utf8_enabled)
        if not raw or b"\0" in raw or (delimiter is not None and delimiter in raw):
            raise ValueError("invalid folder component")
        prefix = b"" if parent is self.root else parent.raw if parent.raw.endswith(delimiter) else parent.raw + delimiter
        raw = prefix + raw
        if raw.upper() == b"INBOX":
            raise ValueError("special INBOX destination")
        return parent, raw, delimiter

    def destination_delimiter(self, parent, children):
        if parent is self.root:
            delimiters = {child.delimiter for child in children.values()}
            if not delimiters:
                # The empty reference/pattern requests the root delimiter,
                # including NIL for an empty flat namespace (RFC 9051 LIST).
                with self.deadline(self.seconds):
                    status, data = self.client.list('""', '""')
                if status != "OK":
                    raise ValueError("namespace discovery failed")
                roots = parse_list(data)
                if any(raw != b"" for raw, _, _ in roots):
                    raise ValueError("unexpected namespace discovery")
                delimiters = {delimiter for _, delimiter, _ in roots}
            if len(delimiters) != 1:
                raise ValueError("namespace delimiter is unknown or ambiguous")
            delimiter = next(iter(delimiters))
        else:
            delimiter = parent.delimiter
            if delimiter is None:
                raise ValueError("flat namespace has no children")
        return delimiter

    def mkdir(self, path):
        parent, raw, delimiter = self.destination(path)
        self.mutate("CREATE", None, parent, raw, delimiter)

    def rename(self, old, new):
        source = self.resolve_write(old)
        if source is self.root or source.raw.upper() == b"INBOX":
            raise ValueError("cannot rename root or INBOX")
        parent, raw, delimiter = self.destination(new)
        ancestor = parent
        while ancestor is not None:
            if (ancestor.raw, ancestor.delimiter) == (source.raw, source.delimiter):
                raise ValueError("destination is inside source")
            ancestor = ancestor.parent
        self.mutate("RENAME", source, parent, raw, delimiter)

    def name_query(self, command, pattern, subscribed=False):
        options = ('(SUBSCRIBED)', '""') if subscribed else ('""',)
        try:
            with self.deadline(self.seconds):
                if any(char in pattern for char in (b"\r", b"\n", b"\0")):
                    self.client.literal = pattern
                    tag = self.client._command(command, *options)
                else:
                    tag = self.client._command(command, *options, quoted(pattern))
                status, data = self.client._get_tagged_response(tag)
                self.client._check_bye()
                status, data = self.client._untagged_response(status, data, command)
        except BaseException:
            with contextlib.suppress(OSError):
                self.client.shutdown()
            raise
        if status != "OK":
            raise ValueError(command + " unavailable")
        return parse_list(data)

    def subscriptions(self, roots):
        observed = set()
        rev2 = "IMAP4REV2" in self.client.capabilities and "IMAP4REV1" not in self.client.capabilities
        for raw, delimiter in roots:
            command = "LIST" if rev2 else "LSUB"
            boundary = raw if delimiter is not None and raw.endswith(delimiter) else raw + (delimiter or b"")
            for name, actual_delimiter, flags in self.name_query(command, raw + b"*", subscribed=rev2):
                if actual_delimiter != delimiter:
                    continue
                if rev2 and b"\\SUBSCRIBED" not in (flag.upper() for flag in flags):
                    continue
                if name == raw or (delimiter is not None and name.startswith(boundary)):
                    observed.add((name, delimiter))
        return observed

    def probe(self, raw, delimiter):
        matches = [(name, actual) for name, actual, flags in self.name_query("LIST", raw)
                   if name == raw and b"\\NONEXISTENT" not in (flag.upper() for flag in flags)]
        if any(actual != delimiter for _, actual in matches):
            raise ValueError("mailbox identity changed")
        return bool(matches)

    def restore_navigation(self, path, expected=None):
        # A fresh tree invalidates stale descendant caches too. Resolve the
        # desired path exactly, then try each surviving logical ancestor.
        self.root.children = None
        desired = path
        while True:
            try:
                node = self.resolve_write(path)
                if path == desired and expected is not None and (node.raw, node.delimiter) != expected:
                    raise ValueError("mailbox identity changed")
                self.current = node
                return path != desired
            except ValueError:
                if path == "/":
                    self.current = self.root
                    return True
                path = path.rpartition("/")[0] or "/"

    def mutate(self, command, source, parent, raw, delimiter, preflight=None, on_outcome=None):
        current_path = self.current.path
        expected = (self.current.raw, self.current.delimiter)
        prefix_length = 0 if parent is self.root else len(parent.raw)
        if parent is not self.root and not parent.raw.endswith(delimiter):
            prefix_length += len(delimiter)
        target_path = parent.path.rstrip("/") + "/" + display_component(
            mailbox_text(raw[prefix_length:], self.client.utf8_enabled))
        roots = [(raw, delimiter)]
        operands = [raw]
        parents = {parent.path}
        if source is not None:
            roots.insert(0, (source.raw, source.delimiter))
            operands.insert(0, source.raw)
            parents.add(source.parent.path)
        try:
            before = self.subscriptions(roots)
        except ValueError:
            before = None
        # Bulk callers can fence exact saved identities after the optional
        # subscription observations and immediately before sending a write.
        if preflight is not None:
            preflight()
        outcome = "confirmed"
        try:
            with self.deadline(self.seconds):
                write_mailboxes(self.client, command, operands)
        except WriteRejected:
            outcome = "rejected"
        except WriteUnknown:
            outcome = "unknown"
            if on_outcome is not None:
                on_outcome(outcome)
            old_encoding = self.client.utf8_enabled
            print(command + " outcome unknown: tagged completion missing; not replayed", file=sys.stderr)
            if self.reconnect is None:
                raise
            self.client = self.reconnect()
            if self.client.utf8_enabled != old_encoding:
                self.root = Folder(b"", None, (b"\\Noselect",), "")
                self.current = self.root
                print("Mailbox encoding changed; observations unavailable; returned to /", file=sys.stderr)
                return outcome
            for label, (name, delim) in zip(("old", "new") if source else ("new",), roots):
                try:
                    state = "present" if self.probe(name, delim) else "not listed"
                except ValueError:
                    state = "unavailable"
                print("Observed " + label + " path: " + state + "; outcome remains unknown", file=sys.stderr)
        if outcome != "unknown":
            if on_outcome is not None:
                on_outcome(outcome)
            print(command + " " + outcome, file=sys.stderr)
        if outcome == "confirmed" and source is not None:
            if current_path == source.path or current_path.startswith(source.path + "/"):
                current_path = target_path + current_path[len(source.path):]
                suffix = expected[0][len(source.raw):]
                if suffix and source.delimiter is not None and source.raw.endswith(source.delimiter):
                    suffix = (b"" if raw.endswith(delimiter) else delimiter) + suffix
                expected = (raw + suffix, delimiter)
        for path in sorted(parents):
            try:
                self.children(self.resolve_write(path), refresh=True)
            except ValueError:
                pass
        if self.restore_navigation(current_path, expected):
            print("Current folder unavailable; returned to " + self.current.path, file=sys.stderr)
        try:
            after = self.subscriptions(roots)
        except ValueError:
            after = None
        if before is None or after is None:
            print("Subscription observations unavailable", file=sys.stderr)
        else:
            print("Observed subscriptions: +" + str(len(after - before)) + " -" + str(len(before - after)), file=sys.stderr)
        if outcome == "rejected":
            raise WriteRejected(command + " rejected")
        return outcome

    def show(self, refresh=False):
        for child in self.children(self.current, refresh).values():
            suffix = " [container]" if not child.selectable else ""
            print(child.component + suffix)
