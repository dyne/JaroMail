#!/usr/bin/env python3
"""Check local skip versus strict failure and explicit prerequisite rejection."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="jaro-sieve-ci-") as temp:
    root = Path(temp)
    env = dict(os.environ)
    for key in ("SIEVE_DEPS_PREFIX", "SIEVE_ORACLE_EVIDENCE"):
        env.pop(key, None)
    local = subprocess.run(["zsh", "extras/test/test-sieve-oracles.sh"], cwd=repo,
                           env=env, capture_output=True, text=True)
    assert local.returncode == 77 and local.stdout.startswith("SKIP "), local
    strict = subprocess.run(["zsh", "extras/test/run-tests.sh", "extras/test/test-sieve-oracles.sh"],
                            cwd=repo, env=dict(env, JARO_TEST_REQUIRE_ALL="1"), capture_output=True, text=True)
    assert strict.returncode == 1 and "0 passed, 0 failed, 1 skipped" in strict.stdout, strict
    print("PASS missing prerequisites skip locally and fail strict selection", flush=True)
    selected = os.environ.get("SIEVE_DEPS_PREFIX")
    # Independent negative paths also run without a populated developer prefix.
    absent = subprocess.run(["zsh", "extras/test/test-sieve-oracles.sh"], cwd=repo,
                            env=dict(env, SIEVE_DEPS_PREFIX=str(root / "absent")),
                            capture_output=True, text=True)
    assert absent.returncode not in (0, 77), absent
    print("PASS explicitly missing development prefix fails", flush=True)
    if selected:
        prefix = Path(selected).resolve()
        wrong = root / "wrong"
        for relative in ("bin/doveconf", "lib/dovecot/settings/libpigeonhole_settings.so",
                         "lib/dovecot/libdovecot-managesieve.so"):
            target = wrong / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(prefix / relative, target)
        header = wrong / "include/dovecot/config.h"
        header.parent.mkdir(parents=True)
        header.write_text('#define DOVECOT_VERSION "2.4.1"\n')
        records = root / "wrong-records"
        result = subprocess.run(["zsh", "extras/test/test-sieve-oracles.sh"], cwd=repo,
            env=dict(env, SIEVE_DEPS_PREFIX=str(wrong), SIEVE_ORACLE_EVIDENCE=str(records)),
            capture_output=True, text=True)
        assert result.returncode not in (0, 77), result
        diagnostic = (records / "oracle with spaces/build.stderr").read_text()
        assert "Dovecot development version must be 2.4.5" in diagnostic, diagnostic
        print("PASS wrong development version cannot substitute for strict oracle", flush=True)
        (wrong / "bin/doveconf").unlink()
        result = subprocess.run(["zsh", "extras/test/test-sieve-oracles.sh"], cwd=repo,
            env=dict(env, SIEVE_DEPS_PREFIX=str(wrong)), capture_output=True, text=True)
        assert result.returncode not in (0, 77) and "missing matched private configuration prerequisite" in result.stderr
        print("PASS missing private configuration tool cannot use a system substitute", flush=True)
    else:
        print("SKIP sieve-ci: set SIEVE_DEPS_PREFIX for wrong-version/missing-tool cases")
        sys.exit(77)
print("Tests: 5 passed, 0 failed, 0 skipped")
