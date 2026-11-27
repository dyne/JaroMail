"""Read-only wildcard rename planning over fresh, exact mailbox identities."""

from dataclasses import dataclass, field
from bisect import bisect_left, bisect_right
from urllib.parse import unquote_to_bytes

from folders import FolderTree, canonical_component, display_component


class PreviewError(ValueError):
    pass


def tokenize(line):
    """Remove command quotes but retain backslashes for the pattern grammar.

    This tokenizer is private to rename-glob. Single quotes preserve everything;
    elsewhere a backslash pairs with the next character, including whitespace or
    quotes. Neither stage evaluates shell expansions.
    """
    words, token, quote, started = [], [], None, False
    index = 0
    while index < len(line):
        char = line[index]
        if char == "\\" and quote != "'":
            if index + 1 == len(line):
                raise ValueError("trailing command escape")
            token.extend((char, line[index + 1]))
            started = True
            index += 2
            continue
        if quote:
            if char == quote:
                quote = None
            else:
                token.append(char)
        elif char in "\"'":
            quote = char
            started = True
        elif char.isspace():
            if started:
                words.append("".join(token))
                token, started = [], False
        else:
            token.append(char)
            started = True
        index += 1
    if quote:
        raise ValueError("unterminated command quote")
    if started:
        words.append("".join(token))
    return words


def path_parts(value):
    """Split logical separators before decoding display or backslash escapes."""
    parts, part, index = [], [], 0
    absolute = value.startswith("/")
    while index < len(value):
        char = value[index]
        if char == "\\":
            if index + 1 == len(value):
                raise PreviewError("trailing pattern escape")
            part.append((value[index + 1], True))
            index += 2
            continue
        if char == "/":
            parts.append(part)
            part = []
        else:
            part.append((char, False))
        index += 1
    parts.append(part)
    if absolute:
        parts.pop(0)
    if any(not part for part in parts):
        raise PreviewError("empty path component")
    return absolute, parts


def literal(chars):
    # Escaping a percent must yield a literal percent, not a second decoding.
    text = "".join("".join("%%%02X" % byte for byte in char.encode("utf-8"))
                   if escaped else char for char, escaped in chars)
    return unquote_to_bytes(canonical_component(text)).decode("utf-8") if text else ""


def path_component(chars):
    if chars in ([(".", False)], [(".", False), (".", False)]):
        return "".join(char for char, _ in chars)
    return display_component(literal(chars))


def absolute_path(tree, absolute, components):
    result = [] if absolute else [part for part in tree.current.path.split("/") if part]
    for component in components:
        if component == ".":
            continue
        if component == "..":
            if result:
                result.pop()
        else:
            result.append(component)
    return "/" + "/".join(result)


def template_path(tree, template, capture):
    absolute, parts = path_parts(template)
    components = []
    for part in parts:
        chunks, pending, index, substituted = [], [], 0, False
        while index < len(part):
            if part[index:index + 3] == [("{", False), ("1", False), ("}", False)]:
                chunks.extend((literal(pending), capture))
                pending = []
                substituted = True
                index += 3
            else:
                pending.append(part[index])
                index += 1
        if substituted:
            text = "".join(chunks) + literal(pending)
            if not text:
                raise PreviewError("empty destination component")
            # A capture is data, including slash, percent, dots and {1} itself.
            components.append(display_component(text))
        else:
            components.append(path_component(part))
    return absolute_path(tree, absolute, components)


def within(raw, root, delimiter):
    boundary = root if delimiter is not None and root.endswith(delimiter) else root + (delimiter or b"")
    return raw == root or (delimiter is not None and raw.startswith(boundary))


def boundary(raw, delimiter):
    return raw if raw.endswith(delimiter) else raw + delimiter


def prefix_range(names, prefix):
    """Half-open sorted range for a byte prefix, without scanning its members."""
    end = bytearray(prefix)
    while end and end[-1] == 255:
        end.pop()
    if end:
        end[-1] += 1
    return bisect_left(names, prefix), bisect_left(names, bytes(end)) if end else len(names)


