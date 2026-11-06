"""Source-built Pigeonhole oracle: simulate fileinto decisions in private fixtures.

This parses the pinned tool's result report, never the Sieve language. All script
and message inputs are copied before compilation/simulation. No delivery flag is
exposed. Future parity tests can call compile(), evaluate(), and expect().
"""

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


REPO = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures/sieve-oracle"


class OracleFailure(RuntimeError):
    pass


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fileinto_result(stdout):
    """Reject unknown reports, extra actions, implicit keep and other side effects."""
    match = re.fullmatch(
        r'\nPerformed actions:\n\n \* store message in folder: ([^\x00-\x1f]+)\n'
        r'(        \+ create mailbox if it does not exist\n)?'
        r'\nImplicit keep:\n\n  \(none\)\n\n', stdout)
    if match is None:
        raise OracleFailure("expected exactly one fileinto action and no implicit keep")
    return {"action": "fileinto", "mailbox": match[1], "create": match[2] is not None}


class SieveOracle:
    def __init__(self, work, deps_prefix, tools_output=None):
        self.work = Path(work).resolve()
        self.work.mkdir(parents=True, exist_ok=False)
        self.prefix = Path(deps_prefix).resolve(strict=True)
        self.output = Path(tools_output or os.environ.get(
            "SIEVE_TOOLS_OUTPUT", REPO / "build/sieve-tools")).resolve()
        self.events = []
        self.sequence = 0
        self.environment = dict(os.environ)
        # Never inherit a live service's configuration descriptor/cache/environment.
        for key in list(self.environment):
            if key.startswith("DOVECOT_") or key == "MASTER_IS_PARENT":
                self.environment.pop(key)
        libdir = self.prefix / "lib/dovecot"
        runtime = [self.prefix / "bin/doveconf", libdir / "settings/libpigeonhole_settings.so",
                   libdir / "libdovecot-managesieve.so"]
        for path in runtime:
            if not path.is_file() or not path.resolve().is_relative_to(self.prefix):
                raise OracleFailure("missing matched private configuration prerequisite: " + str(path))
        if b"Pigeonhole version 2.4.5 (742bb892)" not in runtime[1].read_bytes():
            raise OracleFailure("Pigeonhole configuration module version/revision mismatch")
        self.environment.update(LD_LIBRARY_PATH=str(libdir),
                                DOVECONF_PATH=str(runtime[0]), CONFIG_MODULES=str(runtime[1]),
                                USER="jaromail-fixture", HOME=str(self.work / "home"))
        for directory in ("home", "mail", "run", "state"):
            (self.work / directory).mkdir(mode=0o700)
        template = (FIXTURES / "dovecot.conf.in").read_text()
        for token, directory in (("RUN", "run"), ("STATE", "state"), ("MAIL", "mail"), ("HOME", "home")):
            value = str(self.work / directory).replace("\\", "\\\\").replace('"', '\\"')
            if any(ord(c) < 32 for c in value):
                raise OracleFailure("control byte in private configuration path")
            template = template.replace("@" + token + "@", '"' + value + '"')
        self.config = self.work / "dovecot.conf"
        self.config.write_text(template)
        build_env = dict(self.environment, SIEVE_DEPS_PREFIX=str(self.prefix),
                         SIEVE_TOOLS_OUTPUT=str(self.output))
        self._run([sys.executable, REPO / "build/sieve-tools.py"], self.work / "build", build_env)
        configuration = self._run([runtime[0], "-c", self.config, "-n"], self.work / "configuration")
        if (not re.search(r"^# 2\.4\.5 \([0-9a-f]+\): ", configuration.stdout, re.M) or
                "# Pigeonhole version 2.4.5 (742bb892)\n" not in configuration.stdout or
                re.search(r"^(?:userdb|passdb)\b", configuration.stdout, re.M)):
            raise OracleFailure("configuration tool/version differs or a user database is configured")
        self.generation = (self.output / "current").resolve(strict=True)
        self.tools = {name: self.generation / "bin" / name for name in ("sievec", "sieve-test")}
        build_record = self.generation / "build.json"
        self.provenance = {
            "development_prefix": str(self.prefix), "build_record": str(build_record),
            "build_record_sha256": sha(build_record),
            "runtime": {str(p): sha(p) for p in runtime},
            "tools": {str(p): sha(p) for p in self.tools.values()},
            "configuration_sha256": sha(self.config)}
        self._save()

    def _save(self):
        (self.work / "oracle.json").write_text(json.dumps({
            "provenance": getattr(self, "provenance", None), "events": self.events},
            indent=2, sort_keys=True) + "\n")

    def _run(self, command, logs, env=None, succeeds=True):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=self.work, env=env or self.environment,
                                capture_output=True, text=True)
        stdout, stderr = Path(str(logs) + ".stdout"), Path(str(logs) + ".stderr")
        stdout.write_text(result.stdout)
        stderr.write_text(result.stderr)
        self.events.append({"command": command, "cwd": str(self.work),
                            "environment": {k: (env or self.environment).get(k) for k in (
                                "LD_LIBRARY_PATH", "DOVECONF_PATH", "CONFIG_MODULES", "USER", "HOME",
                                "SIEVE_DEPS_PREFIX", "SIEVE_TOOLS_OUTPUT", "CCACHE_DIR")},
                            "exit": result.returncode, "expected_success": succeeds,
                            "observedAt": datetime.now(timezone.utc).isoformat(),
                            "stdout": {"path": str(stdout), "sha256": sha(stdout)},
                            "stderr": {"path": str(stderr), "sha256": sha(stderr)}})
        self._save()
        if succeeds and result.returncode != 0:
            raise OracleFailure("tool failed, see " + str(stderr))
        if not succeeds and (result.returncode not in (1, 89) or not re.search(
                r"^policy:(?: line [0-9]+:)? error:", result.stderr, re.M)):
            raise OracleFailure("expected a script/capability compilation error, see " + str(stderr))
        return result

    def _inputs(self, script, message=None):
        self.sequence += 1
        run = self.work / ("case-%03d" % self.sequence)
        run.mkdir(mode=0o700)
        script = Path(script)
        source = script.read_bytes()
        (run / "policy.sieve").write_bytes(source)
        before = {script: source}
        if message is not None:
            message = Path(message)
            source = message.read_bytes()
            (run / "message.eml").write_bytes(source)
            before[message] = source
        return run, before

    def _unchanged(self, run, before):
        for path, contents in before.items():
            if path.read_bytes() != contents:
                raise OracleFailure("oracle modified input " + str(path))
        if (run / "policy.sieve").read_bytes() != next(iter(before.values())):
            raise OracleFailure("oracle modified copied script")
        if len(before) == 2 and (run / "message.eml").read_bytes() != list(before.values())[1]:
            raise OracleFailure("oracle modified copied message")
        for directory in ("mail", "home"):
            if any((self.work / directory).iterdir()):
                raise OracleFailure("simulation wrote private " + directory + " contents")

    def compile(self, script, extensions=None, succeeds=True):
        run, before = self._inputs(script)
        command = [self.tools["sievec"], "-c", self.config]
        if extensions is not None:
            command += ["-x", extensions]
        command += [run / "policy.sieve", run / "policy.svbin"]
        self._run(command, run / "compile", succeeds=succeeds)
        binary = run / "policy.svbin"
        if succeeds and (not binary.is_file() or binary.read_bytes()[:4] != b"\xbe\xba\xfe\xca"):
            raise OracleFailure("compiler did not produce a Pigeonhole binary")
        if not succeeds and binary.exists():
            raise OracleFailure("failed compilation published a binary")
        self._unchanged(run, before)
        return binary if succeeds else None

    def evaluate(self, script, message, extensions=None):
        run, before = self._inputs(script, message)
        trace = run / "trace.txt"
        # -C compiles current source; deliberately no -e, -u, -s, plugin or delivery options.
        command = [self.tools["sieve-test"], "-c", self.config, "-C", "-t", trace,
                   "-T", "level=matching", run / "policy.sieve", run / "message.eml"]
        if extensions is not None:
            command[3:3] = ["-x", extensions]
        result = self._run(command, run / "simulate")
        self._unchanged(run, before)
        trace_text = trace.read_text()
        if ("## Started executing script 'policy'" not in trace_text or
                "## Finished executing script 'policy'" not in trace_text):
            raise OracleFailure("missing complete interpreter trace")
        action = fileinto_result(result.stdout)
        (run / "decision.json").write_text(json.dumps(action, sort_keys=True) + "\n")
        return action

    def expect(self, script, message, mailbox, create=False, extensions=None):
        result = self.evaluate(script, message, extensions)
        expected = {"action": "fileinto", "mailbox": mailbox, "create": create}
        if result != expected:
            raise OracleFailure(f"oracle decision {result!r} differs from expected {expected!r}")
        return result
