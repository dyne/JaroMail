"""Generate and check deterministic disposable Maildirs for dedup benchmarks."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import stat
import subprocess
import sys


MIB = 1 << 20
FOLDERS = tuple(f"box{i}" for i in range(4))
SUBDIRS = ("cur", "new")
MARKER = "jaro-dedup-benchmark-v1"
ESCAPE = re.compile(rb'[^ -~]|["\\]')


def read_meta(root):
    meta = json.loads((root / "corpus.json").read_text())
    if meta["format"] != MARKER:
        raise ValueError("unrecognized benchmark fixture")
    return meta


def free_space(path):
    fs = os.statvfs(path)
    return dict(bytes=fs.f_bavail * fs.f_frsize, inodes=fs.f_favail, block=fs.f_frsize)


def reserve(path, needed, files):
    space = free_space(path)
    if needed + 512 * MIB > space["bytes"] or files + 128 > space["inodes"]:
        raise ValueError(f"insufficient capacity for {files} files, {needed} bytes plus reserve: {space}")


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            digest.update(block)
    return digest.hexdigest()


def body_size(args, group):
    if args.profile == "small":
        return 224 + 37 * (group % 29)
    sizes = args.sizes_mib
    extra = group // len(sizes) if args.profile == "attachment-unique" else 0
    return sizes[group % len(sizes)] * MIB + extra


def occurrences(group, classes, duplicates):
    # Put half the extra copies in one hot group and spread the rest evenly.
    hot = duplicates // 2
    rounds, extra = divmod(duplicates - hot, classes)
    return 1 + (hot if group == 0 else 0) + rounds + (group < extra)


def body_chunks(header, filler, label, size):
    # The identity is in the middle. All attachment ends match over the
    # native sampler's full windows; no holes, truncation or preallocation.
    yield header
    size -= len(header)
    middle = (size - len(label)) // 2
    for part, amount in enumerate((middle, size - middle - len(label))):
        for _ in range(amount // len(filler)):
            yield filler
        if amount % len(filler):
            yield filler[:amount % len(filler)]
        if part == 0:
            yield label


def generate(args):
    n = args.count
    percent = args.duplicate_percent
    if n <= 0 or not 0 <= percent <= 100 or not all(0 < m <= 16384 for m in args.sizes_mib):
        raise ValueError("require positive counts/sizes, sizes <= 16384 MiB and percent in [0, 100]")
    if args.profile == "attachment-unique" and (percent or len(set(args.sizes_mib)) != len(args.sizes_mib)
            or n > len(args.sizes_mib) * MIB):
        raise ValueError("unique sizes require distinct MiB values, no duplicates and nonoverlapping byte offsets")
    duplicates = min(n - 1, n * percent // 100)
    classes = n - duplicates
    root = args.root
    fsblock = free_space(root.parent)["block"]
    allocated = sum(((body_size(args, g) + fsblock - 1) // fsblock) * fsblock *
                    occurrences(g, classes, duplicates) for g in range(classes))
    reserve(root.parent, allocated + n * 320, n)
    root.mkdir(mode=0o700)
    mail = root / "mail"
    for folder, sub in itertools.product(FOLDERS, (*SUBDIRS, "tmp")):
        (mail / folder / sub).mkdir(parents=True)
    rng = random.Random(args.seed)
    if args.profile == "small":
        header = b"From: bench@fixture.invalid\r\nContent-Type: text/plain\r\n\r\n"
        filler = hashlib.sha256(str(args.seed).encode()).hexdigest().encode() + b"\r\n"
    else:
        header = (b"From: bench@fixture.invalid\r\nMIME-Version: 1.0\r\n"
                  b"Content-Type: application/octet-stream\r\nContent-Transfer-Encoding: binary\r\n"
                  b'Content-Disposition: attachment; filename="fixture.bin"\r\n\r\n')
        filler = hashlib.shake_256(str(args.seed).encode()).digest(64 * 1024)
    serial = logical = physical = 0
    with (root / "groups.tsv").open("x") as groups, (root / "paths.tsv").open("x") as paths:
        for g in range(classes):
            size = body_size(args, g)
            copies = occurrences(g, classes, duplicates)
            label = f"\r\nDocument {g:x}\r\n".encode()
            small = b"".join(body_chunks(header, filler, label, size)) if size < MIB else None
            digest = hashlib.sha256()
            forward = reverse = None
            for copy in range(copies):
                folder = rng.randrange(len(FOLDERS))
                sub = rng.randrange(2)
                mode = rng.choice((0o600, 0o640))
                name = f"{rng.getrandbits(64):016x}.{serial:010d}:2,{'S' if serial % 2 else 'F'}"
                rel = f"{FOLDERS[folder]}/{SUBDIRS[sub]}/{name}"
                path = mail / rel
                with path.open("xb") as out:
                    for chunk in (small,) if small is not None else body_chunks(header, filler, label, size):
                        out.write(chunk)
                        if copy == 0:
                            digest.update(chunk)
                    os.fchmod(out.fileno(), mode)
                forward = min(forward, (folder, sub, name, rel)) if forward else (folder, sub, name, rel)
                reverse = min(reverse, (-folder, sub, name, rel)) if reverse else (-folder, sub, name, rel)
                paths.write(f"{g}\t{mode:o}\t{rel}\n")
                physical += path.stat().st_blocks * 512
                logical += size
                serial += 1
            groups.write(f"{g}\t{size}\t{digest.hexdigest()}\t{forward[-1]}\t{reverse[-1]}\n")
    meta = dict(format=MARKER, role="source", profile=args.profile, count=n, groups=classes,
                duplicates=duplicates, duplicate_percent=percent, seed=args.seed, sizes_mib=args.sizes_mib,
                logical_bytes=logical, allocated_bytes=physical)
    (root / "corpus.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta))


def clone(args):
    source, dest = args.root, args.destination
    meta = read_meta(source)
    if meta["role"] != "source" or source.resolve() in dest.resolve().parents:
        raise ValueError("use a fresh destination outside the benchmark source")
    reserve(dest.parent, meta["allocated_bytes"] + 320 * meta["count"], meta["count"])
    dest.mkdir(mode=0o700)
    # Each duplicate must be a distinct directory entry on a distinct inode.
    shutil.copytree(source / "mail", dest / "mail", symlinks=True)
    for manifest in ("paths.tsv", "groups.tsv"):
        shutil.copyfile(source / manifest, dest / manifest)
    meta["role"] = "apply-clone"
    (dest / "corpus.json").write_text(json.dumps(meta, indent=2) + "\n")


def entries(root):
    for folder, sub in itertools.product(FOLDERS, SUBDIRS):
        with os.scandir(root / "mail" / folder / sub) as scan:
            for ent in scan:
                if not ent.is_file(follow_symlinks=False):
                    raise ValueError(f"unexpected non-file in generated fixture: {ent.path}")
                yield Path(ent.path)


def quote(path):
    raw = ESCAPE.sub(lambda m: f'\\x{m[0][0]:02x}'.encode(), os.fsencode(path))
    return b'"' + raw + b'"'


def manifest(root, reverse):
    with (root / "groups.tsv").open() as groups, (root / "paths.tsv").open() as paths:
        runs = itertools.groupby(paths, lambda row: row.split("\t", 1)[0])
        for group, run in itertools.zip_longest(groups, runs):
            gid, size, digest, first, last = group.rstrip("\n").split("\t")
            if run[0] != gid:
                raise ValueError(f"inconsistent group manifest at {gid}")
            winner = last if reverse else first
            for row in run[1]:
                _, mode, rel = row.rstrip("\n").split("\t")
                yield rel, winner, int(size), digest, int(mode, 8)


def verify(args):
    root = args.root
    meta = read_meta(root)
    mail = root.resolve() / "mail"
    checked = records = keeps = 0
    for rel, winner, size, digest, mode in manifest(root, args.reverse):
        records += 1
        keeps += rel == winner
        if args.command == "expected-preview":
            if rel != winner:
                sys.stdout.buffer.write(b"keep " + quote(mail / winner) + b" remove " + quote(mail / rel) + b"\n")
            continue
        if args.survivors and rel != winner:
            continue
        path = mail / rel
        info = path.lstat()
        if info.st_mode != (stat.S_IFREG | mode) or info.st_size != size or digest_file(path) != digest:
            raise ValueError(f"manifest mismatch: {rel}")
        checked += 1
    if (records, keeps) != (meta["count"], meta["groups"]):
        raise ValueError("manifest coverage disagrees with corpus metadata")
    if args.command == "verify":
        if sum(1 for _ in entries(root)) != checked:
            raise ValueError("unexpected entries in fixture")
        print(f"verified {checked} files from {records} records and {keeps} survivor groups")


def cache(args):
    read_meta(args.root)
    count = size = 0
    for path in entries(args.root):
        with path.open("rb") as stream:
            if args.state == "warm":
                for block in iter(lambda: stream.read(MIB), b""):
                    size += len(block)
            else:
                os.fsync(stream.fileno())
                os.posix_fadvise(stream.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                size += path.stat().st_size
        count += 1
    print(f"{args.state}: {count} files, {size} bytes")


def environment(args):
    def output(cmd):
        try:
            return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT).strip()
        except (OSError, subprocess.CalledProcessError) as error:
            return f"unavailable: {error}"

    def storage(path):
        dev = path.stat().st_dev
        sysdev = Path(f"/sys/dev/block/{os.major(dev)}:{os.minor(dev)}").resolve()
        slaves = sorted((sysdev / "slaves").glob("*"))
        media = {}
        for node in slaves or [sysdev]:
            node = node.resolve()
            if (node / "partition").exists():
                node = node.parent
            rota = node / "queue/rotational"
            media[node.name] = rota.read_text().strip() if rota.is_file() else None
        return dict(path=str(path.resolve()), free=free_space(path),
                    mount=output(["findmnt", "-n", "-o", "SOURCE,FSTYPE,OPTIONS", "-T", path]),
                    device=sysdev.name, rotational=media)

    compiler = next(line.split("=", 1)[1] for line in (args.build / "CMakeCache.txt").read_text().splitlines()
                    if line.startswith("CMAKE_C_COMPILER:"))
    compile_db = json.loads((args.build / "compile_commands.json").read_text())
    command = next(row for row in compile_db if row["file"].endswith("/src/jaro-dedup.c"))
    info = dict(platform=platform.platform(), python=sys.version, cpu=output(["lscpu"]),
                corpus=read_meta(args.root), input_storage=storage(args.root), scratch_storage=storage(args.scratch),
                compiler=output([compiler, "--version"]), compile_command=command, helper=str(args.helper.resolve()),
                helper_sha256=digest_file(args.helper), meminfo=Path("/proc/meminfo").read_text(),
                cache_procedure="warm reads each file; cold-hint fsyncs it then requests POSIX_FADV_DONTNEED; metadata stays cached")
    print(json.dumps(info, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, run in (("generate", generate), ("clone", clone), ("verify", verify),
                      ("expected-preview", verify), ("cache", cache), ("environment", environment)):
        sub = commands.add_parser(name)
        sub.add_argument("root", type=Path)
        sub.set_defaults(run=run)
        if name == "generate":
            sub.add_argument("--count", type=int, required=True)
            sub.add_argument("--profile", choices=("small", "attachment-unique", "attachment-repeated"), default="small")
            sub.add_argument("--seed", type=int, default=73)
            sub.add_argument("--duplicate-percent", type=int, default=0)
            sub.add_argument("--sizes-mib", type=lambda s: list(map(int, s.split(","))), default=[1, 8, 64, 1025])
        if name == "clone":
            sub.add_argument("destination", type=Path)
        if run == verify:
            sub.add_argument("--reverse", action="store_true")
            sub.add_argument("--survivors", action="store_true", help="check the applied clone")
        if name == "cache":
            sub.add_argument("state", choices=("warm", "cold-hint"))
        if name == "environment":
            for option in ("scratch", "build", "helper"):
                sub.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