class PreviewTree(FolderTree):
    """One fresh snapshot per preview; list each distinct parent only once.

    Inherited destination validation requests write-style refreshes. This local
    read-only tree reuses its first fresh observation; L4's tree is unchanged.
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.delimiter_cache = {}

    def children(self, node, refresh=False, status_sizes=False):
        return super().children(node, status_sizes=status_sizes)

    def destination_delimiter(self, parent, children):
        key = (parent.raw, parent.delimiter)
        if key not in self.delimiter_cache:
            try:
                self.delimiter_cache[key] = (super().destination_delimiter(parent, children), None)
            except ValueError as exc:
                self.delimiter_cache[key] = (None, str(exc))
        delimiter, error = self.delimiter_cache[key]
        if error is not None:
            raise PreviewError(error)
        return delimiter


class Subtrees:
    """Indexed exact roots and subtree boundaries for overlap/dependency checks."""
    def __init__(self, entries):
        self.entries = dict(entries)
        self.names = sorted(raw for raw, _ in self.entries.values())
        self.exact, self.boundaries, self.delimiters = {}, {}, set()
        for index, (raw, delimiter) in self.entries.items():
            self.exact.setdefault(raw, []).append(index)
            if delimiter is not None:
                self.boundaries.setdefault(boundary(raw, delimiter), []).append(index)
                self.delimiters.add(delimiter[0])

    def related(self, raw, delimiter, exclude=None):
        # Only two representatives are needed to find an owner other than self,
        # even when many mappings have the same destination or boundary.
        if any(index != exclude for index in self.exact.get(raw, ())[:2]):
            return True
        for position, char in enumerate(raw):
            if char in self.delimiters and any(index != exclude for index in
                    self.boundaries.get(raw[:position + 1], ())[:2]):
                return True
        if delimiter is None:
            count = bisect_right(self.names, raw) - bisect_left(self.names, raw)
        else:
            prefix = boundary(raw, delimiter)
            left, right = prefix_range(self.names, prefix)
            count = right - left
            if raw != prefix:
                count += bisect_right(self.names, raw) - bisect_left(self.names, raw)
        if exclude in self.entries and within(self.entries[exclude][0], raw, delimiter):
            count -= 1
        return count > 0


class Inventory:
    """Group sorted raw identities once; disjoint sources visit each member once."""
    def __init__(self, existing):
        self.existing, self.names = existing, {}
        for raw, delimiter in existing.items():
            self.names.setdefault(delimiter, []).append(raw)
        for names in self.names.values():
            names.sort()

    def scope(self, source, materialize=True):
        names = self.names.get(source.delimiter, ())
        exact = self.existing.get(source.raw, "missing") == source.delimiter
        if source.delimiter is None:
            return int(exact), (source.raw,) if exact and materialize else ()
        prefix = boundary(source.raw, source.delimiter)
        left, right = prefix_range(names, prefix)
        extra = (source.raw,) if exact and source.raw != prefix else ()
        count = right - left + len(extra)
        return count, extra + tuple(names[left:right]) if materialize else ()


@dataclass
class Mapping:
    source: object
    capture: str
    target_path: str
    parent: object = None
    raw: object = None
    delimiter: object = None
    members: tuple = ()
    member_count: object = None
    errors: list = field(default_factory=list)


@dataclass
class Preview:
    mappings: list
    errors: list = field(default_factory=list)
    source_pattern: str = ""
    template: str = ""
    source_parent: object = None
    prefix: str = ""
    suffix: str = ""
    utf8: bool = False

    @property
    def blocked(self):
        return bool(self.errors or any(pair.errors for pair in self.mappings))

    def show(self):
        print("Rename preview: " + str(len(self.mappings)) + " matches; each mapping moves its entire subtree")
        for pair in self.mappings:
            scope = str(pair.member_count) + " listed mailboxes" if pair.member_count is not None else "scope unavailable"
            print(pair.source.path + "\t->\t" + pair.target_path + "\tsubtree: " + scope)
            for error in sorted(set(pair.errors)):
                print("  BLOCKED: " + error)
        for error in sorted(set(self.errors)):
            print("BLOCKED: " + error)
        print("Apply blocked" if self.blocked else "Preview only; no changes made")


def build(tree, source_pattern, template):
    absolute, parts = path_parts(source_pattern)
    if any(("*", False) in part for part in parts[:-1]):
        raise PreviewError("SOURCE parent must be literal")
    stars = [index for index, item in enumerate(parts[-1]) if item == ("*", False)]
    if len(stars) != 1:
        raise PreviewError("SOURCE needs exactly one unescaped * in its final component")
    index = stars[0]
    prefix, suffix = literal(parts[-1][:index]), literal(parts[-1][index + 1:])
    # Never retarget relative input through a stale cached current folder.
    fresh = PreviewTree(tree.client, tree.seconds, tree.deadline)
    if not absolute or not template.startswith("/"):
        fresh.current = fresh.resolve_write(tree.current.path)
        if (fresh.current.raw, fresh.current.delimiter) != (tree.current.raw, tree.current.delimiter):
            raise PreviewError("current mailbox identity changed")
    parent_path = absolute_path(fresh, absolute, [path_component(part) for part in parts[:-1]])
    parent = fresh.resolve_write(parent_path)
    mappings = []
    for source in fresh.children(parent, refresh=True).values():
        text = unquote_to_bytes(source.component).decode("utf-8")
        if not text.startswith(prefix) or not text.endswith(suffix) or len(text) < len(prefix) + len(suffix):
            continue
        capture = text[len(prefix):len(text) - len(suffix) if suffix else None]
        pair = Mapping(source, capture, "[invalid destination]")
        mappings.append(pair)
        try:
            pair.target_path = template_path(fresh, template, capture)
            # destination() is L4's complete-path and namespace validator. An
            # existing target is retained as an identity for graph diagnostics.
            try:
                pair.parent, pair.raw, pair.delimiter = fresh.destination(pair.target_path)
            except ValueError as exc:
                if str(exc) != "destination exists":
                    raise
                target = fresh.resolve_write(pair.target_path)
                pair.parent, pair.raw, pair.delimiter = target.parent, target.raw, target.delimiter
                pair.errors.append("destination exists")
            if source.raw.upper() == b"INBOX":
                pair.errors.append("cannot rename root or INBOX")
            if b"\0" in source.raw:
                pair.errors.append("NUL is not a mailbox character")
            if pair.raw.upper() == b"INBOX":
                pair.errors.append("special INBOX destination")
            if source.delimiter != pair.delimiter:
                pair.errors.append("source and destination namespace delimiters differ")
        except ValueError as exc:
            pair.errors.append(str(exc))
    leaf_pattern = "".join(("\\" if escaped else "") + char for char, escaped in parts[-1])
    bound_source = parent.path.rstrip("/") + "/" + leaf_pattern
    # A literal {1} in cwd must not become a template marker when binding it.
    cwd = fresh.current.path.replace("{", "%7B").replace("}", "%7D")
    bound_template = template if template.startswith("/") else cwd.rstrip("/") + "/" + template
    preview = Preview(sorted(mappings, key=lambda pair: pair.source.path), source_pattern=bound_source,
                      template=bound_template, source_parent=parent, prefix=prefix, suffix=suffix,
                      utf8=tree.client.utf8_enabled)
    # Even a zero-match preview must reject malformed template syntax.
    if not mappings:
        template_path(fresh, template, "capture")
        return preview
    sources = Subtrees((index, (pair.source.raw, pair.source.delimiter))
                       for index, pair in enumerate(preview.mappings))
    destinations = Subtrees((index, (pair.raw, pair.delimiter))
                            for index, pair in enumerate(preview.mappings) if pair.raw is not None)
    overlaps = set()
    for index, pair in enumerate(preview.mappings):
        if sources.related(pair.source.raw, pair.source.delimiter, exclude=index):
            pair.errors.append("overlapping source subtrees")
            overlaps.add(index)
        if pair.raw is not None:
            if len(destinations.exact[pair.raw]) > 1:
                pair.errors.append("duplicate destinations")
            elif destinations.related(pair.raw, pair.delimiter, exclude=index):
                pair.errors.append("overlapping destination subtrees")
            if sources.related(pair.raw, pair.delimiter, exclude=index):
                pair.errors.append("rename dependency chain or cycle requires temporary names")
            if within(pair.raw, pair.source.raw, pair.source.delimiter):
                pair.errors.append("destination is inside source or unchanged")
    try:
        inventory = fresh.name_query("LIST", b"*")
        existing = {}
        for raw, delimiter, flags in inventory:
            if b"\\NONEXISTENT" not in (flag.upper() for flag in flags):
                if raw in existing and existing[raw] != delimiter:
                    raise PreviewError("ambiguous mailbox identity in subtree inventory")
                existing[raw] = delimiter
        indexed = Inventory(existing)
        for index, pair in enumerate(preview.mappings):
            source = pair.source
            if existing.get(source.raw, "missing") != source.delimiter:
                pair.errors.append("source membership or identity changed")
            # Blocked overlapping roots still get exact indexed scope counts;
            # do not duplicate their potentially large descendant inventories.
            pair.member_count, pair.members = indexed.scope(source, materialize=index not in overlaps)
            if pair.raw is None or source.delimiter != pair.delimiter:
                continue
            for raw in pair.members:
                suffix = raw[len(source.raw):]
                if suffix and source.delimiter is not None and source.raw.endswith(source.delimiter):
                    suffix = (b"" if pair.raw.endswith(pair.delimiter) else pair.delimiter) + suffix
                target = pair.raw + suffix
                if target in existing:
                    pair.errors.append("destination subtree collides with an existing mailbox")
                    break
    except ValueError as exc:
        preview.errors.append(str(exc))
    return preview
