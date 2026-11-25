"""Installed consumer check for a frozen two-envelope submission; SMTP is a recorder."""

from concurrent.futures import ThreadPoolExecutor
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

prefix = Path(sys.argv[1]).resolve()
cli = prefix / "bin/jaro"
with tempfile.TemporaryDirectory(prefix="envelope-probe-") as tmp:
    work = Path(tmp)
    mail, home, bin = (work / name for name in ("mail", "home", "bin"))
    for directory in [mail, home, bin]:
        directory.mkdir(mode=0o700)
    env = dict(
        os.environ,
        JAROMAILDIR=str(mail),
        JAROACCOUNT=str(mail / "Accounts"),
        HOME=str(home),
        PATH=str(bin) + os.pathsep + os.environ["PATH"],
        CALLS=str(work / "calls"),
    )

    def call(*args, input=b"", ok=True):
        result = subprocess.run(
            [cli, *args],
            cwd=work,
            env=env,
            input=input,
            capture_output=True,
            timeout=30,
        )
        assert (result.returncode == 0) == ok, (
            args,
            result.returncode,
            result.stderr[-300:],
        )
        return result.stdout

    call("init")
    fake = bin / "msmtp"
    fake.write_text(
        """#!/usr/bin/python3
import hashlib, json, os, sys
from pathlib import Path
cfg = Path(sys.argv[sys.argv.index('-C') + 1]).read_text().splitlines()
host = next(x for x in cfg if x.startswith('host '))
with open(os.environ['CALLS'], 'a') as f:
    print(json.dumps([host, sys.argv[3:], hashlib.sha256(sys.stdin.buffer.read()).hexdigest()]), file=f)
sys.exit(int(os.environ.get('FIXTURE_SMTP_RC','0')))
"""
    )
    fake.chmod(0o700)
    for name in ("default", "second"):
        (mail / "Accounts" / (name + ".txt")).write_text(
            f"name {name}\nemail {name}@example.test\nlogin {name}\nimap {name}.example.test\nsmtp {name}.example.test\npassword synthetic-local-fixture\n"
        )
    recipients = ["one@example.test", "two@example.test"]

    def envelope(address):
        data = b"From: signer@example.test\r\nTo: Undisclosed recipients:;\r\nSubject: Fixture\r\n\r\npayload\r\n"
        return {"mime": base64.b64encode(data).decode(), "recipients": [address]}

    message = {
        "version": 1,
        "key": "fixture-draft-v1",
        "envelopes": [envelope(r) for r in recipients],
    }

    def submit(body, ok=True):
        raw = call(
            "-a", "second", "outbox", "submit", input=json.dumps(body).encode(), ok=ok
        )
        return json.loads(raw)["envelopes"] if ok else []

    queued = submit(message)
    assert len(queued) == 2 and len({row["id"] for row in queued}) == 2
    assert queued == submit(message)
    assert {row["state"] for row in queued} == {"eligible"}
    submit(dict(message, envelopes=[envelope("other@example.test")]), False)
    call("-a", "default", "outbox", "send", queued[0]["id"], "--expect", "eligible")
    after = submit(message)
    assert [row["state"] for row in after] == ["complete", "eligible"]
    assert len(list((mail / "outbox/new").iterdir())) == 1
    call("-a", "default", "send")
    assert [row["state"] for row in submit(message)] == ["complete", "complete"]
    calls = [json.loads(row) for row in (work / "calls").read_text().splitlines()]
    assert [row[0] for row in calls] == ["host second.example.test"] * 2
    assert [row[1] for row in calls] == [["--", r] for r in recipients]
    print(
        "PASS lost response, payload conflict, targeted and bulk workers, immutable account and envelope"
    )

    # Kill a producer after publishing its first occurrence but before its
    # durable per-envelope marker. Restart through the installed front door.
    runtime = prefix / "share/jaromail"
    interrupted = dict(message, key="fixture-interruption")
    crash_script = """import importlib.util,os,sys
spec=importlib.util.spec_from_file_location('submission',sys.argv[1])
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
original=module.publish
def cut(stage,dest,name,data):
    original(stage,dest,name,data)
    if name.startswith('jaro-v1-'): os._exit(73)
module.publish=cut
sys.argv=sys.argv[1:]
module.main()
"""
    crash = subprocess.run(
        [
            sys.executable,
            "-c",
            crash_script,
            runtime / "outbox/submission.py",
            "submit",
            mail,
            runtime / "bin/jaro-outbox",
            "second",
        ],
        input=json.dumps(interrupted).encode(),
        env=env,
        cwd=work,
    )
    assert crash.returncode == 73 and len(list((mail / "outbox/new").iterdir())) == 1
    with ThreadPoolExecutor(4) as workers:
        replies = list(workers.map(submit, [interrupted] * 4))
    assert (
        replies.count(replies[0]) == 4
        and len(list((mail / "outbox/new").iterdir())) == 2
    )
    first, second = [r["id"] for r in replies[0]]
    env["FIXTURE_SMTP_RC"] = "69"
    call("outbox", "send", first, "--expect", "eligible", ok=False)
    assert submit(interrupted)[0]["state"] == "uncertain"
    count = lambda: len((work / "calls").read_text().splitlines())
    held_calls = count()
    call(
        "outbox",
        "retry",
        first,
        "--expect",
        "inflight",
        "--confirm-duplicate",
        ok=False,
    )
    call("outbox", "retry", first, "--expect", "uncertain", ok=False)
    assert count() == held_calls
    del env["FIXTURE_SMTP_RC"]
    call(
        "-a",
        "default",
        "outbox",
        "retry",
        first,
        "--expect",
        "uncertain",
        "--confirm-duplicate",
    )
    assert submit(interrupted)[0]["state"] == "complete"
    env["FIXTURE_SMTP_RC"] = "69"
    call("outbox", "send", second, "--expect", "eligible", ok=False)
    held_calls = count()
    call("outbox", "archive", second, "--expect", "uncertain")
    assert count() == held_calls and submit(interrupted)[1]["state"] == "complete"
    # A damaged producer marker must never resurrect a completed envelope.
    (mail / ".outbox-state/submissions" / (first + ".published")).unlink()
    assert submit(interrupted)[0]["state"] == "complete"
    assert not list((mail / "outbox/new").iterdir())
    calls = [json.loads(row) for row in (work / "calls").read_text().splitlines()]
    assert all(row[0] == "host second.example.test" for row in calls)
    assert [row[1] for row in calls[-3:]] == [
        ["--", recipients[0]],
        ["--", recipients[0]],
        ["--", recipients[1]],
    ]
    changed = envelope(recipients[0])
    changed["mime"] = envelope(recipients[0])["mime"][:-4] + "eg0K"
    submit(dict(interrupted, envelopes=[changed, envelope(recipients[1])]), False)
    print(
        "PASS producer interruption, concurrent redelivery, held uncertainty, explicit retry, archive and completed repair"
    )
