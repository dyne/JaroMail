"""Guided edits to policy source. JaroMail compiles, evaluates and publishes it."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(os.environ["JAROMAILDIR"])
WORK = Path(os.environ["JAROWORKDIR"])
COMPILER = [WORK / "bin/jaro-filter-compile", "--root", ROOT, "--accounts-dir", ROOT / "Accounts"]
ENGINE = ROOT / "cache/filter-engine"
GUIDED = {"from-address", "to-address", "from-domain", "to-domain", "from-glob", "to-glob", "known-domain"}
WORD = re.compile(r'"(?:[^"\\]|\\.)*"|[^ \t"]+')
HEX = re.compile(r"[a-f0-9]{64}")
MAX_SOURCE = 64 << 20


class Problem(Exception):
    def __init__(self, code):
        self.code = code


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read(path, limit=MAX_SOURCE):
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as stream:
        size = os.fstat(stream.fileno())
        if not stat.S_ISREG(size.st_mode):
            raise Problem("invalid_resource")
        if size.st_size > limit:
            raise Problem("limit")
        data = stream.read(limit + 1)
        if len(data) > limit:
            raise Problem("limit")
        return data


def source(name):
    try:
        return read(ROOT / name).decode("utf-8")
    except FileNotFoundError:
        if name != "FilterProfile.txt":
            raise
        return None


def source_name(data):
    if data.get("kind", "rule") not in ("rule", "mapping"):
        raise Problem("invalid_request")
    return "FilterProfile.txt" if data.get("kind") == "mapping" else "Filters.txt"


def revision(text):
    return digest(b"\0absent" if text is None else text.encode())


def tokens(line):
    """Keep token spans so an edit retains whitespace, spelling and inline comments."""
    values = []
    end = 0
    while end < len(line):
        start = end
        while start < len(line) and line[start] in " \t":
            start += 1
        if start == len(line) or line[start] == "#":
            break
        match = WORD.match(line, start)
        if not match or len(values) == 4:
            return []
        raw = match[0]
        try:
            value = json.loads(raw) if raw.startswith('"') else raw
        except ValueError:
            return []
        end = match.end()
        if end < len(line) and line[end] not in " \t":
            return []
        values.append((value, start, end))
    return values


def guided(parts):
    words = [value for value, _, _ in parts]
    if not words or words[0] not in GUIDED or any(len(word.encode()) > 4096 for word in words):
        return None
    if words[0] == "known-domain" and len(words) == 2:
        return dict(selector=words[0], predicate=words[1], destination="known")
    if len(words) == 4 and words[0] != "known-domain" and words[2] in ("move", "save"):
        return dict(selector=words[0], predicate=words[1], destination=words[3])
    return None


def lines(text):
    # splitlines() also splits valid Unicode characters inside tokens.
    return io.StringIO(text, newline="\n")


def mapping(parts):
    if len(parts) == 3 and parts[0][0] == "mailbox":
        return dict(logical=parts[1][0], mailbox=parts[2][0])
    return None


def catalog(data):
    name = source_name(data)
    text = source(name)
    offset = data.get("offset", 0)
    if type(offset) is not int or offset < 0:
        raise Problem("invalid_request")
    rows = []
    next_offset = None
    for index, line in enumerate(lines(text or "")):
        if index < offset:
            continue
        line = line.rstrip("\r\n")
        if not line.strip():
            continue
        parts = tokens(line)
        if name == "FilterProfile.txt":
            entry = mapping(parts)
            if entry is None:
                continue
            row = dict(line=index + 1, mapping=entry)
        else:
            rule = guided(parts)
            row = dict(line=index + 1, rule=rule, label=line[:240] if not rule else "")
        if len(rows) == 40:
            next_offset = index
            break
        rows.append(row)
    return dict(revision=revision(text), rows=rows, next=next_offset)


def token_text(value):
    if not isinstance(value, str) or not value or len(value.encode()) > 4096 or re.search(r"[\x00-\x1f\x7f]", value):
        raise Problem("invalid_policy")
    if not value.startswith("#") and not re.search(r'[\s"]', value):
        return value
    return json.dumps(value, ensure_ascii=False)


def append_record(text, words):
    ending = "\r\n" if "\r\n" in text else "\n"
    return text + (ending if text and not text.endswith("\n") else "") + " ".join(words) + ending


def record_at(text, index):
    if type(index) is not int or index < 1:
        raise Problem("stale_policy")
    offset = 0
    for number, line in enumerate(lines(text), 1):
        if number == index:
            break
        offset += len(line)
    else:
        raise Problem("stale_policy")
    return offset, line


def mapping_candidate(text, data):
    index = data.get("line")
    remove = data.get("remove") is True
    if index is None:
        if remove:
            raise Problem("invalid_request")
        if not text:
            text = run([sys.argv[2], "policy", "default-profile"]).stdout.decode()
        return append_record(text, ["mailbox", token_text(data.get("logical")), token_text(data.get("mailbox"))])
    offset, line = record_at(text or "", index)
    parts = tokens(line.rstrip("\r\n"))
    if not mapping(parts):
        raise Problem("invalid_policy")
    if remove:
        remainder = line[parts[-1][2]:]
        replacement = line[:parts[0][1]] + remainder if remainder.lstrip(" \t").startswith("#") else ""
    else:
        replacement = line
        for (value, start, end), new in zip(reversed(parts[1:]), [data.get("mailbox"), data.get("logical")]):
            if value != new:
                replacement = replacement[:start] + token_text(new) + replacement[end:]
    return text[:offset] + replacement + text[offset + len(line):]


def candidate(text, data):
    selector = data.get("selector")
    if not isinstance(selector, str) or selector not in GUIDED:
        raise Problem("invalid_policy")
    predicate = data.get("predicate")
    tokens_new = [selector, token_text(predicate)]
    dest = data.get("destination")
    if selector != "known-domain":
        tokens_new.extend(["move", token_text(dest)])
    index = data.get("line")
    if index is None:
        return append_record(text, tokens_new)
    offset, line = record_at(text, index)
    original_size = len(line)
    old = tokens(line.rstrip("\r\n"))
    if not guided(old):
        raise Problem("invalid_policy")
    if len(old) == len(tokens_new):
        values = [selector, predicate] if len(old) == 2 else [selector, predicate, old[2][0], dest]
        for (value, start, end), new, encoded in reversed(list(zip(old, values, tokens_new))):
            if value != new:
                line = line[:start] + encoded + line[end:]
    else:
        line = line[:old[0][1]] + " ".join(tokens_new) + line[old[-1][2]:]
    return text[:offset] + line + text[offset + original_size:]


def run(args, input=None, partial=False):
    result = subprocess.run(args, input=input, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if result.returncode and not (partial and result.returncode == 7):
        raise Problem({2:"invalid_policy", 4:"invalid_policy", 5:"unpublished_policy", 6:"stale_policy"}.get(result.returncode, "backend_failure"))
    return result


def sample(data):
    row = data["occurrence"]  # verified signed selection from the service
    path = Path(*([] if row["folder"] == "." else [row["folder"]]), row["part"], row["name"])
    st = os.stat(ROOT / path, follow_symlinks=False)
    revision = digest(str((st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)).encode())
    if revision != row["revision"] or not stat.S_ISREG(st.st_mode):
        raise Problem("invalid_resource")
    return str(path)


def edit(op, data):
    name = source_name(data)
    text = source(name)
    if data.get("revision") != revision(text):
        raise Problem("stale_policy")
    body = (mapping_candidate(text, data) if name == "FilterProfile.txt" else candidate(text, data)).encode()
    path = sample(data)
    args = [sys.argv[2], "policy", op]
    if op == "publish":
        tok = data.get("preview")
        if not isinstance(tok, str) or not HEX.fullmatch(tok):
            raise Problem("invalid_request")
        args.extend(["--expect", tok])
    args.extend(["--", name, path])
    result = json.loads(run(args, body).stdout)
    if result["version"] != 1 or not HEX.fullmatch(result["token"]) or not HEX.fullmatch(result["generation"]):
        raise Problem("invalid_response")
    if op == "preview":
        if text != source(name):
            raise Problem("stale_policy")
        sample(data)
        result["decision"]["source"] = Path(result["decision"]["source"]).name
    return result


def published():
    parts = run([*COMPILER, "--check", "--report0"]).stdout.split(b"\0")
    if len(parts) != 5 or parts[0] != b"JFC1" or not HEX.fullmatch(parts[1].decode()):
        raise Problem("invalid_response")
    generation = parts[1].decode()
    try:
        selected = os.readlink(ENGINE / "current")
    except FileNotFoundError:
        raise Problem("unpublished_policy")
    if selected != "generations/" + generation:
        raise Problem("unpublished_policy")
    return generation, ENGINE / selected / "Filters.sieve"


def batch(op, data):
    generation, script = published()
    if op == "apply" and data.get("generation") != generation:
        raise Problem("stale_policy")
    if op == "explain":
        message = ROOT / sample(data)
        folder = message.parent.parent
    else:
        folder = ROOT / data["folder"]  # checked against native folder inventory by the service
    if folder.resolve(strict=True) != folder:
        raise Problem("invalid_resource")
    args = [WORK / "bin/jaro-sieve-filter", "--from", folder, "--to", ROOT, "--report0", generation]
    if op != "apply":
        args.append("--dry-run")
    if op == "explain":
        args.extend(["--message", message])
    # The native batch supports 100,000 messages. Parse its complete framed
    # output incrementally and return counts plus a bounded display sample.
    with tempfile.TemporaryFile() as report:
        rc = subprocess.run([*args, script], stdout=report, stderr=subprocess.DEVNULL).returncode
        if rc not in (0, 7):
            raise Problem("unpublished_policy" if rc == 5 else "filter_batch")
        report.seek(0)
        def fields():
            tail = b""
            while chunk := report.read(65536):
                items = (tail + chunk).split(b"\0")
                tail = items.pop()
                if len(tail) > 65536:
                    raise Problem("invalid_response")
                yield from items
            if tail:
                raise Problem("invalid_response")
        iterator = iter(fields())
        if next(iterator, None) != b"JFD1" or next(iterator, None) != generation.encode():
            raise Problem("invalid_response")
        counts = dict(classified=0, moved=0, noop=0, error=0)
        decisions = []
        index = 0
        for first in iterator:
            row = [next(iterator, None) for _ in range(10)]
            index += 1
            if first != str(index).encode() or None in row or row[6].decode() not in counts:
                raise Problem("invalid_response")
            status = row[6].decode()
            counts[status] += 1
            if len(decisions) < 30:
                decisions.append(dict(message=Path(os.fsdecode(row[0])).name, rule=row[1].decode(),
                    folder=row[3].decode(), status=status, source=Path(os.fsdecode(row[7])).name,
                    line=int(row[8]), detail=row[9].decode()))
        if bool(counts["error"]) != bool(rc):
            raise Problem("invalid_response")
    if op == "explain":
        sample(data)
    return dict(generation=generation, counts=counts, decisions=decisions, total=index)


def transfer(op, data):
    path = Path(data["file"])
    if op == "export":
        generation, script = published()
        content = read(script)
        manifest = json.loads(read(script.parent / "manifest.json"))
        if manifest.get("generation") != generation or manifest.get("sieve_sha256") != digest(content):
            raise Problem("unpublished_policy")
        with path.open("xb") as target:
            target.write(content)
        return {"generation":generation}
    args = [*COMPILER, "--import-sieve", path, "--report0"]
    if data.get("legacy") is True:
        args.append("--legacy-generated")
    if op == "check-import":
        args.append("--check")
    return compile_report(args)


def compile_report(args):
    fields = run(args).stdout.split(b"\0")
    if len(fields) != 5 or fields[0] != b"JFC1" or not HEX.fullmatch(fields[1].decode()):
        raise Problem("invalid_response")
    return {"generation":fields[1].decode()}

def main():
    op, data = sys.argv[1], json.load(sys.stdin)
    if op == "catalog":
        return catalog(data)
    if op in ("preview", "publish"):
        return edit(op, data)
    if op in ("explain", "dry-run", "apply"):
        return batch(op, data)
    if op == "refresh-policy":
        return compile_report([*COMPILER, "--report0"])
    if op in ("export", "check-import", "import"):
        return transfer(op, data)
    raise Problem("not_found")


if __name__ == "__main__":
    try:
        print(json.dumps(main()))
    except Problem as error:
        print(json.dumps({"error":error.code}))
    except (FileNotFoundError, NotADirectoryError):
        print(json.dumps({"error":"invalid_resource"}))
    except (TypeError, ValueError, KeyError, OSError):
        print(json.dumps({"error":"invalid_request"}))
