#!/usr/bin/env python3
"""Build pinned Pigeonhole frontends and an optional relocatable Linux runtime."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parent.parent
VERSION = "2.4.5"
REVISION = "742bb892"
LIBRARIES = ("dovecot-sieve", "dovecot-storage", "dovecot-lda", "dovecot")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    print(shlex.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def words(name, default=""):
    return shlex.split(os.environ.get(name, default))


def macro(path, name):
    match = re.search(r'^#define\s+' + name + r'\s+"([^"]+)"',
                      path.read_text(), re.M)
    if not match:
        raise ValueError(f"missing generated {name} in {path}")
    return match.group(1)


def runtime_inputs(prefix):
    libdir = prefix / "lib/dovecot"
    files = {"bin/doveconf": prefix / "bin/doveconf",
             "lib/libpigeonhole_settings.so": libdir / "settings/libpigeonhole_settings.so",
             "dovecot.conf": REPO / "src/sieve/dovecot.conf", "run": REPO / "src/sieve/run",
             "maildir.conf": REPO / "src/sieve/maildir.conf",
             "filter": REPO / "src/sieve/filter"}
    for name in (*LIBRARIES, "dovecot-managesieve"):
        soname = "lib" + name + ".so.0"
        files["lib/" + soname] = libdir / soname
    for target, path in files.items():
        if not path.is_file() or (target.startswith(("bin/", "lib/")) and
                                  not path.resolve().is_relative_to(prefix)):
            raise ValueError("missing private runtime dependency: " + str(path))
    for name in LIBRARIES:
        development = libdir / ("lib" + name + ".so")
        installed = files["lib/lib" + name + ".so.0"]
        if not development.samefile(installed) and digest(development) != digest(installed):
            raise ValueError("runtime SONAME differs from the linked development library: " + str(installed))
    if b"Pigeonhole version 2.4.5 (742bb892)" not in files["lib/libpigeonhole_settings.so"].read_bytes():
        raise ValueError("Pigeonhole settings module version/revision mismatch")
    storage = subprocess.check_output(["nm", "-D", "--defined-only",
                                       files["lib/libdovecot-storage.so.0"]], text=True)
    if not re.search(r"\bmaildir_storage$", storage, re.M):
        raise ValueError("Dovecot storage library lacks the Maildir driver")
    return files


def package_runtime(stage, files):
    runtime = stage / "runtime"
    for name, source in files.items():
        target = runtime / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for tool in ("sievec", "sieve-dump", "maildir-filter"):
        os.link(stage / "bin" / tool, runtime / "bin" / tool)
    inventory = "".join(f"{digest(path)}  {path.relative_to(runtime)}\n"
                        for path in sorted(runtime.rglob("*")) if path.is_file())
    (runtime / "SHA256SUMS").write_text(inventory)
    result = subprocess.check_output([runtime / "bin/doveconf", "-c", runtime / "dovecot.conf", "-n"],
        env={"LD_LIBRARY_PATH": str(runtime / "lib"),
             "CONFIG_MODULES": str(runtime / "lib/libpigeonhole_settings.so")}, text=True)
    if not (result.startswith("# 2.4.5 (") and "# Pigeonhole version 2.4.5 (742bb892)\n" in result):
        raise ValueError("packaged Dovecot/Pigeonhole configuration ABI mismatch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps-prefix", default=os.environ.get("SIEVE_DEPS_PREFIX"))
    parser.add_argument("--output", default=os.environ.get(
        "SIEVE_TOOLS_OUTPUT", str(REPO / "build/sieve-tools")))
    parser.add_argument("--package-runtime", action="store_true")
    parser.add_argument("--compiler", help="single compiler executable selected by CMake")
    args = parser.parse_args()
    if not args.deps_prefix:
        raise ValueError("SIEVE_DEPS_PREFIX is required (matched 2.4.5 development installation)")
    prefix = Path(args.deps_prefix).resolve(strict=True)
    include = prefix / "include/dovecot"
    sieve_include = include / "sieve"
    core_config = include / "config.h"
    sieve_config = sieve_include / "pigeonhole-config.h"
    if macro(core_config, "DOVECOT_VERSION") != VERSION:
        raise ValueError("Dovecot development version must be " + VERSION)
    if macro(sieve_config, "PIGEONHOLE_VERSION") != VERSION:
        raise ValueError("Pigeonhole development version must be " + VERSION)
    abi = {"dovecot": macro(core_config, "DOVECOT_ABI_VERSION"),
           "pigeonhole": macro(sieve_config, "PIGEONHOLE_ABI_VERSION")}
    if set(abi.values()) != {"2.4.ABIv5"}:
        raise ValueError("development ABI must be 2.4.ABIv5 for both libraries")
    if REVISION not in (sieve_include / "pigeonhole-version.h").read_text():
        raise ValueError("Pigeonhole development headers do not match the pinned revision")

    # Explicit prefix paths prevent accidental fallback to host Dovecot libraries.
    libraries = [prefix / "lib/dovecot" / ("lib" + n + ".so") for n in LIBRARIES]
    for library in libraries:
        if not library.is_file() or not library.resolve().is_relative_to(prefix):
            raise ValueError("missing private-prefix development library: " + str(library))
    if (VERSION + " (" + REVISION + ")").encode() not in libraries[0].read_bytes():
        raise ValueError("Sieve library version/revision differs from the pinned headers")
    if not re.search(rb'2\.4\.5 \([^\x00]+\)', libraries[-1].read_bytes()):
        raise ValueError("Dovecot library version differs from the development headers")
    symbols = subprocess.check_output(["nm", "-D", "--defined-only", libraries[0]], text=True)
    manifest = json.loads((REPO / "vendor/sources.json").read_text())
    for symbol in manifest["external_build_dependencies"]["private_symbols"]:
        if not re.search(r'\b' + symbol + r'$', symbols, re.M):
            raise ValueError("matched Sieve library lacks private symbol " + symbol)
    run([sys.executable, REPO / "vendor/verify.py"])

    upstream = REPO / "vendor/pigeonhole/upstream/src"
    programs = manifest["programs"]
    sources = [upstream / "sieve-tools" / (name + ".c") for name in programs]
    sources.append(upstream / "lib-sieve-tool/sieve-tool.c")
    if args.package_runtime:
        programs = [*programs, "maildir-filter"]
        sources.append(REPO / "src/sieve/maildir-filter.c")
    runtime_files = runtime_inputs(prefix) if args.package_runtime else {}
    compiler = [args.compiler] if args.compiler else words("CC", "cc")
    if not compiler:
        raise ValueError("CC must name a compiler")
    launcher = words("CCACHE") if "CCACHE" in os.environ else (
        [shutil.which("ccache")] if shutil.which("ccache") else [])
    if any(Path(part).name == "ccache" for part in compiler):
        launcher = []
    linker = compiler[1:] if Path(compiler[0]).name == "ccache" else compiler
    if not linker:
        raise ValueError("CC must name a compiler after ccache")
    cppflags, cflags = words("CPPFLAGS"), words("CFLAGS", "-O2 -g")
    ldflags, ldlibs = words("LDFLAGS"), words("LDLIBS")
    includes = [upstream / "lib-sieve-tool", upstream / "lib-sieve/util", include, sieve_include]
    inputs = [p for p in include.rglob("*") if p.is_file()]
    inputs += libraries + sources + [REPO / "vendor/sources.json", Path(__file__).resolve()]
    inputs += list(runtime_files.values())
    maildir_source = REPO / "vendor/dovecot/upstream/maildir-save.c"
    maildir_patch = REPO / "src/sieve/maildir-save.patch"
    if args.package_runtime:
        inputs += [maildir_source, maildir_patch]
    inputs += [REPO / "vendor" / manifest["directory"] / e["path"] for e in manifest["files"]]
    for part in compiler + launcher:
        located = shutil.which(part)
        if located:
            inputs.append(Path(located).resolve())
    identity = {"schema": 1, "prefix": str(prefix), "abi": abi,
                "inputs": {str(p): digest(p) for p in sorted(set(inputs))},
                "compiler": compiler, "launcher": launcher, "cppflags": cppflags,
                "cflags": cflags, "ldflags": ldflags, "ldlibs": ldlibs,
                "environment": {name: os.environ.get(name) for name in (
                    "CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX")},
                "compiler_version": subprocess.check_output(compiler + ["--version"], text=True)}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    destination = output / fingerprint
    record_path = destination / "build.json"
    if record_path.is_file():
        record = json.loads(record_path.read_text())
        if (record["identity"] == identity and all(Path(p).is_file() and digest(Path(p)) == h
                for p, h in record["dependencies"].items()) and
                all((destination / p).is_file() and digest(destination / p) == h
                    for p, h in record["artifacts"].items())):
            publish(output, destination)
            print("REUSE " + fingerprint)
            return
        raise ValueError("cached tool/development dependencies changed; use a fresh output directory")
    if destination.exists():
        raise ValueError("incomplete output exists; use a fresh output directory")
    stage = Path(tempfile.mkdtemp(prefix=".building-", dir=output))
    (stage / "bin").mkdir()
    env = dict(os.environ)
    libdir = str(libraries[0].parent)
    env["LD_LIBRARY_PATH"] = libdir
    commands = []
    if args.package_runtime:
        # Only the local worker links the adapted storage transaction. The
        # shared runtime and server keep their upstream Maildir save behavior.
        generated = stage / "maildir-save.c"
        run(["patch", "--silent", "--output", generated, maildir_source, maildir_patch])
        sources.append(generated)
    for source in sources:
        obj = stage / (source.stem + ".o")
        command = launcher + compiler + ["-DHAVE_CONFIG_H"] + ["-I" + str(p) for p in includes]
        command += cppflags + cflags
        if args.package_runtime and source == generated:
            command.append("-D_GNU_SOURCE")
        # Upstream headers and tools require GNU C extensions (including printf %m).
        if args.package_runtime:
            command.append("-Wno-pedantic")
        command += ["-MD", "-MF", str(obj.with_suffix(".d")), "-c", str(source), "-o", str(obj)]
        commands.append(command)
        run(command, env=env)
    for name in programs:
        rpath = "$ORIGIN/../lib" if args.package_runtime else libdir
        command = linker + ldflags + ["-Wl,--export-dynamic", "-Wl,-rpath," + rpath,
            "-Wl,-Map," + str(stage / (name + ".map")), "-o", str(stage / "bin" / name),
            str(stage / (name + ".o")), str(stage / "sieve-tool.o")]
        command += list(map(str, libraries)) + ldlibs
        if name == "maildir-filter":
            command.insert(command.index(str(libraries[0])), str(stage / "maildir-save.o"))
        commands.append(command)
        run(command, env=env)
    if runtime_files:
        package_runtime(stage, runtime_files)
    # GCC dependency output quotes spaces with backslashes. Parse after joining continuations.
    dependencies = {}
    for depfile in stage.glob("*.d"):
        text = depfile.read_text().replace("\\\n", "")
        for name in shlex.split(text.split(":", 1)[1]):
            path = Path(name)
            if not path.is_relative_to(stage):
                dependencies[str(path)] = digest(path)
    artifacts = {str(p.relative_to(stage)): digest(p) for p in stage.rglob("*") if p.is_file()}
    record = {"fingerprint": fingerprint, "identity": identity, "commands": commands,
              "dependencies": dependencies, "artifacts": artifacts,
              "private_symbols": manifest["external_build_dependencies"]["private_symbols"]}
    (stage / "build.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    stage.rename(destination)
    publish(output, destination)
    print("BUILT " + fingerprint)


def publish(output, destination):
    link = output / ".current-new"
    if link.is_symlink():
        link.unlink()
    link.symlink_to(destination.name, target_is_directory=True)
    link.replace(output / "current")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("sieve-tools: " + str(error), file=sys.stderr)
        sys.exit(1)
