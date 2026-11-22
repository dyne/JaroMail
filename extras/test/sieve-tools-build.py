#!/usr/bin/env python3
"""Exercise actual frontend compilation and failure/publication boundaries."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


repo = Path(sys.argv[1]).resolve()
prefix = Path(os.environ["SIEVE_DEPS_PREFIX"]).resolve()
builder = repo / "build/sieve-tools.py"
checks = 0


def passed(name):
    global checks
    checks += 1
    print("PASS " + name, flush=True)


with tempfile.TemporaryDirectory(prefix="jaro-sieve-build-", dir=os.environ.get("SIEVE_BUILD_TEST_TMP")) as temp:
    root = Path(temp)
    output = root / "output with spaces"
    env = dict(os.environ, SIEVE_TOOLS_OUTPUT=str(output),
               CPPFLAGS="-DJARO_SIEVE_CPPFLAG=1", CFLAGS="-O1 -g -DJARO_SIEVE_CFLAG=1",
               LDFLAGS="-Wl,--as-needed", LDLIBS="-lm", CCACHE_LOGFILE=str(root / "ccache.log"))
    # Deny acquisition helpers and Python networking during the actual offline build.
    denied = root / "deny-network"
    denied.mkdir()
    for name in ("curl", "wget", "git", "sievec", "sieve-test"):
        path = denied / name
        path.write_text("#!/bin/sh\necho forbidden-helper >&2\nexit 93\n")
        path.chmod(0o755)
    (denied / "sitecustomize.py").write_text(
        "import socket\ndef denied(*a, **kw):\n raise RuntimeError('network forbidden')\n"
        "socket.socket=denied\nsocket.create_connection=denied\n")
    env["PATH"] = str(denied) + os.pathsep + env["PATH"]
    env["PYTHONPATH"] = str(denied)

    def invoke(overrides=None, expected=0, message=None):
        selected = dict(env)
        selected.update(overrides or {})
        result = subprocess.run([sys.executable, str(builder)], cwd=repo,
                                env=selected, text=True, capture_output=True)
        if result.returncode != expected:
            raise AssertionError(result.stdout + result.stderr)
        if message:
            assert message in result.stdout + result.stderr, result
        return result

    invoke(message="BUILT ")
    generation = (output / "current").resolve()
    record = json.loads((generation / "build.json").read_text())
    assert len(record["commands"]) == 9
    compile_commands, links = record["commands"][:5], record["commands"][5:]
    assert {Path(c[c.index("-c") + 1]).name for c in compile_commands} == {
        "sievec.c", "sieve-test.c", "sieve-dump.c", "sieve-filter.c", "sieve-tool.c"}
    for command in compile_commands:
        assert "-DJARO_SIEVE_CPPFLAG=1" in command and "-DJARO_SIEVE_CFLAG=1" in command
    if shutil.which("ccache") and "CCACHE" not in os.environ and "ccache" not in env.get("CC", ""):
        assert all(Path(c[0]).name == "ccache" for c in compile_commands)
    for command in links:
        assert Path(command[0]).name != "ccache"
        assert "-Wl,--as-needed" in command and "-lm" in command
    passed("empty-output compile/link, caller flags, compile-only ccache, acquisition denied")

    vendor_dependencies = {str(Path(p).relative_to(repo / "vendor"))
                           for p in record["dependencies"] if Path(p).is_relative_to(repo / "vendor")}
    assert vendor_dependencies == {
        "pigeonhole/upstream/src/sieve-tools/sievec.c",
        "pigeonhole/upstream/src/sieve-tools/sieve-test.c",
        "pigeonhole/upstream/src/sieve-tools/sieve-dump.c",
        "pigeonhole/upstream/src/sieve-tools/sieve-filter.c",
        "pigeonhole/upstream/src/lib-sieve-tool/sieve-tool.c",
        "pigeonhole/upstream/src/lib-sieve-tool/sieve-tool.h",
        "pigeonhole/upstream/src/lib-sieve/util/mail-raw.h"}
    assert str(prefix / "include/dovecot/config.h") in record["dependencies"]
    assert str(prefix / "include/dovecot/sieve/pigeonhole-config.h") in record["dependencies"]
    for name in ("sievec", "sieve-test", "sieve-dump", "sieve-filter"):
        mapping = (generation / (name + ".map")).read_text()
        assert "mail_raw_open_file" in mapping
        for library in ("dovecot-sieve", "dovecot-storage", "dovecot-lda", "dovecot"):
            assert str(prefix / "lib/dovecot" / ("lib" + library + ".so")) in mapping
        # No arguments intentionally exercises main/loader and returns upstream EX_USAGE.
        startup = subprocess.run([str(generation / "bin" / name)], env=dict(env, LD_LIBRARY_PATH=str(prefix / "lib/dovecot")),
                                 capture_output=True, text=True)
        assert startup.returncode == 64 and "Usage:" in startup.stdout, startup
        dynamic = subprocess.check_output(["ldd", str(generation / "bin" / name)], env=env, text=True)
        assert "not found" not in dynamic
        # --as-needed can omit lda because these entry points do not use its symbols.
        for library in ("dovecot-sieve", "dovecot-storage", "dovecot"):
            assert str(prefix / "lib/dovecot" / ("lib" + library + ".so")) in dynamic, dynamic
    passed("exact seven source/header dependencies, matched dynamic links/private symbols, four mains run")

    original_mtime = (generation / "bin/sievec").stat().st_mtime_ns
    invoke(message="REUSE ")
    assert (output / "current").resolve() == generation
    assert (generation / "bin/sievec").stat().st_mtime_ns == original_mtime
    passed("warm output reuse preserves binaries")
    if compile_commands[0][0].endswith("ccache"):
        cache_log = root / "ccache.log"
        previous = cache_log.stat().st_size
        invoke({"SIEVE_TOOLS_OUTPUT": str(root / "fresh-output")}, message="BUILT ")
        entries = cache_log.read_text()[previous:]
        assert "direct_cache_hit" in entries or "preprocessed_cache_hit" in entries, entries
        passed("fresh output compilation reuses warm ccache entries")
    invoke({"CFLAGS": env["CFLAGS"] + " -DJARO_CHANGED=1"}, message="BUILT ")
    assert (output / "current").resolve() != generation
    passed("changed flags select a different fingerprint")

    if shutil.which("ccache"):
        invoke({"CC": "ccache cc", "CCACHE": ""}, message="BUILT ")
        wrapped = json.loads((output / "current/build.json").read_text())
        assert all(Path(c[0]).name == "ccache" and c.count("ccache") == 1 for c in wrapped["commands"][:5])
        assert all(Path(c[0]).name != "ccache" for c in wrapped["commands"][5:])
        passed("caller ccache compiler is preserved once and excluded from links")

    invoke({"SIEVE_DEPS_PREFIX": str(root / "absent")}, 1, "sieve-tools:")
    wrong = root / "wrong-version/include/dovecot"
    wrong.mkdir(parents=True)
    (wrong / "config.h").write_text('#define DOVECOT_VERSION "2.4.1"\n')
    invoke({"SIEVE_DEPS_PREFIX": str(root / "wrong-version")}, 1, "version must be 2.4.5")
    passed("missing and mismatched development prerequisites rejected")

    cloned = root / "development clone"
    shutil.copytree(prefix / "include", cloned / "include")
    shutil.copytree(prefix / "lib/dovecot", cloned / "lib/dovecot")
    development_output = root / "development-output"
    development_env = {"SIEVE_DEPS_PREFIX": str(cloned), "SIEVE_TOOLS_OUTPUT": str(development_output)}
    invoke(development_env, message="BUILT ")
    development_generation = (development_output / "current").resolve()
    header = cloned / "include/dovecot/config.h"
    stat = header.stat()
    header.write_bytes(header.read_bytes() + b"\n/* changed build configuration */\n")
    os.utime(header, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    invoke(development_env, message="BUILT ")
    assert (development_output / "current").resolve() != development_generation
    passed("development header bytes invalidate fingerprint with unchanged timestamp")

    library = cloned / "lib/dovecot/libdovecot-sieve.so"
    original = library.read_bytes()
    library.write_bytes(original.replace(b"2.4.5 (742bb892)", b"2.4.1 (742bb892)"))
    invoke(development_env, 1, "Sieve library version/revision differs")
    library.write_bytes(original)
    passed("mismatched actual Sieve library rejected")

    missing_symbols = root / "missing-symbols"
    missing_symbols.mkdir()
    nm = missing_symbols / "nm"
    nm.write_text("#!/bin/sh\necho '0000 T mail_raw_user_create'\n")
    nm.chmod(0o755)
    invoke({"PATH": str(missing_symbols) + os.pathsep + env["PATH"]}, 1, "lacks private symbol")
    passed("missing raw-mail library exports rejected")

    compiler = shutil.which("cc")
    for phase in ("compile", "link"):
        wrapper = root / (phase + "-failure")
        wrapper.write_text("#!/bin/sh\n" +
            ("for arg do if [ \"$arg\" = -c ]; then echo injected-compile >&2; exit 91; fi; done\n"
             if phase == "compile" else
             'case " $* " in *" -c "*|*" --version "*) ;; *) echo injected-link >&2; exit 92;; esac\n') +
            "exec " + shlex.quote(compiler) + ' "$@"\n')
        wrapper.chmod(0o755)
        failed_output = root / (phase + "-output")
        invoke({"CC": str(wrapper), "CCACHE": "", "SIEVE_TOOLS_OUTPUT": str(failed_output)},
               1, "injected-" + phase)
        assert not (failed_output / "current").exists()
        passed("injected " + phase + " failure propagates without publication")

    # Use the real install rules, then remove the entire selected development prefix.
    (cloned / "bin").mkdir()
    shutil.copy2(prefix / "bin/doveconf", cloned / "bin/doveconf")
    build = root / "package build"
    install = root / "install with spaces"
    subprocess.run(["cmake", "-S", repo, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                    "-DJARO_SIEVE_DEPS_PREFIX=" + str(cloned)], check=True)
    subprocess.run(["cmake", "--build", build, "--target", "sieve-runtime"], check=True)
    subprocess.run(["cmake", "--install", build, "--component", "sieve", "--prefix", install], check=True)
    cloned.rename(root / "unavailable development prefix")
    moved = root / "relocated application"
    install.rename(moved)
    runtime = moved / "share/jaromail/sieve"
    assert (moved / "share/jaromail/licenses/dovecot/COPYING.MIT").is_file()
    assert (moved / "share/jaromail/licenses/pigeonhole/COPYING.LGPL").is_file()
    home = root / "empty home"
    home.mkdir()
    isolated = {"PATH": "/usr/bin:/bin", "HOME": str(home), "DOVECOT_CONFIG_FD": "wrong",
                "DOVECONF_PATH": "/missing", "CONFIG_MODULES": "/missing"}
    loader_env = dict(isolated, LD_LIBRARY_PATH=str(runtime / "lib"))
    for program in [*sorted((runtime / "bin").iterdir()), runtime / "lib/libpigeonhole_settings.so"]:
        mapping = subprocess.check_output(["ldd", program], env=loader_env, text=True)
        assert "not found" not in mapping, mapping
        for line in mapping.splitlines():
            if "libdovecot" in line:
                assert "=> " + str(runtime / "lib") + "/" in line, line
    policy = root / "sample policy.sieve"
    binary = root / "compiled policy.svbin"
    policy.write_text('require "fileinto"; fileinto "shopping";\n')
    def installed(*arguments, expected=0):
        result = subprocess.run([runtime / "run", *arguments], env=isolated, cwd=home,
                                text=True, capture_output=True)
        assert (result.returncode == 0) == (expected == 0), result
        return result
    installed("compile", policy, binary)
    original = binary.read_bytes()
    policy.write_text('not-a-command;')
    installed("compile", policy, binary, expected=1)
    assert binary.read_bytes() == original
    assert not list(root.glob(".jaro-sievec.*"))
    passed("failed compilation preserves previous bytecode and removes its temporary file")
    policy.unlink()
    dump = installed("dump", binary).stdout
    assert "FILEINTO" in dump and "shopping" in dump, dump
    assert not list(home.iterdir())
    passed("relocated staged runtime loads source-free fileinto binary from a private configuration")
    binary.write_bytes(b"invalid binary")
    assert "failed to load binary" in installed("dump", binary, expected=1).stderr
    (runtime / "lib/libdovecot-storage.so.0").unlink()
    assert "incomplete or changed pinned runtime" in installed("dump", binary, expected=1).stderr
    passed("incompatible binary and missing installed Maildir library diagnosed")

    (generation / "bin/sievec").write_bytes(b"corrupt output")
    invoke(expected=1, message="cached tool/development dependencies changed")
    passed("corrupt output is never reused")

print(f"Tests: {checks} passed, 0 failed, 0 skipped")
