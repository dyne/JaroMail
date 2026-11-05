"""Contract vectors and actual POSIX/Zsh transport checks, not native stubs.

These tests execute no future helper and do not interpret Sieve or classify
messages. Golden bytes are independent fixture data available to future helpers.
"""
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


def paths0(data):
    if not data:
        return []
    if not data.endswith(b"\0"):
        raise ValueError("unterminated path")
    paths = data[:-1].split(b"\0")
    if not all(paths):
        raise ValueError("empty path")
    return paths


def decisions0(data):
    if not data.endswith(b"\0"):
        raise ValueError("unterminated stream")
    fields = data[:-1].split(b"\0")
    if len(fields) < 2 or fields[0] != b"JFD1" or not re.fullmatch(b"[0-9a-f]{64}", fields[1]):
        raise ValueError("header")
    records = fields[2:]
    if len(records) % 11:
        raise ValueError("partial record")
    return fields[1].decode(), [records[i:i + 11] for i in range(0, len(records), 11)]


def main():
    fixture = json.loads(Path(sys.argv[1]).read_text())
    assert fixture["schema"] == 1
    assert len(fixture["path_lists"]) == 8 and len(fixture["decisions"]) == 5
    checked = 0
    for example in fixture["path_lists"]:
        data = bytes.fromhex(example["hex"])
        try:
            actual = paths0(data)
        except ValueError:
            assert not example["valid"], example["id"]
        else:
            assert example["valid"], example["id"]
            assert actual == [bytes.fromhex(p) for p in example["paths"]]
            assert b"".join(p + b"\0" for p in actual) == data
        checked += 1
    for example in fixture["decisions"]:
        try:
            gen, actual = decisions0(bytes.fromhex(example["hex"]))
        except ValueError:
            assert not example["valid"], example["id"]
        else:
            assert example["valid"] and gen == example["generation"], example["id"]
            assert actual == [[bytes.fromhex(f) for f in r] for r in example["records"]]
        checked += 1
    assert len(fixture["fingerprint_vectors"]) == 2
    fingerprints = []
    for vector in fixture["fingerprint_vectors"]:
        data = b"JFFP1\0" + b"".join(bytes.fromhex(r) for r in vector["records_hex"])
        assert hashlib.sha256(data).hexdigest() == vector["sha256"]
        fingerprints.append(vector["sha256"])
    assert fingerprints[0] != fingerprints[1], "case policy must affect fingerprint"
    print(f"PASS golden byte framing: {checked} wire vectors and 2 independently expected fingerprint vectors")

    # These are authored future behavior examples; validate their schema only.
    # No invented CLI/cache/import implementation is called to make them pass.
    assert len(fixture["invocations"]) == 12
    assert len(fixture["cache_header_examples"]) == 4
    assert len(fixture["import_examples"]) == 7
    for group in ("invocations", "cache_header_examples", "import_examples"):
        assert len({e["id"] for e in fixture[group]}) == len(fixture[group])
    for e in fixture["invocations"] + fixture["import_examples"]:
        assert e["expected_exit"] in (0, 2, 3, 4, 5, 6, 7, 127)
    for e in fixture["cache_header_examples"]:
        assert e["compatible_header"] == (e["magic"] == "JFNCACHE" and e["cache_version"] == e["policy_version"] == 1)
    casing = json.loads((Path(sys.argv[1]).parents[1] / "filter-policy/casing.json").read_text())
    assert casing["address_case"] == "ascii-mailbox-casemap-v1"
    assert casing["sieve_comparator"] == "i;ascii-casemap"
    assert fixture["cache_contract"]["address_case"] == casing["address_case"]
    assert fixture["cache_contract"]["sieve_address_comparator"] == casing["sieve_comparator"]
    assert fixture["cache_contract"]["metadata_field_count"] == 7
    assert len(casing["cases"]) == 14 and len({c["id"] for c in casing["cases"]}) == 14
    for c in casing["cases"]:
        assert c["rule"] and c["headers"] and c["local"]
        assert c["sieve"] == {"known":"INBOX", "zz.spam":"Spam"}.get(c["local"], c["local"])
    print("PASS schema only: 12 invocation, 4 header-version, 7 import and 14 casing targets; helper behavior NOT executed")

    with tempfile.TemporaryDirectory(prefix="jaro-filter-contract.", dir=os.environ.get("TMPDIR")) as scratch:
        env = dict(os.environ, LC_ALL="C", ZDOTDIR=scratch)
        raw_path = os.fsencode(scratch) + b"/path with space\n\xff"
        fd = os.open(raw_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(b"unchanged bytes\n")
        listed = paths0(raw_path + b"\0")
        assert listed == [raw_path]
        with open(listed[0], "rb") as f:
            assert f.read() == b"unchanged bytes\n"
        raw_stream = bytes.fromhex(fixture["decisions"][0]["hex"])
        transport = subprocess.run(["zsh", "-f", "-c", 'fields=(); while IFS= read -r -d "" field; do fields+=("$field"); done; printf "%s\\0" "${fields[@]}"'],
                                   input=raw_stream, capture_output=True, env=env, check=True)
        assert transport.stdout == raw_stream, "Zsh transport lost bytes/empty fields"
        print("PASS actual filesystem/Zsh framing: spaces, newline, non-UTF8 and empty fields preserved")

        # Prove the .zwc hazard and the chosen descriptor-source escape using
        # the real Zsh interpreter; this is not a model of generation loading.
        source = Path(scratch) / "compat name.zsh"
        source.write_text("typeset -g contract_marker=old\n")
        subprocess.run(["zsh", "-f", "-c", 'zcompile "$1"', "contract", str(source)], env=env, check=True)
        source.write_text("typeset -g contract_marker=new\n")
        os.utime(source, (time.time() - 20, time.time() - 20))
        result = subprocess.run(["zsh", "-f", "-c", 'source "$1"; print -r -- "$contract_marker"; exec {fd}<"$1"; source /dev/fd/$fd; exec {fd}<&-; print -r -- "$contract_marker"', "contract", str(source)],
                                env=env, capture_output=True, check=True)
        assert result.stdout == b"old\nnew\n", (result.stdout, result.stderr)
        print("PASS actual Zsh cache selection: stale .zwc precondition proven; descriptor reads current text")
        result = subprocess.run(["zsh", "-f", "-c", 'unsetopt RE_MATCH_PCRE CASE_MATCH; [[ ABC =~ ^abc$ ]]'], env=env)
        assert result.returncode == 0
        print("PASS actual legacy regex casing: clean POSIX ERE with CASE_MATCH off is case-insensitive")
    print("Filter contract checks: 5 passed, 0 failed, 0 skipped; schema/framing and actual transport checks only")


if __name__ == "__main__":
    main()
