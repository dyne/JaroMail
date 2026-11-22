"""Count Pigeonhole calls on a private copy of a completed delivery benchmark.

Requires Linux/glibc's sotruss audit module. Traced calls never enter timings.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from filter_baseline import maildir, sha, write_json


def instrument(runtime, audit, traces):
    """Enable the dynamic loader's function-call tracer only in the real tools."""
    sums = runtime / "SHA256SUMS"
    manifest = sums.read_text()
    for name in ("sievec", "sieve-dump", "maildir-filter"):
        tool = runtime / "bin" / name
        original = tool.with_suffix(".real")
        tool.rename(original)
        digest = sha(original.read_bytes())
        tool.write_text(
            "#!/usr/bin/python3 -I\nimport os, sys\n"
            f"os.environ.update(LD_AUDIT={str(audit)!r}, SOTRUSS_TOLIST='libdovecot-sieve.so.0', "
            f"SOTRUSS_OUTNAME={str(traces / name)!r})\n"
            f"os.execv({str(original)!r}, sys.argv)\n")
        tool.chmod(0o755)
        entry = f"{digest}  bin/{name}\n"
        assert manifest.count(entry) == 1
        manifest = manifest.replace(entry, f"{sha(tool.read_bytes())}  bin/{name}\n")
    sums.write_text(manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="Installed share/jaromail")
    parser.add_argument("--fixture", type=Path, required=True, help="Completed --delivery batch fixture directory")
    parser.add_argument("--fixtures-json", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New private directory")
    parser.add_argument("--audit", type=Path, required=True, help="Absolute path to glibc's sotruss-lib.so")
    args = parser.parse_args()
    audit = args.audit.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(mode=0o700)
    reference, = [row for row in json.loads(args.fixtures_json.read_bytes()) if row["workload"] == args.fixture.name]
    records = reference["inventory"]
    assert records and all(Path(r["source"]).parts[0] == "incoming" for r in records), "batch fixture required"
    stage = output / "installation"
    shutil.copytree(args.work.resolve().parents[1], stage, symlinks=True)
    work = stage / "share/jaromail"
    traces = output / "traces"
    traces.mkdir()
    instrument(work / "sieve", audit, traces)
    root = output / "mail"
    root.mkdir()
    shutil.copytree(args.fixture / "Accounts", root / "Accounts")
    for name in ("Filters.txt", "Aliases.txt", "FilterProfile.txt", "whitelist.abook", "blacklist.abook"):
        shutil.copyfile(args.fixture / name, root / name)
    assert sha((root / "Filters.txt").read_bytes()) == reference["policy_sha256"]
    for name in ("incoming", "receipts", "empty"):
        maildir(root, name)
    for row in records:
        original = args.fixture / row["destination"]
        assert sha(original.read_bytes()) == row["sha256"]
        shutil.copyfile(original, root / row["source"])
    env = dict(os.environ, PATH=str(work / "bin") + os.pathsep + os.environ["PATH"],
               JAROWORKDIR=str(work), JAROMAILDIR=str(root), JAROACCOUNT=str(root / "Accounts"),
               JARO_FILTER_ENGINE="sieve")
    results = []
    symbols = ("sieve_compile", "sieve_generator_run", "sieve_save_as", "sieve_load", "sieve_interpreter_run")

    def run(name, argv, expected, moved=False):
        before = set(traces.iterdir())
        with (output / (name + ".stdout")).open("wb") as out, (output / (name + ".stderr")).open("wb") as err:
            subprocess.run(argv, env=env, stdout=out, stderr=err, check=True)
        counts = Counter()
        files = sorted(set(traces.iterdir()) - before)
        for path in files:
            for symbol in re.findall(rb":\s*\*?(sieve_\w+)\(", path.read_bytes()):
                symbol = symbol.decode("ascii")
                if symbol in symbols:
                    counts[symbol] += 1
        expected_mail = {row["destination" if moved else "source"]: row["sha256"] for row in records}
        actual_mail = {str(p.relative_to(root)): sha(p.read_bytes()) for p in root.rglob("*")
                       if p.is_file() and p.parent.name in ("new", "cur", "tmp")}
        assert actual_mail == expected_mail, name
        results.append(dict(phase=name, argv=list(map(str, argv)), counts=dict(counts),
                            traces=[p.name for p in files], verified_occurrences=len(records)))
        write_json(output / "counts.json", dict(input_sha256=reference["input_sha256"], results=results))
        assert counts == expected, (name, counts, expected)

    compiler = [work / "bin/jaro-filter-compile", "--root", root, "--accounts-dir", root / "Accounts", "--report0"]
    # Pinned sievec tries a named storage, then the file. Only the resolved
    # file yields an executable, which sieve-dump opens before publication.
    run("cold-compile", compiler, {"sieve_compile": 2, "sieve_save_as": 1, "sieve_load": 1})
    run("unchanged-update", compiler, {})
    run("readonly-check", [*compiler, "--check"], {})
    generation = (root / "cache/filter-engine/current").resolve()
    batch = work / "bin/jaro-sieve-filter"

    def helper(source, *options):
        return [batch, "--from", root / source, "--to", root, *options, "--report0", generation.name,
                generation / "Filters.sieve"]

    count = len(records)
    run("pin-validation", helper("empty", "--dry-run"), {"sieve_load": 1})
    cached = {"sieve_load": 1, "sieve_interpreter_run": count}
    run("classify", helper("incoming", "--dry-run"), cached)
    run("helper-apply", helper("incoming"), cached, moved=True)
    for row in records:
        (root / row["destination"]).rename(root / row["source"])
    run("jaro-filter", [stage / "bin/jaro", "filter", root / "incoming"], cached, moved=True)
    # Hand-authored source exercises the same worker's compile branch. A missing
    # generator trace here would invalidate zero compilation on the cached path.
    script = output / "uncached.sieve"
    shutil.copyfile(generation / "Filters.sieve", script)
    run("source-control", [batch, "--dry-run", "--from", root / "empty", "--to", root, script],
        {"sieve_generator_run": 1}, moved=True)
    print(f"{len(results)} profiled phases passed for {count} messages")


if __name__ == "__main__":
    main()
