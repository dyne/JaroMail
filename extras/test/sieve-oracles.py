#!/usr/bin/env python3
"""Run independently expected messages through the actual pinned interpreter."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import sys

sys.dont_write_bytecode = True
from sieve_oracle import FIXTURES, OracleFailure, SieveOracle, fileinto_result


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


def run(root):
    before = snapshot(FIXTURES)
    oracle = SieveOracle(root / "oracle with spaces", os.environ["SIEVE_DEPS_PREFIX"])
    print("PASS matching private configuration versions and no user database", flush=True)
    cases = json.loads((FIXTURES / "cases.json").read_text())["cases"]
    assert len(cases) == 39 and len({c["id"] for c in cases}) == 39
    oracle.compile(FIXTURES / "apex.sieve")
    print("PASS valid fileinto compiled to actual binary", flush=True)
    negative_count = 0
    for script, extensions in (("malformed.sieve", None), ("missing-capability.sieve", None),
                               ("apex.sieve", "-fileinto")):
        oracle.compile(FIXTURES / script, extensions=extensions, succeeds=False)
        negative_count += 1
        print("PASS rejected " + script + (" " + extensions if extensions else ""), flush=True)
    sources = root / "messages"
    sources.mkdir()
    for case in cases:
        message = sources / (case["id"] + ".eml")
        headers = ["%s: %s" % tuple(h) for h in case["headers"]]
        headers += ["Message-ID: <%s@oracle.example>" % case["id"], "Subject: Disposable oracle fixture"]
        message.write_text("\n".join(headers) + "\n\nBody has person@example.net and sender@example.org.\n")
        oracle.expect(FIXTURES / case["script"], message, case["mailbox"], case.get("create", False))
        print("PASS " + case["id"], flush=True)

    message = sources / "apex-exact.eml"
    wrong = root / "wrong.sieve"
    wrong.write_text('require "fileinto"; fileinto "deliberately wrong";\n')
    try:
        oracle.expect(wrong, message, "apex")
    except OracleFailure as error:
        assert "differs from expected" in str(error)
    else:
        raise AssertionError("oracle harness accepted a wrong destination")
    print("PASS deliberately wrong destination rejected", flush=True)
    # Two valid fileinto actions must fail the single-decision contract.
    wrong.write_text('require "fileinto"; fileinto "apex"; fileinto "extra";\n')
    try:
        oracle.expect(wrong, message, "apex")
    except OracleFailure as error:
        assert "exactly one fileinto" in str(error)
    else:
        raise AssertionError("oracle harness accepted multiple actions")
    print("PASS multiple interpreted actions rejected", flush=True)
    # A no-action script succeeds in upstream but performs implicit keep, which is not fileinto.
    wrong.write_text("stop;\n")
    try:
        oracle.expect(wrong, message, "apex")
    except OracleFailure as error:
        assert "exactly one fileinto" in str(error)
    else:
        raise AssertionError("oracle harness accepted implicit keep")
    print("PASS implicit keep rejected", flush=True)
    # Parser never infers success from a matching mailbox substring in arbitrary output.
    try:
        fileinto_result("apex\n")
    except OracleFailure:
        pass
    else:
        raise AssertionError("oracle accepted an unknown report")
    assert snapshot(FIXTURES) == before
    assert not any(oracle.work.joinpath("mail").iterdir())
    assert not any(oracle.work.joinpath("home").iterdir())
    print("PASS unknown report rejected; fixtures unchanged and no delivery", flush=True)
    total = 2 + negative_count + len(cases) + 4
    (root / "summary.json").write_text(json.dumps({"executed":total,"failed":0,"skipped":0,"total":total,
        "fixture_sha256":before,"oracle_record":str(oracle.work / "oracle.json")},indent=2,sort_keys=True)+"\n")
    print(f"Tests: {total} passed, 0 failed, 0 skipped")


if os.environ.get("SIEVE_ORACLE_EVIDENCE"):
    root = Path(os.environ["SIEVE_ORACLE_EVIDENCE"]).resolve()
    root.mkdir(parents=True, exist_ok=False)
    run(root)
else:
    with tempfile.TemporaryDirectory(prefix="jaro-sieve-oracle-") as temp:
        run(Path(temp))
