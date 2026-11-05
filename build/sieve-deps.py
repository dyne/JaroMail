#!/usr/bin/env python3
"""Provision signed 2.4.5 development dependencies in an external owned cache.

stdout is only the verified private prefix; progress and diagnostics use stderr.
No download occurs without --download. This is never part of the application build.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


REPO = Path(__file__).resolve().parent.parent
SIGNER = "EF0882079FD4ED32BF8B23B2A1B09EF84EDC5219"
KEY_URL = "https://repo.dovecot.org/DOVECOT-REPO-GPG-2.4"
SOURCES = {
    "dovecot-2.4.5.tar.gz": ("https://dovecot.org/releases/2.4/dovecot-2.4.5.tar.gz",
        "868c2686a61b5f8e00a3e4721789b1ab46e6528fd773a5fbed07a6ecba7731e6"),
    "dovecot-pigeonhole-2.4.5.tar.gz": (
        "https://pigeonhole.dovecot.org/releases/2.4/dovecot-pigeonhole-2.4.5.tar.gz",
        "ad7c478cb3aaa76c5f81f86727a3e6843645b0a1253f5684fb8a0beec0d22925")}
CORE_OPTIONS = ["--with-shared-libs", "--disable-static"] + ["--without-" + name for name in (
    "pam", "gssapi", "ldap", "sql", "lua", "libcap", "systemd", "solr", "flatcurve",
    "textcat", "stemmer", "icu", "sodium", "apparmor", "bzlib", "lz4", "zstd", "libunwind", "pcre2")]
SIEVE_OPTIONS = ["--with-dovecot-install-dirs", "--without-managesieve", "--without-ldap", "--disable-static"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def external(value):
    path = Path(value).resolve()
    if path.is_relative_to(REPO):
        raise ValueError("dependency caches must be outside the repository")
    if any(ord(c) < 32 for c in str(path)):
        raise ValueError("control byte in cache path")
    return path


def obtain(path, url, download):
    if path.is_symlink():
        raise ValueError("source cache symlink rejected: " + str(path))
    if path.is_file():
        return
    if not download:
        raise ValueError("missing supplied prerequisite " + str(path) + "; use --download to acquire official sources")
    print("Acquire " + url, file=sys.stderr, flush=True)
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(32 * 1024 * 1024 + 1)
    if len(data) > 32 * 1024 * 1024:
        raise ValueError("oversized source prerequisite")
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".download-", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    temporary.replace(path)


def inventory(prefix):
    if prefix.is_symlink() or not prefix.is_dir():
        raise ValueError("development prefix is missing or symlinked")
    result = {}
    for path in sorted(prefix.rglob("*")):
        info = path.lstat()
        entry = {"mode": oct(stat.S_IMODE(info.st_mode))}
        if path.is_symlink():
            if not path.resolve().is_relative_to(prefix):
                raise ValueError("dependency symlink leaves the owned prefix: " + str(path))
            entry.update(type="symlink", target=os.readlink(path))
        elif path.is_file():
            entry.update(type="file", sha256=digest(path), bytes=info.st_size)
        elif path.is_dir():
            entry.update(type="directory")
        else:
            raise ValueError("unexpected development prefix entry: " + str(path))
        result[str(path.relative_to(prefix))] = entry
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", required=True)
    parser.add_argument("--source-cache", required=True)
    parser.add_argument("--key", help="supplied official public key (otherwise source-cache/DOVECOT-REPO-GPG-2.4)")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--verify-only", action="store_true", help="require a complete matching cached prefix")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        raise ValueError("--jobs must be between 1 and 64")
    cache, source_cache = external(args.cache_dir), external(args.source_cache)
    cache.mkdir(parents=True, exist_ok=True)
    source_cache.mkdir(parents=True, exist_ok=True)
    key = Path(args.key).resolve(strict=True) if args.key else source_cache / "DOVECOT-REPO-GPG-2.4"
    obtain(key, KEY_URL, args.download)
    gpg_home = cache / "gnupg"
    gpg_home.mkdir(mode=0o700, exist_ok=True)
    gpg = ["gpg", "--homedir", str(gpg_home), "--batch", "--no-auto-key-retrieve"]
    keys = subprocess.check_output(gpg + ["--with-colons", "--show-keys", str(key)], text=True, stderr=subprocess.DEVNULL)
    primary = []
    want_primary = False
    for line in keys.splitlines():
        fields = line.split(":")
        if fields[0] == "pub":
            want_primary = True
        elif fields[0] == "fpr" and want_primary:
            primary.append(fields[9])
            want_primary = False
    if primary != [SIGNER]:
        raise ValueError("official signing key fingerprint mismatch")
    subprocess.run(gpg + ["--import", str(key)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    provenance = {}
    for filename, (url, expected) in SOURCES.items():
        archive, signature = source_cache / filename, source_cache / (filename + ".sig")
        obtain(archive, url, args.download)
        obtain(signature, url + ".sig", args.download)
        if digest(archive) != expected:
            raise ValueError("pinned release hash mismatch: " + filename)
        checked = subprocess.run(gpg + ["--status-fd", "1", "--verify", str(signature), str(archive)],
                                 capture_output=True, text=True, check=True)
        if "[GNUPG:] VALIDSIG " + SIGNER + " " not in checked.stdout:
            raise ValueError("release signer mismatch: " + filename)
        provenance[filename] = {"url":url,"sha256":expected,"signature_sha256":digest(signature),
                                "signer":SIGNER,"signature_status":checked.stdout}
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC must name a compiler")
    launcher = shlex.split(os.environ["CCACHE"]) if "CCACHE" in os.environ else (
        [shutil.which("ccache")] if shutil.which("ccache") else [])
    if any(Path(part).name == "ccache" for part in compiler):
        launcher = []
    linker = compiler[1:] if Path(compiler[0]).name == "ccache" else compiler
    flags = {name:os.environ.get(name, "-O2 -g" if name == "CFLAGS" else "")
             for name in ("CPPFLAGS", "CFLAGS", "LDFLAGS", "LIBS")}
    tools = {name:subprocess.check_output(command, text=True).strip() for name, command in (
        ("compiler", linker + ["--version"]), ("make", ["make", "--version"]),
        ("openssl", ["pkg-config", "--modversion", "openssl"]), ("zlib", ["pkg-config", "--modversion", "zlib"]))}
    identity = {"schema":1,"recipe_sha256":digest(Path(__file__).resolve()),"cache":str(cache),
                "sources":provenance,"key_sha256":digest(key),"compiler":compiler,"launcher":launcher,
                "compiler_binaries":{str(Path(p).resolve()):digest(Path(p).resolve()) for part in compiler + launcher
                    if (p := shutil.which(part))},"flags":flags,"tools":tools,
                "host":platform.machine(),"os_release":Path("/etc/os-release").read_text(),
                "environment":{name:os.environ.get(name) for name in (
                    "CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX",
                    "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR")},
                "core_options":CORE_OPTIONS,"pigeonhole_options":SIEVE_OPTIONS}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    generation = cache / fingerprint
    prefix = generation / "prefix"
    receipt = generation / "development.json"
    if receipt.is_file():
        record = json.loads(receipt.read_text())
        if record["identity"] != identity or record["inventory"] != inventory(prefix):
            raise ValueError("cached development fingerprint/content/mode mismatch; use a fresh cache")
        print("REUSE " + fingerprint, file=sys.stderr)
        print(prefix)
        return
    if args.verify_only:
        raise ValueError("no verified matching development cache")
    if generation.exists():
        raise ValueError("incomplete dependency generation; use a fresh cache")
    source_dir, logs = generation / "src", generation / "logs"
    source_dir.mkdir(parents=True)
    logs.mkdir()
    commands = []
    env = dict(os.environ, **flags, CC=shlex.join(linker))
    env.setdefault("CCACHE_DIR", str(cache / "ccache"))
    for key_name in ("LD_LIBRARY_PATH", "DOVECONF_PATH", "CONFIG_MODULES", "DOVECOT_CONFIG_FD"):
        env.pop(key_name, None)

    def execute(command, cwd, name):
        log = logs / (name + ".log")
        print("Build " + name, file=sys.stderr, flush=True)
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        commands.append({"command":command,"cwd":str(cwd),"exit":process.returncode,
                         "observedAt":datetime.now(timezone.utc).isoformat(),
                         "log":str(log),"log_sha256":digest(log)})
        (generation / "commands.json").write_text(json.dumps(commands,indent=2)+"\n")
        if process.returncode != 0:
            raise ValueError("dependency command failed, see " + str(log))

    for filename in SOURCES:
        with tarfile.open(source_cache / filename) as archive:
            root = filename.removesuffix(".tar.gz")
            for member in archive.getmembers():
                name = PurePosixPath(member.name)
                if (name.is_absolute() or ".." in name.parts or not name.parts or name.parts[0] != root or
                        not (member.isfile() or member.isdir())):
                    raise ValueError("unsafe source archive member " + member.name)
            archive.extractall(source_dir, filter="data")
    core = source_dir / "dovecot-2.4.5"
    sieve = source_dir / "dovecot-pigeonhole-2.4.5"
    make_options = ["CC=" + shlex.join(launcher + compiler), "CCLD=" + shlex.join(linker)]
    execute(["./configure", "--prefix=" + str(prefix)] + CORE_OPTIONS, core, "core-configure")
    execute(["make", "-j" + str(args.jobs)] + make_options, core, "core-build")
    execute(["make", "install"] + make_options, core, "core-install")
    execute(["./configure", "--prefix=" + str(prefix), "--with-dovecot=" + str(prefix / "lib/dovecot")]
            + SIEVE_OPTIONS, sieve, "pigeonhole-configure")
    for directory in ("src/lib-sieve", "src/lib-managesieve", "src/plugins/settings"):
        name = directory.split("/")[-1]
        execute(["make", "-j" + str(args.jobs), "-C", directory] + make_options, sieve, name + "-build")
        execute(["make", "-C", directory, "install"] + make_options, sieve, name + "-install")
    execute(["make", "install-dist_pkginc_libHEADERS", "install-nodist_pkginc_libHEADERS"] + make_options,
            sieve, "pigeonhole-headers")
    receipt.write_text(json.dumps({"fingerprint":fingerprint,"identity":identity,
        "inventory":inventory(prefix),"commands":commands},indent=2,sort_keys=True)+"\n")
    print("BUILT " + fingerprint, file=sys.stderr)
    print(prefix)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, tarfile.TarError) as error:
        print("sieve-deps: " + str(error), file=sys.stderr)
        sys.exit(1)
