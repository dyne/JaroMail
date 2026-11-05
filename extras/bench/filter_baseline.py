"""Deterministic, correctness-checked legacy baseline; no live mail/services.

Timing runs have no executable wrappers. Separate profile runs transparently
exec selected PATH programs; their times are never used for speedup gates.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import time


TOOLS = "abook awk cat chmod cut date find grep hostname maddr mkdir mpick mseq mv notmuch realpath rm sed sort touch tr uniq wc".split()
PHASES = ("update_filters-cold", "update_filters-warm-unchanged", "sieve-emission",
          "jaro-sieve-unchanged", "classification-dry", "application", "index-cold", "index-unchanged")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def maildir(root, name):
    for sub in ("new", "cur", "tmp"):
        (root / name / sub).mkdir(parents=True, exist_ok=True)


def make_fixture(root, rules, messages):
    """Input bytes and independent decision tables, without shell generation."""
    root.mkdir(parents=True)
    (root / "Accounts").mkdir()
    (root / "cache").mkdir()
    (root / "Accounts/default.txt").write_text(
        "name Benchmark Fixture\nemail own@example.net\nlogin own@example.net\n")
    half = rules // 2
    text = [f"from ^from{i:04d}@example[.]org$ move from-routes" for i in range(half)]
    text += [f"to ^to{i:04d}@example[.]org$ move route-{i % 2}" for i in range(half)]
    (root / "Filters.txt").write_text("\n".join(text) + "\n")
    (root / "Aliases.txt").write_text("own@example.net\n")
    book = "# abook addressbook file\n[format]\nprogram=abook\nversion=0.6.1\n"
    book += "".join(f"\n[{i}]\nname=Known {i:04d}\nemail=known{i:04d}@contacts.invalid\n" for i in range(rules))
    (root / "whitelist.abook").write_text(book)
    (root / "blacklist.abook").write_text("# empty synthetic blacklist\n")
    for dest in ("incoming", "known", "unsorted", "from-routes", "route-0", "route-1", "priv", "zz.spam", "zz.blacklist"):
        maildir(root, dest)
    expected, target, payloads, distribution = {}, {}, {}, Counter()
    for i in range(messages):
        ident = f"message-{i:05d}"
        kind = ("match", "miss", "conflict", "known-only")[i % 4]
        distribution[kind] += 1
        n = (i // 4) % half
        sender = f"known{i % rules:04d}@contacts.invalid" if kind == "known-only" else f"from{n:04d}@example.org" if kind == "conflict" else "stranger@example.net"
        recipient = f"to{n:04d}@example.org" if kind in ("match", "conflict") else "reader@example.net"
        data = (f"Message-ID: <{ident}@benchmark.invalid>\nFrom: {sender}\nTo: {recipient}\n"
                f"Subject: {kind} {i:05d}\nDate: Thu, 01 Jan 1970 00:00:00 +0000\nX-Spam-Flag: NO\n\n"
                "Deterministic benchmark body.\n").encode()
        (root / "incoming" / ("cur" if i % 2 else "new") / (ident + ":2,S")).write_bytes(data)
        payloads[ident] = sha(data)
        expected[ident] = f"route-{n % 2}" if kind in ("match", "conflict") else "unsorted"
        target[ident] = "from-routes" if kind == "conflict" else "known" if kind == "known-only" else expected[ident]
    paths = [p for p in sorted(root.rglob("*")) if p.is_file()]
    inputs = {str(p.relative_to(root)): sha(p.read_bytes()) for p in paths}
    return {"rules": rules, "contacts": rules, "messages": messages,
            "distribution": dict(distribution), "inputs": inputs,
            "input_sha256": sha(json.dumps(inputs, sort_keys=True).encode()),
            "legacy_expected": expected, "target_expected": target, "payload_sha256": payloads}


def scan(root):
    found, content, paths = {}, {}, {}
    for box in root.iterdir():
        if not box.is_dir():
            continue
        for sub in ("new", "cur"):
            folder = box / sub
            if not folder.is_dir():
                continue
            for p in folder.iterdir():
                if not p.is_file():
                    continue
                data = p.read_bytes()
                ident = re.search(rb"^Message-ID: <([^@]+)@benchmark.invalid>$", data, re.M)
                assert ident, f"unexpected message {p}"
                ident = ident[1].decode()
                assert ident not in found, f"duplicate message ID {ident}"
                found[ident], content[ident] = box.name, sha(data)
                paths[str(p.relative_to(root))] = sha(data)
    return found, content, paths


def run_command(argv, env, cwd, prefix, stage_clock=None):
    start = time.perf_counter()
    with prefix.with_suffix(".stdout").open("wb") as out, prefix.with_suffix(".stderr").open("wb") as err:
        child = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=err)
        _, status, usage = os.wait4(child.pid, 0)
        child.returncode = os.waitstatus_to_exitcode(status)
    result = {"command": argv, "cwd": str(cwd), "exit": child.returncode,
              "wall_seconds": time.perf_counter() - start,
              "stage_seconds": float(stage_clock.read_text()) if stage_clock and stage_clock.exists() else None,
              "maxrss": usage.ru_maxrss, "maxrss_unit": "KiB" if platform.system() == "Linux" else "platform-native",
              "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
              "observedAt": datetime.now(timezone.utc).isoformat(),
              "stdout": str(prefix.with_suffix(".stdout")), "stderr": str(prefix.with_suffix(".stderr"))}
    assert child.returncode == 0, result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=[10, 100, 1000])
    parser.add_argument("--messages", type=int, nargs="+", default=[100, 1000])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--no-profile", action="store_true")
    args = parser.parse_args()
    assert args.runs >= 1 and all(n >= 2 and n % 2 == 0 for n in args.sizes)
    assert all(n >= 4 and n % 4 == 0 for n in args.messages)
    args.scratch.mkdir(parents=True, exist_ok=False)
    args.output.mkdir(parents=True, exist_ok=True)
    # Empty output is required: evidence must never be silently overwritten.
    assert not any(args.output.iterdir()), "output directory must be empty"
    wrappers = args.scratch / "profile-bin"
    wrappers.mkdir()
    resolved = {name: shutil.which(name) for name in TOOLS}
    for name, executable in resolved.items():
        if executable:
            path = wrappers / name
            path.write_text("#!/bin/sh\nprintf '%s\\n' " + shlex.quote(name) + " >> \"$JARO_BENCH_EXEC_LOG\"\nexec " + shlex.quote(executable) + ' "$@"\n')
            path.chmod(0o700)
    metadata = {"schema": 1, "host": {"node": platform.node(), "platform": platform.platform(), "machine": platform.machine(), "cpus": os.cpu_count()},
                "python": platform.python_version(), "environment": {k: os.environ.get(k) for k in ("PATH", "LD_LIBRARY_PATH", "CCACHE_DIR", "CC", "CFLAGS", "LC_ALL")},
                "compiler_flags": {"build_script_sha256": sha((args.repo / "build/build-gnu.sh").read_bytes()),
                                   "CC": os.environ.get("CC", "cc (Makefile/GNU Make default)"), "CFLAGS": os.environ.get("CFLAGS", "-O3 on GNU/Linux; -Os on linux-musl"),
                                   "CPPFLAGS": os.environ.get("CPPFLAGS", ""), "LDFLAGS": os.environ.get("LDFLAGS", "empty on GNU/Linux; -static on linux-musl"),
                                   "scope": "Existing C helpers only; no native filter compiler exists yet"},
                "runtime_versions": {name: subprocess.run([name, "--version"], text=True, capture_output=True).stdout.splitlines()[:1] for name in ("zsh", "abook", "notmuch", "gcc")},
                "runtime_binary_sha256": {name: sha(Path(executable).read_bytes()) for name, executable in resolved.items() if executable},
                "profiler": {"kind": "separate transparent PATH exec wrappers", "selected_executables": resolved,
                             "limitations": "Counts selected executable invocations only, not shell forks/builtins; bootstrap included. Wrapper times excluded from performance medians. No strace/perf installed."},
                "cache": "cold means absent application filter cache/index, never OS page-cache eviction; warm/unchanged means same content and existing application cache in a new process",
                "runs": args.runs, "phases": PHASES, "source_root": str(args.repo), "work_root": str(args.work)}
    write_json(args.output / "environment.json", metadata)
    records, manifests = [], []
    for rules in args.sizes:
        for count in args.messages:
            name = f"r{rules}-m{count}"
            seed = args.scratch / (name + "-fixture")
            fixture = make_fixture(seed, rules, count)
            manifests.append({"name": name, **fixture})
            write_json(args.output / "fixtures.json", manifests)
            for profiled in ([False] if args.no_profile else [False, True]):
                for repeat in range(args.runs):
                    label = f"{name}-{'profile' if profiled else 'time'}-{repeat + 1}"
                    root = args.scratch / label
                    shutil.copytree(seed, root)
                    assert all(sha((root / path).read_bytes()) == value for path, value in fixture["inputs"].items())
                    env = dict(os.environ, JAROMAILDIR=str(root), JAROWORKDIR=str(args.work))
                    if profiled:
                        env["PATH"] = str(wrappers) + os.pathsep + env["PATH"]
                    before = scan(root)
                    for phase in PHASES:
                        prefix = args.output / (label + "-" + phase)
                        clock = prefix.with_suffix(".clock")
                        exec_log = prefix.with_suffix(".exec")
                        env["JARO_BENCH_EXEC_LOG"] = str(exec_log)
                        jaro = [str(args.repo / "src/jaro")]
                        if phase.startswith("update_filters") or phase == "sieve-emission":
                            argv = ["zsh", str(args.repo / "extras/bench/filter-stage.zsh"), str(args.repo), "emission" if phase == "sieve-emission" else "update_filters", str(clock)]
                        elif phase == "jaro-sieve-unchanged":
                            argv = jaro + ["sieve"]
                        elif phase == "classification-dry":
                            argv = jaro + ["-n", "filter", str(root / "incoming")]
                        elif phase == "application":
                            argv = jaro + ["filter", str(root / "incoming")]
                        else:
                            argv = jaro + ["index"]
                        result = run_command(argv, env, args.repo, prefix, clock)
                        # Snapshot instrumentation before independent verification
                        # invokes notmuch count; checks never inflate stage counts.
                        result["selected_exec_counts"] = dict(Counter(exec_log.read_text().splitlines())) if exec_log.exists() else None
                        result.update(workload=name, rules=rules, contacts=rules, messages=count,
                                      repeat=repeat + 1, phase=phase, profiled=profiled, input_sha256=fixture["input_sha256"])
                        if phase.startswith("update_filters"):
                            cache = (root / "cache/filters").read_text()
                            assert len(re.findall(r"^filter_(?:from|to)\+=", cache, re.M)) == rules
                            assert len(re.findall(r"^filter_whitelist\+=", cache, re.M)) == rules
                            if profiled:
                                assert result["selected_exec_counts"].get("cat", 0) >= rules
                        if phase in ("sieve-emission", "jaro-sieve-unchanged"):
                            emitted = (root / "Filters.sieve").read_text()
                            assert sum(emitted.count(f'"^to{i:04d}@example[.]org$"') for i in range(rules // 2)) == rules // 2
                            assert sum(emitted.count(f'"^from{i:04d}@example[.]org$"') for i in range(rules // 2)) == rules // 2
                            assert all(f'"known{i:04d}@contacts.invalid"' in emitted for i in range(rules))
                        now = scan(root)
                        if phase == "classification-dry":
                            assert now == before, "dry classification changed messages, names or destinations"
                            trace = Path(result["stderr"]).read_text() + Path(result["stdout"]).read_text()
                            assert len(re.findall(r"-> route-[01]", trace)) == count // 2
                            assert len(re.findall(r"-> unsorted", trace)) == count
                            result["dry_trace"] = {"route_proposals": count // 2, "fallback_proposals": count,
                                                   "meaning": "legacy dry-run continues after route proposal; not one decision per message"}
                            if profiled:
                                assert result["selected_exec_counts"].get("maddr", 0) >= 2 * count
                        if phase in ("application", "index-cold", "index-unchanged"):
                            assert now[0] == fixture["legacy_expected"], "wrong destination/ID accounting"
                            assert now[1] == fixture["payload_sha256"], "message content changed"
                            result["verified_destinations"] = dict(Counter(now[0].values()))
                        if phase.startswith("index"):
                            found = subprocess.check_output(["notmuch", "--config=" + str(root / "notmuch-config"), "count", "*"], env=env, text=True)
                            assert int(found) == count, (phase, found, count)
                            result["indexed_count"] = int(found)
                        result["verified_messages"] = count
                        records.append(result)
                        write_json(args.output / "samples.json", records)
                    print(f"PASS {label}: {len(PHASES)} phases; {count} exact IDs/hashes/destinations", flush=True)
    summaries = []
    for workload in sorted({r["workload"] for r in records}):
        for phase in PHASES:
            samples = [r for r in records if r["workload"] == workload and r["phase"] == phase and not r["profiled"]]
            profiles = [r for r in records if r["workload"] == workload and r["phase"] == phase and r["profiled"]]
            summary = {"workload": workload, "phase": phase, "runs": len(samples),
                       "median_wall_seconds": statistics.median(r["wall_seconds"] for r in samples),
                       "median_stage_seconds": statistics.median(r["stage_seconds"] for r in samples) if all(r["stage_seconds"] is not None for r in samples) else None,
                       "median_maxrss": statistics.median(r["maxrss"] for r in samples), "maxrss_unit": samples[0]["maxrss_unit"],
                       "profile_runs": len(profiles), "selected_exec_counts": [r["selected_exec_counts"] for r in profiles]}
            summaries.append(summary)
    write_json(args.output / "summary.json", summaries)
    print(f"Benchmark checks: {len(records)} passed, 0 failed, 0 skipped; {len(manifests)} workloads; {args.runs} repetitions per mode")


if __name__ == "__main__":
    main()
