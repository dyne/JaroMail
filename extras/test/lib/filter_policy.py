"""Real CLI characterization, deliberately separate from authored target data.

No Sieve interpreter is implemented here: emitted text is checked as text only.
Future independent sieve-test/native execution must consume target.json directly.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    repo, scratch, work = map(Path, sys.argv[1:])
    fixtures = repo / "extras/test/fixtures/filter-policy"
    target = json.loads((fixtures / "target.json").read_text())
    legacy = json.loads((fixtures / "legacy.json").read_text())
    cases = target["cases"]
    all_cases = cases + target["domain_cases"] + target["isolated_domain_cases"]
    assert len(cases) == 23 and len(all_cases) == 38
    assert len({c["id"] for c in all_cases}) == len(all_cases)
    assert set(legacy["destinations"]) == {c["id"] for c in cases}
    for c in all_cases:
        assert c["rule"] and c["local"] and c["sieve"]
        assert c["sieve"] == {"known": "INBOX", "zz.spam": "Spam"}.get(c["local"], c["local"])

    root = scratch / "policy-mail"
    env = dict(os.environ, JAROMAILDIR=str(root), JAROWORKDIR=str(work))
    commands = []

    def cli(*args, stdin=None, expected=0):
        p = subprocess.run([str(repo / "src/jaro"), *args], env=env,
                           input=stdin, text=True, capture_output=True, timeout=120)
        commands.append({"args": args, "exit": p.returncode, "stdout": p.stdout, "stderr": p.stderr})
        (scratch / "filter-policy-commands.json").write_text(json.dumps(commands, indent=2))
        assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
        return p

    def maildir(name):
        for sub in ("new", "cur", "tmp"):
            (root / name / sub).mkdir(parents=True, exist_ok=True)

    def digest(data):
        return hashlib.sha256(data).hexdigest()

    def messages():
        return [p for p in root.glob("*/*/*") if p.is_file() and p.parent.name in ("new", "cur")]

    cli("init")
    (root / "Filters.txt").write_text("\n".join(target["rules"]) + "\n")
    (root / "Aliases.txt").write_text("own@example.org\n")
    for address in target["known"]:
        cli("import", stdin=f"Known <{address}>\n")
    for address in target["blacklist"]:
        cli("-l", "blacklist", "import", stdin=f"Blocked <{address}>\n")
    for dest in {c["local"] for c in cases} | set(legacy["destinations"].values()) | {"incoming", "oldbucket", "secondbucket"}:
        maildir(dest)
    cli("sieve")
    emitted = (root / "Filters.sieve").read_text()
    offsets = [emitted.index(fragment) for fragment in legacy["sieve_fragments"]]
    assert offsets == sorted(offsets), "legacy emitted stage order"
    assert '"duplicate@example.org" { fileinto :create "lastbucket"' in emitted
    assert 'fileinto :create "oldbucket"' not in emitted
    assert '"overlap" { fileinto :create "firstbucket"' in emitted
    assert '"overlap@example.org" { fileinto :create "secondbucket"' in emitted
    print("PASS legacy-sieve-emission: 8 ordered stage fragments, duplicate last winner, both overlaps")
    report_dir = os.environ.get("JARO_FILTER_POLICY_REPORT")
    if report_dir:
        Path(report_dir).mkdir(parents=True, exist_ok=True)
        (Path(report_dir) / "legacy-sieve.txt").write_text(emitted)
    original = {}
    for i, c in enumerate(cases):
        headers = {"Message-ID": f"<{c['id']}@fixtures.invalid>", "Subject": c["id"], **c["headers"]}
        data = ("\n".join(f"{k}: {v}" for k, v in headers.items()) + "\n\nBody with unchanged bytes.\n").encode()
        original[c["id"]] = digest(data)
        (root / "incoming" / ("cur" if i % 2 else "new") / (c["id"] + ":2,S")).write_bytes(data)
    cli("filter", str(root / "incoming"))
    actual = {}
    for p in messages():
        data = p.read_bytes()
        ident = next(line[len("Message-ID: <"):-len("@fixtures.invalid>")] for line in data.decode().splitlines() if line.startswith("Message-ID: <"))
        assert ident not in actual, f"duplicate message: {ident}"
        assert digest(data) == original[ident], f"content changed: {ident}"
        actual[ident] = p.parent.parent.name
    assert actual == legacy["destinations"], (actual, legacy["destinations"])
    if report_dir:
        (Path(report_dir) / "message-observations.json").write_text(json.dumps({
            "actual": actual, "input_sha256": original,
            "target": {c["id"]: {k: c[k] for k in ("rule", "local", "sieve")} for c in cases}
        }, indent=2, sort_keys=True) + "\n")
    casing_observed = {}
    for field in ("To", "Cc"):
        ident = "own-uppercase-" + field.lower()
        data = (f"Message-ID: <{ident}@fixtures.invalid>\nFrom: stranger@example.net\n{field}: OWN@EXAMPLE.ORG\n\nCasing body unchanged.\n").encode()
        (root / "incoming/new" / ident).write_bytes(data)
        cli("filter", str(root / "incoming"))
        found = [p for p in messages() if p.read_bytes() == data]
        assert len(found) == 1, ident
        casing_observed[ident] = found[0].parent.parent.name
        assert casing_observed[ident] == legacy["casing_destinations"][ident]
    if report_dir:
        (Path(report_dir) / "casing-observations.json").write_text(json.dumps(casing_observed, indent=2) + "\n")
    print(f"PASS legacy-message-decisions: {len(actual)} IDs plus 2 own-casing probes, destinations, exits, byte hashes")

    # Empty files and empty maildirs have separate explicit legacy observations.
    maildir("empty-probe")
    empty = root / "empty-probe/new/empty"
    empty.touch()
    cli("filter", str(root / "empty-probe"))
    assert not empty.exists() and actual == legacy["destinations"]
    cli("filter", str(root / "empty-probe"), expected=legacy["empty_maildir_exit"])
    print("PASS legacy-empty-input: empty file deletion and empty Maildir exit")

    # Shipped defaults: all 14 active records compile and emit; From extraction
    # remains defective, even though the generated Sieve contains the route.
    defaults = (repo / "docs/Filters.txt").read_text()
    active = [line.split() for line in defaults.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    assert len(active) == 14
    (root / "Filters.txt").write_text(defaults)
    cli("sieve")
    emitted = (root / "Filters.sieve").read_text()
    for selector, pattern, action, destination in active:
        assert selector == "from" and action == "save" and destination == "zz.social"
        assert f'"{pattern}" {{ fileinto :create "zz.social"' in emitted
    data = b"Message-ID: <default@fixtures.invalid>\nFrom: bot@github.com\nTo: nobody@example.net\n\nDefault body.\n"
    (root / "incoming/new/default").write_bytes(data)
    cli("filter", str(root / "incoming"))
    found = [p for p in messages() if p.read_bytes() == data]
    assert len(found) == 1 and found[0].parent.parent.name == "unsorted"
    # A fresh root avoids same-second legacy .zwc reuse; cache publication is
    # covered by later native-cache milestones, not hidden by sleeping here.
    root = scratch / "empty-rules-mail"
    env["JAROMAILDIR"] = str(root)
    cli("init")
    # Empty rules retain the builtin generated stages and valid fallback.
    (root / "Filters.txt").write_text("")
    cli("sieve")
    assert 'fileinto :create "zz.social"' not in (root / "Filters.sieve").read_text()
    data = b"Message-ID: <no-rules@fixtures.invalid>\nFrom: bot@github.com\n\nNo rules.\n"
    (root / "incoming/new/no-rules").write_bytes(data)
    cli("filter", str(root / "incoming"))
    assert len([p for p in messages() if p.read_bytes() == data and p.parent.parent.name == "unsorted"]) == 1
    if report_dir:
        (Path(report_dir) / "cli-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    print("PASS legacy-defaults-empty-rules: 14 shipped rules, exact IDs and byte integrity")
    print("PASS target-fixture-integrity: 38 unique independently authored decisions; domain execution deferred to native/oracle milestones")
    print("Filter policy checks: 5 passed, 0 failed, 0 skipped; 27 nonempty CLI message decisions + 2 empty-input probes")


if __name__ == "__main__":
    main()
