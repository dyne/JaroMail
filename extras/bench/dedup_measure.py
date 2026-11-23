"""Run matched Linux dedup measurements on a generated, disposable corpus."""
import argparse
import datetime
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

import dedup_corpus as corpus


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def memory():
    keys = {"MemTotal", "MemAvailable", "Cached", "Shmem", "Dirty", "Writeback", "Buffers"}
    values = {k: int(v.split()[0]) for k, v in
              (line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines()) if k in keys}
    pressure = Path("/proc/pressure/memory")
    if pressure.exists():
        for line in pressure.read_text().splitlines():
            fields = line.split()
            values["psi_" + fields[0] + "_us"] = int(fields[-1].split("=")[1])
    # These distinct tmpfs mounts can include swapped pages. Shmem measures
    # resident shared memory and is not interchangeable with mount occupancy.
    for key, path in (("tmp_mount_used_KiB", "/tmp"), ("shm_mount_used_KiB", "/dev/shm")):
        fs = os.statvfs(path)
        values[key] = (fs.f_blocks - fs.f_bfree) * fs.f_frsize // 1024
    return values


def scratch_usage(scratch, pid):
    # Include unlinked SQLite temporary files while their descriptors are open.
    inodes = {}
    def record(path):
        st = path.stat()
        inodes[st.st_dev, st.st_ino] = st.st_size, st.st_blocks * 512

    for parent, _, files in os.walk(scratch):
        for name in files:
            try:
                record(Path(parent) / name)
            except FileNotFoundError:
                pass
    todo = [pid]
    while todo:
        p = todo.pop()
        try:
            todo.extend(map(int, Path(f"/proc/{p}/task/{p}/children").read_text().split()))
            for fd in Path(f"/proc/{p}/fd").iterdir():
                try:
                    if os.readlink(fd).startswith(str(scratch) + "/"):
                        record(fd)
                except FileNotFoundError:
                    pass
        except (FileNotFoundError, ProcessLookupError):
            pass
    return sum(a for a, _ in inodes.values()), sum(b for _, b in inodes.values())


def timed(command, dest, scratch, gnu_time, env):
    mem_before = memory()
    observations = []
    failures = []
    inaccessible = []
    stop = threading.Event()
    def sample(pid):
        while not stop.is_set():
            try:
                logical, blocks = scratch_usage(scratch, pid)
                observations.append((logical, blocks, memory()))
            except PermissionError as exc:
                # procfs permissions may change while a descendant execs.
                # Preserve missing samples separately from genuine zero use.
                inaccessible.append(str(exc))
            except OSError as exc:
                failures.append(str(exc))
            stop.wait(0.025)

    args = [str(gnu_time), "-v", "-o", str(dest / "time.txt"), *map(str, command)]
    started = now()
    with (dest / "output").open("xb") as output, (dest / "stderr").open("xb") as errors:
        start = time.perf_counter_ns()
        process = subprocess.Popen(args, stdout=output, stderr=errors, env=env)
        monitor = threading.Thread(target=sample, args=(process.pid,))
        monitor.start()
        status = process.wait()
        duration = time.perf_counter_ns() - start
        stop.set()
        monitor.join()
    after = memory()
    if failures:
        raise RuntimeError(f"scratch sampler failed: {failures[:3]}")
    timing = (dest / "time.txt").read_text()
    def field(label):
        return int(re.search(re.escape(label) + r":\s*(\d+)", timing)[1])
    counters = {}
    for line in (dest / "stderr").read_text().splitlines():
        if line.startswith(("dedup ", "baseline:")):
            counters.update({k:int(v) for k, v in re.findall(r"([\w-]+)=(\d+)", line)})
    samples = [mem_before, after, *(m for _, _, m in observations)]
    result = dict(command=args, startedAt=started, observedAt=now(), exit=status, wall_ns=duration,
                  peak_rss_kib=field("Maximum resident set size (kbytes)"),
                  fs_inputs=field("File system inputs"), fs_outputs=field("File system outputs"),
                  scratch_sample_period_ms=25, samples=len(observations),
                  inaccessible_samples=len(inaccessible), first_inaccessible=inaccessible[0] if inaccessible else None,
                  observed_scratch_bytes=max((x for x,_,_ in observations), default=None),
                  observed_scratch_allocated_bytes=max((x for _,x,_ in observations), default=None),
                  memory_kib_before=mem_before, memory_kib_after=after,
                  cache_min_kib=min(m["Cached"] for m in samples),
                  cache_max_kib=max(m["Cached"] for m in samples),
                  min_available_kib=min(m["MemAvailable"] for m in samples), counters=counters)
    if status:
        raise RuntimeError(f"command exited {status}: see {dest / 'stderr'}")
    return result


