"""Shallow LIST discovery and reversible display paths, independent of SELECT."""

import base64
from dataclasses import dataclass, field
import re
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
    def __init__(self, client, seconds, deadline):
        self.client, self.seconds, self.deadline = client, seconds, deadline
        self.root = Folder(b"", None, (b"\\Noselect",), "")
        self.current = self.root

    def children(self, node, refresh=False):
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

    def show(self, refresh=False):
        for child in self.children(self.current, refresh).values():
            suffix = " [container]" if not child.selectable else ""
            print(child.component + suffix)
