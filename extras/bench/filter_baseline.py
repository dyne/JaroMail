"""Deterministic, correctness-checked filter benchmarks; no live mail/services.

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
    root.mkdir(mode=0o700, parents=True)
    (root / "Accounts").mkdir()
    (root / "cache").mkdir()
    (root / "Accounts/default.txt").write_text(
        "name Benchmark Fixture\nemail own@example.net\nlogin own@example.net\nimap fixture.invalid\n")
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
        target[ident] = "from-routes" if kind == "conflict" else "INBOX" if kind == "known-only" else expected[ident]
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


def generation_snapshot(root):
    """Stable publication identity, excluding lock/access times."""
    engine = root / "cache/filter-engine"
    # A successful transaction deliberately retains its committed journal until
    # the next writer cleans recovery markers. Those markers are not products.
    paths = [root / "Filters.sieve", engine / "current"]
    paths += sorted((engine / "generations").rglob("*"))
    return {str(p.relative_to(root)): [p.lstat().st_ino, p.lstat().st_mtime_ns,
             os.readlink(p) if p.is_symlink() else sha(p.read_bytes())]
            for p in paths if p.is_symlink() or p.is_file()}


def native_decisions(result, fixture):
    """Assert every actual CLI decision against the independently authored table."""
    trace = Path(result["stdout"]).read_text()
    rows = re.findall(r"^\d+ / \d+ -> (.*?) \[classified\] path=(.*?) rule=", trace, re.M)
    found = {}
    for destination, path in rows:
        ident = re.search(r"message-\d{5}", path)
        assert ident and ident[0] not in found, (destination, path)
        # Zsh's diagnostic quoting is harmless for these ASCII fixture names.
        destination = destination.strip("$'\"")
        found[ident[0]] = destination
    assert found == fixture["target_expected"], "native dry decision/ID accounting mismatch"
    assert f'Native filter: {fixture["messages"]} processed, 0 errors, helper status 0' in Path(result["stderr"]).read_text()
    return dict(Counter(found.values()))


def delivery_benchmark(args):
    """Common literal-route workload, usable unchanged on base and candidates."""
    assert args.runs >= 5, 'delivery baseline requires at least five repetitions'
    assert all(n >= 1 for n in args.sizes + args.messages)
    assert args.message_bytes >= 256
    args.scratch.mkdir(parents=True, exist_ok=False)
    args.output.mkdir(parents=True, exist_ok=True)
    assert not any(args.output.iterdir()), 'output directory must be empty'
    compiler = args.work / 'bin/jaro-filter-compile'
    sieve = args.delivery_runtime == 'sieve'
    batch = args.work / 'bin' / ('jaro-sieve-filter' if sieve else 'jaro-filter')
    cli = args.work.parents[1] / 'bin/jaro'
    assert all(p.is_file() for p in (compiler, batch, cli))
    references = {}
    if args.reference_fixtures:
        references = {item['workload']: item for item in json.loads(args.reference_fixtures.read_bytes())}
    metadata = dict(schema=3, source_root=str(args.repo), runtime=str(args.work),
                    engine=args.delivery_runtime,
                    reference_fixtures_sha256=sha(args.reference_fixtures.read_bytes()) if args.reference_fixtures else None,
                    runtime_sha256={str(p): sha(p.read_bytes()) for p in (compiler, batch, cli)},
                    source_revision=args.revision, host=platform.platform(), cpus=os.cpu_count(),
                    environment={key: os.environ.get(key) for key in ('PATH', 'LD_LIBRARY_PATH', 'CCACHE_DIR', 'CFLAGS')},
                    command_path=str(args.work / 'bin') + os.pathsep + os.environ['PATH'],
                    cache='Warm OS caches; cold means absent application generation only. No cache eviction.',
                    memory='wait4 descendant peak RSS includes launcher floor; no isolated engine RSS claim',
                    schedule='Sequential repeats alternate single then batch, reversing order on even repeats; restoration excluded',
                    filesystem=subprocess.check_output(['df', '-T', str(args.scratch)], text=True),
                    build='Caller supplies matched uninstrumented Release installed runtime; build logs retained separately',
                    harness_sha256=sha(Path(__file__).read_bytes()), runs=args.runs)
    write_json(args.output / 'environment.json', metadata)
    samples, fixtures = [], []
    for rules in args.sizes:
        for count in args.messages:
            for device in args.delivery_devices:
                for repeat in range(args.runs):
                    for mode in [m for m in (('single', 'batch') if repeat % 2 == 0 else ('batch', 'single')) if m in args.delivery_modes]:
                        label = f'r{rules}-m{count}-b{args.message_bytes}-{device}-{mode}-{repeat+1}'
                        root = args.scratch / label
                        root.mkdir()
                        (root / 'Accounts').mkdir()
                        (root / 'Accounts/default.txt').write_text('name Benchmark\nemail own@example.net\nlogin fixture\nproto pop\nhost fixture.invalid\nport 995\n')
                        policy = ''.join(f'to-address target{i}@example.org move receipts\n' for i in range(rules))
                        (root / 'Filters.txt').write_text(policy)
                        (root / 'Aliases.txt').write_text('')
                        for book in ('whitelist.abook', 'blacklist.abook'):
                            (root / book).write_text('')
                        qualification = sha((args.repo / 'docs/filter-regex-qualification.json').read_bytes())
                        (root / 'FilterProfile.txt').write_text('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\ncapability mailbox\ncapability regex\nregex-qualification ' + qualification + '\n')
                        source = root
                        if device == 'cross':
                            assert args.cross_root is not None, '--cross-root required'
                            source = args.cross_root / label
                            source.mkdir(parents=True, exist_ok=False)
                            assert source.stat().st_dev != root.stat().st_dev, 'cross fixture must use distinct devices'
                        maildir(root, 'receipts')
                        paths, payloads = [], []
                        for i in range(count):
                            box = 'incoming' if mode == 'batch' else f'incoming-{i:05d}'
                            maildir(source, box)
                            path = source / box / ('cur' if i % 2 else 'new') / f'message-{i:05d}:2,FS'
                            header = f'From: stranger@example.net\nTo: target{i % rules}@example.org\nMessage-ID: <delivery-{i}@benchmark.invalid>\nSubject: literal delivery\n\n'.encode()
                            raw = header + (bytes([65 + i % 26]) * (args.message_bytes - len(header)))
                            assert len(raw) == args.message_bytes
                            paths.append(path); payloads.append(raw)
                        inventory = [dict(source=str(path.relative_to(source)), destination='receipts/cur/' + path.name,
                                          sha256=sha(raw), bytes=len(raw), flags='FS') for path, raw in zip(paths, payloads)]
                        identity = sha(json.dumps(dict(policy=sha(policy.encode()), inventory=inventory), sort_keys=True).encode())
                        fixture = dict(workload=label, input_sha256=identity, policy_sha256=sha(policy.encode()),
                                       inventory=inventory, source_device=source.stat().st_dev, destination_device=root.stat().st_dev)
                        if args.reference_fixtures:
                            assert fixture == references[label], f'input inventory or filesystem differs for {label}'
                        fixtures.append(fixture)
                        write_json(args.output / 'fixtures.json', fixtures)
                        env = dict(os.environ, PATH=metadata['command_path'], JAROMAILDIR=str(root), JAROWORKDIR=str(args.work), JAROACCOUNT=str(root / 'Accounts'), JARO_FILTER_ENGINE=args.delivery_runtime)

                        def restore():
                            for path in paths:
                                if path.exists(): path.unlink()
                            for path in (root / 'receipts/cur').iterdir(): path.unlink()
                            for path, raw in zip(paths, payloads): path.write_bytes(raw)

                        def verify(moved):
                            actual = {}
                            for base in set((root, source)):
                                for path in base.rglob('*'):
                                    if path.is_file() and path.parent.name in ('cur', 'new', 'tmp'):
                                        actual[str(path)] = sha(path.read_bytes())
                            expected = {str(root / item['destination'] if moved else path): item['sha256'] for path, item in zip(paths, inventory)}
                            assert actual == expected, 'physical path/digest/occurrence inventory mismatch'

                        phases = ('cold-compile', 'unchanged-update', 'readonly-check', 'pin-validation', 'classify', 'helper-apply', 'jaro-filter')
                        for phase in phases:
                            restore()
                            publication_before = generation_snapshot(root) if phase != 'cold-compile' else None
                            commands = [[str(compiler), '--root', str(root), '--accounts-dir', str(root / 'Accounts'), '--report0']]
                            if phase == 'readonly-check': commands[0].append('--check')
                            elif not sieve and phase == 'pin-validation':
                                empty = root / 'empty.paths'
                                empty.write_bytes(b'')
                                commands = [[str(batch), '--root', str(root), '--files0-from', str(empty)]]
                            elif not sieve and phase in ('classify', 'helper-apply'):
                                head = [str(batch), '--root', str(root), '--mode', 'classify' if phase == 'classify' else 'apply']
                                commands = [head + ['--', str(path)] for path in paths] if mode == 'single' else [head + ['--maildir', str(paths[0].parent.parent)]]
                            elif phase in ('pin-validation', 'classify', 'helper-apply'):
                                generation = (root / 'cache/filter-engine/current').resolve()
                                sources = [path.parent.parent for path in paths] if mode == 'single' else [paths[0].parent.parent]
                                if phase == 'pin-validation':
                                    blank = root / 'empty'
                                    maildir(root, 'empty')
                                    sources = [blank]
                                commands = [[str(batch), '--from', str(box), '--to', str(root),
                                             '--report0', generation.name, *(['--dry-run'] if phase != 'helper-apply' else []),
                                             str(generation / 'Filters.sieve')] for box in sources]
                            elif phase == 'jaro-filter':
                                commands = [[str(cli), 'filter', str(path.parent.parent)] for path in paths] if mode == 'single' else [[str(cli), 'filter', str(paths[0].parent.parent)]]
                            results = []
                            for invocation, command in enumerate(commands):
                                result = run_command(command, env, args.repo, args.output / f'{label}-{phase}-{invocation:05d}')
                                if phase in ('cold-compile', 'unchanged-update', 'readonly-check'):
                                    magic, pinned, changed, directory, trailer = Path(result['stdout']).read_bytes().split(b'\0')
                                    assert magic == b'JFC1' and trailer == b''
                                    assert changed == (b'1' if phase == 'cold-compile' else b'0')
                                    current = (root / 'cache/filter-engine/current').resolve()
                                    assert pinned == os.fsencode(current.name)
                                    assert directory == (b'' if phase == 'readonly-check' else os.fsencode(current))
                                if phase in ('classify', 'helper-apply'):
                                    fields = Path(result['stdout']).read_bytes().split(b'\0')
                                    assert fields[0] == b'JFD1' and fields[-1] == b''
                                    rows = fields[2:-1]
                                    selected = [paths[invocation]] if mode == 'single' else sorted(paths, key=os.fsencode)
                                    assert len(rows) == 11 * len(selected)
                                    for offset, path in enumerate(selected):
                                        row = rows[offset*11:(offset+1)*11]
                                        assert row[1] == os.fsencode(path) and row[5] == os.fsencode(root / 'receipts')
                                        assert row[7] == (b'classified' if phase == 'classify' else b'moved')
                                results.append(result)
                            moved = phase in ('helper-apply', 'jaro-filter')
                            verify(moved)
                            publication_after = generation_snapshot(root)
                            assert publication_after
                            if publication_before is not None:
                                assert publication_after == publication_before, 'unchanged fixture republished generation'
                            wall = sum(r['wall_seconds'] for r in results)
                            products = root / 'cache/filter-engine/generations'
                            samples.append(dict(workload=label, rules=rules, messages=count, message_bytes=args.message_bytes,
                                                device=device, mode=mode, repeat=repeat+1, phase=phase, input_sha256=identity,
                                                wall_seconds=wall, messages_per_second=count/wall if phase in ('classify', 'helper-apply', 'jaro-filter') else None,
                                                maxrss=max(r['maxrss'] for r in results), cpu_seconds=sum(r['user_seconds']+r['system_seconds'] for r in results),
                                                database_bytes=sum(p.stat().st_size for p in products.rglob('*')
                                                                   if p.name in ('Filters.svbin', 'policy.bin', 'rules.cdb')),
                                                generation_product_bytes=sum(p.stat().st_size for p in products.rglob('*') if p.is_file()),
                                                publication_sha256=sha(json.dumps(publication_after, sort_keys=True).encode()), publication_unchanged=publication_before is not None,
                                                commands=results, verified_occurrences=count))
                            write_json(args.output / 'samples.json', samples)
                        print('PASS ' + label + ': 7 phases, exact physical inventory', flush=True)
    groups = defaultdict(list)
    for sample in samples:
        groups[tuple(sample[key] for key in ('rules', 'messages', 'message_bytes', 'device', 'mode', 'phase'))].append(sample)
    summaries = []
    for key, group in sorted(groups.items()):
        wall = [sample['wall_seconds'] for sample in group]
        summaries.append(dict(zip(('rules', 'messages', 'message_bytes', 'device', 'mode', 'phase'), key),
                              runs=len(group), median_wall_seconds=statistics.median(wall), minimum_seconds=min(wall), maximum_seconds=max(wall),
                              spread_seconds=max(wall)-min(wall), median_cpu_seconds=statistics.median(s['cpu_seconds'] for s in group),
                              median_maxrss=statistics.median(s['maxrss'] for s in group), maxrss_unit='KiB on Linux',
                              median_messages_per_second=statistics.median(s['messages_per_second'] for s in group) if group[0]['messages_per_second'] else None,
                              database_bytes=group[0]['database_bytes']))
    write_json(args.output / 'summary.json', summaries)
    print(f'Delivery benchmark: {len(samples)} phase samples passed; 0 failed; 0 skipped')


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
    parser.add_argument("--engine", choices=("legacy", "native", "sieve"), default="sieve")
    parser.add_argument('--delivery', action='store_true', help='Run the common literal-route delivery baseline')
    parser.add_argument('--delivery-runtime', choices=('sieve', 'native'), default='sieve',
                        help='Installed Sieve runtime (default) or historical native installation')
    parser.add_argument('--reference-fixtures', type=Path,
                        help='Require matching reference inventories, message digests and device IDs')
    parser.add_argument('--message-bytes', type=int, default=1024)
    parser.add_argument('--delivery-devices', nargs='+', choices=('same', 'cross'), default=['same'])
    parser.add_argument('--delivery-modes', nargs='+', choices=('single', 'batch'), default=['single', 'batch'])
    parser.add_argument('--cross-root', type=Path)
    parser.add_argument('--revision', help='Exact caller-verified source revision for delivery evidence')
    args = parser.parse_args()
    if args.delivery:
        delivery_benchmark(args)
        return
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
    profile_work = args.scratch / "profile-work"
    profile_work.mkdir()
    (profile_work / "bin").mkdir()
    for path in args.work.iterdir():
        if path.name != "bin":
            (profile_work / path.name).symlink_to(path, target_is_directory=path.is_dir())
    for path in (args.work / "bin").iterdir():
        target = profile_work / "bin" / path.name
        if path.name in ("jaro-filter-compile", "jaro-sieve-filter"):
            target.write_text("#!/bin/sh\nprintf '%s\\n' " + shlex.quote(path.name) + " >> \"$JARO_BENCH_EXEC_LOG\"\nexec " + shlex.quote(str(path)) + ' "$@"\n')
            target.chmod(0o700)
        else:
            target.symlink_to(path)
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
    metadata["measurement_source_sha256"] = {name: sha((args.repo / "extras/bench" / name).read_bytes())
        for name in ("filter_baseline.py", "filter-baseline.sh", "filter-stage.zsh")}
    write_json(args.output / "environment.json", metadata)
    metadata["engine"] = args.engine
    metadata["compiler_flags"]["scope"] = "All normal GNU/Linux application helpers including native filter helpers; default -O3"
    metadata["native_binary_sha256"] = {name: sha((args.work / "bin" / name).read_bytes()) for name in ("jaro-filter-compile", "jaro-sieve-filter") if (args.work / "bin" / name).exists()}
    metadata["profiler"]["limitations"] += " Installed absolute native/legacy helper invocations are additionally wrapped in a separate runtime; uninstrumented runtime remains untouched."
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
                    env["JARO_FILTER_ENGINE"] = args.engine
                    qualification = sha((args.repo / "docs/filter-regex-qualification.json").read_bytes())
                    profile = ('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\ncapability mailbox\ncapability regex\nregex-qualification ' + qualification + '\n')
                    (root / "FilterProfile.txt").write_text(profile)
                    metadata["supplemental_profile"] = {"sha256": sha(profile.encode()), "regex_qualification": qualification,
                        "reason": "Explicit export capability for unchanged baseline ERE inputs; original input-file hashes remain identical"}
                    write_json(args.output / "environment.json", metadata)
                    if profiled:
                        env["PATH"] = str(wrappers) + os.pathsep + env["PATH"]
                        env["JAROWORKDIR"] = str(profile_work)
                    before = scan(root)
                    for phase in PHASES:
                        publication_before = generation_snapshot(root) if phase != "update_filters-cold" else None
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
                        if phase in ("sieve-emission", "jaro-sieve-unchanged"):
                            emitted = (root / "Filters.sieve").read_text()
                            assert sum(emitted.count(f'"^to{i:04d}@example[.]org$"') for i in range(rules // 2)) == rules // 2
                            assert sum(emitted.count(f'"^from{i:04d}@example[.]org$"') for i in range(rules // 2)) == rules // 2
                            assert all(f'"known{i:04d}@contacts.invalid"' in emitted for i in range(rules))
                        now = scan(root)
                        if phase == "classification-dry":
                            assert now == before, "dry classification changed messages, names or destinations"
                            result["verified_decisions"] = native_decisions(result, fixture)
                        if phase in ("application", "index-cold", "index-unchanged"):
                            assert now[0] == fixture["target_expected"], "wrong destination/ID accounting"
                            assert now[1] == fixture["payload_sha256"], "message content changed"
                            result["verified_destinations"] = dict(Counter(now[0].values()))
                        if phase.startswith("index"):
                            found = subprocess.check_output(["notmuch", "--config=" + str(root / "notmuch-config"), "count", "*"], env=env, text=True)
                            assert int(found) == count, (phase, found, count)
                            result["indexed_count"] = int(found)
                        publication_after = generation_snapshot(root)
                        assert publication_after, "missing native generation"
                        result["publication_sha256"] = sha(json.dumps(publication_after, sort_keys=True).encode())
                        if publication_before is not None:
                            if publication_before != publication_after:
                                write_json(prefix.with_suffix('.publication-changes.json'), {
                                    'before': publication_before, 'after': publication_after})
                            assert publication_before == publication_after, "unchanged input unexpectedly republished"
                            result["publication_unchanged"] = True
                        if profiled and phase in ("classification-dry", "application"):
                            assert result["selected_exec_counts"].get("jaro-sieve-filter") == 1
                            assert result["selected_exec_counts"].get("jaro-filter-compile") == 1
                            assert not any(result["selected_exec_counts"].get(t, 0) for t in ("maddr", "mseq", "mpick"))
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
    if not args.no_profile:
        for phase in PHASES:
            counts = [r["selected_exec_counts"] for r in records if r["profiled"] and r["phase"] == phase]
            assert all(c == counts[0] for c in counts), ("selected subprocess growth", phase, counts)
    print(f"Benchmark checks: {len(records)} passed, 0 failed, 0 skipped; {len(manifests)} workloads; {args.runs} repetitions per mode")


if __name__ == "__main__":
    main()