def sorted_hash(source, dest, env):
    with dest.open("wb") as out:
        subprocess.run(["sort", "-S", "64M", "-T", dest.parent, source], stdout=out, check=True, env=env)
    value = corpus.digest_file(dest)
    dest.unlink()
    return value


def run(args):
    source, scratch, out = args.source.resolve(), args.scratch.resolve(), args.out.resolve()
    if list(scratch.iterdir()):
        raise ValueError("provide an empty dedicated scratch directory")
    meta = corpus.read_meta(source)
    if meta["role"] != "source":
        raise ValueError("expected original corpus for clone preparation")
    out.mkdir(mode=0o700)
    home = out / "home"
    home.mkdir()
    env = dict(os.environ, HOME=str(home), JAROMAILDIR=str(home / "unused"), LC_ALL="C",
               PYTHONDONTWRITEBYTECODE="1")
    env.pop("JAROWORKDIR", None)
    corpus_cmd = [sys.executable, corpus.__file__]
    baseline = Path(__file__).with_name("dedup_full_hash.py")
    expected = out / "expected-preview"
    with expected.open("xb") as stream:
        subprocess.run([*corpus_cmd, "expected-preview", source], check=True, env=env, stdout=stream)
    expected_sha = sorted_hash(expected, out / "sorted", env)
    expected.unlink()
    for repetition, state, mode in itertools.product(range(1, args.repeats + 1), args.cache, args.mode):
        # Alternate implementation order to distribute drift between arms.
        order = args.impl if repetition % 2 else args.impl[::-1]
        for impl in order:
            name = f"{source.name}-{state}-{mode}-{impl}-{repetition}"
            dest = out / name
            dest.mkdir()
            with tempfile.TemporaryDirectory(prefix="copy-", dir=out) as staging:
                root = source
                if mode == "apply":
                    root = Path(staging) / "trial"
                    subprocess.run([*corpus_cmd, "clone", source, root], check=True, env=env)
                with (dest / "cache.txt").open("x") as log:
                    subprocess.run([*corpus_cmd, "cache", root, state], check=True, env=env, stdout=log)
                dirs = [root / "mail" / box for box in corpus.FOLDERS]
                if impl == "cli":
                    cmd = [args.cli, *(["-n"] if mode == "preview" else []),
                           "dedup", "--scratch-dir", scratch, "--", *dirs]
                elif impl == "profile":
                    cmd = [args.profile, "--" + mode, "--scratch-dir", scratch]
                    if args.profile_cache_kib:
                        cmd.extend(["--test-cache-kib", str(args.profile_cache_kib)])
                    if args.profile_heap_kib:
                        cmd.extend(["--test-heap-kib", str(args.profile_heap_kib)])
                    cmd.extend(["--", *dirs])
                else:
                    cmd = [sys.executable, baseline, root, "--scratch-dir", scratch,
                           *(["--apply"] if mode == "apply" else [])]
                result = timed(cmd, dest, scratch, args.gnu_time, env)
                if list(scratch.iterdir()):
                    raise RuntimeError("scratch remains after successful run")
                result.update(workload=source.name, implementation=impl, cache=state, mode=mode,
                              repeat=repetition, logical_bytes=meta["logical_bytes"], messages=meta["count"])
                preview = dest / "output"
                if mode == "preview":
                    result["expected_preview_sorted_sha256"] = expected_sha
                    result["preview_sorted_sha256"] = sorted_hash(preview, dest / "sorted", env)
                    if result["preview_sorted_sha256"] != expected_sha:
                        raise ValueError(f"preview mismatch: {name}")
                elif preview.stat().st_size:
                    raise ValueError("unexpected apply output")
                result["report_bytes"] = preview.stat().st_size
                result["report_sha256"] = corpus.digest_file(preview)
                preview.unlink()
                verify = [*corpus_cmd, "verify", root, *(["--survivors"] if mode == "apply" else [])]
                with (dest / "verification.txt").open("x") as log:
                    subprocess.run(verify, check=True, env=env, stdout=log)
                result["verification"] = (dest / "verification.txt").read_text().strip()
                (dest / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                print(name, round(result["wall_ns"] / 1e9, 4), result["peak_rss_kib"], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    for option in ("scratch", "out", "cli", "profile", "gnu-time"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--profile-cache-kib", type=int, help="test driver SQLite page cache")
    parser.add_argument("--profile-heap-kib", type=int, help="test driver SQLite hard heap limit")
    parser.add_argument("--impl", nargs="+", choices=("cli","baseline","profile"), default=["cli","baseline","profile"])
    parser.add_argument("--cache", nargs="+", choices=("warm","cold-hint"), default=["warm","cold-hint"])
    parser.add_argument("--mode", nargs="+", choices=("preview","apply"), default=["preview","apply"])
    run(parser.parse_args())
