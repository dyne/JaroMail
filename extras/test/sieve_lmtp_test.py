"""Physically deliver one generated policy via installed JaroMail and pinned LMTP."""
from contextlib import contextmanager
from email.parser import BytesHeaderParser
import grp
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import smtplib
import subprocess
import tempfile
import time

from filter_batch_test import REPO, write_policy_fixture
from sieve_maildir_test import maildir


SENDER = "transport@example.org"
RCPT = "fixture@example.net"
RULES = ["to-glob *+shop@*.ch move shopping",
         "from-address editor@example.org move Projects/Letters",
         "to-glob literal?@example.org move literal",
         "known-domain contacts.example"]
# Subject, winning mailbox, and input header overrides.
CASES = [
    ("shop", "shopping", {"To": "lea+shop@example.ch"}),
    ("deep", "shopping", {"To": "a+b+shop@mx.example.ch"}),
    ("empty-star", "shopping", {"Cc": "+shop@example.ch"}),
    ("case", "shopping", {"Cc": "LEA+SHOP@EXAMPLE.CH"}),
    ("no-tag", "unsorted", {"To": "lea@example.ch"}),
    ("long-tag", "unsorted", {"To": "lea+shopping@example.ch"}),
    ("suffix", "unsorted", {"To": "lea+shop@example.ch.example.org"}),
    ("split", "unsorted", {"To": "lea+shop@example.org", "Cc": "bob@example.ch"}),
    ("question", "literal", {"To": "literal?@example.org"}),
    ("question-negative", "unsorted", {"To": "literalx@example.org"}),
    ("nested", "Projects/Letters", {"Sender": "editor@example.org", "To": "lea+shop@example.ch"}),
    ("known", "INBOX", {"From": "known@example.org", "X-Spam-Flag": "YES"}),
    ("domain", "INBOX", {"From": "new@contacts.example"}),
    ("spam", "Spam", {"To": "own@example.org", "X-Spam-Flag": "yes"}),
    ("own", "priv", {"Cc": "own@example.org"}),
    ("blacklist", "zz.blacklist", {"From": "blocked@example.org", "To": "lea+shop@example.ch"}),
    ("bounce", "zz.bounces", {"Sender": "mailman-bounce@example.org"}),
]


def message(name, headers):
    values = {"Return-Path": "<" + SENDER + ">", "Envelope-To": RCPT,
              "From": "stranger@example.org", "To": "other@example.org", **headers,
              "Message-ID": f"<{name}@qualification.invalid>", "Subject": name}
    lines = [f"{key}: {value}" for key, value in values.items()]
    return ("\r\n".join([*lines, "", f"Body for {name}.", ".dot-prefixed", "", "last line", ""])).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, obj):
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n")


def config(root, run, missing):
    user = pwd.getpwuid(os.getuid()).pw_name
    group = grp.getgrgid(os.getgid()).gr_name
    users = root / "users"
    users.write_text(f"{RCPT}:*:{os.getuid()}:{os.getgid()}::{root}:\n")
    users.chmod(0o600)
    config_file = root / "dovecot.conf"
    config_file.write_text(f"""dovecot_config_version = 2.4.5
dovecot_storage_version = 2.4.5
protocols {{
  lmtp = yes
}}
hostname = fixture.example.net
postmaster_address = postmaster@example.net
ssl = no
base_dir = "{run}"
state_dir = "{root}/state"
log_path = "{root}/server.log"
info_log_path = "{root}/server.log"
debug_log_path = "{root}/server.log"
log_debug = category=sieve
default_internal_user = {user}
default_login_user = {user}
default_internal_group = {group}
first_valid_uid = {os.getuid()}
last_valid_uid = {os.getuid()}
mail_uid = {os.getuid()}
mail_gid = {os.getgid()}
mail_home = "{root}"
mail_driver = maildir
mail_path = "{root}/mail"
mail_inbox_path = "{root}/mail/INBOX"
mailbox_list_layout = fs
mail_save_crlf = yes
namespace inbox {{
  inbox = yes
  separator = /
}}
passdb passwd-file {{
  passwd_file_path = "{users}"
}}
userdb passwd-file {{
  passwd_file_path = "{users}"
}}
service anvil {{
  chroot =
}}
service auth-worker {{
  user = {user}
}}
service lmtp {{
  user = {user}
  unix_listener lmtp {{
    mode = 0600
  }}
}}
protocol lmtp {{
  mail_plugins {{
    sieve = yes
  }}
}}
sieve_script personal {{
  driver = file
  path = "{root}/policy/Filters.sieve"
}}
""")
    if missing:
        with config_file.open("a") as f:
            f.write("sieve_extensions {\n  mailbox = no\n}\n")
    return config_file


@contextmanager
def server(root, prefix, missing=False):
    # UNIX-domain socket names have a short length limit; keep this private
    # runtime separate from the possibly much longer retained evidence path.
    with tempfile.TemporaryDirectory(prefix="lmtp-") as sockets, (root / "master.log").open("wb") as log:
        run = Path(sockets) / "run"
        conf = config(root, run, missing)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("DOVECOT_") and k not in ("CONFIG_MODULES", "MASTER_IS_PARENT")}
        env["LD_LIBRARY_PATH"] = str(prefix / "lib/dovecot")
        check = subprocess.run([prefix / "bin/doveconf", "-c", conf, "-n"], env=env, capture_output=True)
        (root / "effective.conf").write_bytes(check.stdout)
        assert check.returncode == 0, check.stderr
        proc = subprocess.Popen([prefix / "sbin/dovecot", "-F", "-c", conf], stdout=log, stderr=log,
                                env=env, start_new_session=True)
        sock = str(run / "lmtp")
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and proc.poll() is None:
                try:
                    client = smtplib.LMTP(sock, local_hostname="client.example.net", timeout=2)
                    break
                except (OSError, smtplib.SMTPServerDisconnected):
                    time.sleep(.05)
            else:
                logs = [p.read_text(errors="replace")[-2000:] for p in (root/"server.log",root/"master.log") if p.exists()]
                raise RuntimeError(f"LMTP did not start: {logs}")
            with client:
                yield client
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()


def inventory(root, inputs, expected, local):
    found = {}
    for path in sorted(root.rglob("*")):
        if path.parent.name not in ("new", "cur") or not path.is_file():
            continue
        data = path.read_bytes()
        headers, separator, body = data.partition(b"\r\n\r\n")
        assert separator, path
        parsed = BytesHeaderParser().parsebytes(headers)
        name = parsed["Subject"]
        original = inputs[name]
        assert name not in found and parsed["Message-ID"] == f"<{name}@qualification.invalid>"
        assert str(path.parent.parent.relative_to(root)) == expected[name], (name, path, expected[name])
        assert body == original.split(separator, 1)[1], path
        if local:
            assert data == original, path
            added = []
        else:
            assert data.endswith(original), path
            transport = BytesHeaderParser().parsebytes(data[:-len(original)])
            assert transport["Delivered-To"] == RCPT and transport["Return-Path"] == f"<{SENDER}>"
            assert transport["Received"] and parsed["Envelope-To"] == RCPT
            added = list(transport)
        found[name] = dict(path=str(path.relative_to(root)), mailbox=expected[name],
                           input_sha256=digest(original), message_sha256=digest(data), body_sha256=digest(body),
                           added_headers=added, received=parsed.get_all("Received", []))
    assert found.keys() == inputs.keys(), (found.keys(), inputs.keys())
    return found


def qualify(work):
    prefix = Path(os.environ["SIEVE_DEPS_PREFIX"]).resolve(strict=True)
    version = subprocess.check_output([prefix / "sbin/dovecot", "--version"], text=True).strip()
    assert version.split()[0] == "2.4.5", version
    # Exercise an actual CMake install. A pre-staged install can be reused in
    # packaging checks by explicitly naming it.
    if "JARO_SIEVE_RELEASE_STAGE" in os.environ:
        stage = Path(os.environ["JARO_SIEVE_RELEASE_STAGE"]).resolve(strict=True)
    else:
        stage, build = work / "install with spaces", work / "build"
        subprocess.run(["cmake", "-S", REPO, "-B", build, "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["cmake", "--build", build, "--parallel"], check=True)
        subprocess.run(["cmake", "--install", build, "--prefix", stage], check=True)
    tools = stage / "share/jaromail/bin"
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
    local = work / "local"
    local.mkdir(mode=0o700)
    source = maildir(local / "incoming")
    (local / "Accounts").mkdir()
    write_policy_fixture(local, RULES, known=["known@example.org"], blacklist=["blocked@example.org"],
                         own=["own@example.org"])
    compiled = subprocess.check_output([tools / "jaro-filter-compile", "--root", local, "--report0"], env=env)
    magic, gen, changed, directory, trailer = compiled.split(b"\0")
    assert (magic, changed, trailer) == (b"JFC1", b"1", b"")
    generation = Path(os.fsdecode(directory))
    script = generation / "Filters.sieve"
    binary = generation / "Filters.svbin"
    src_bytes = script.read_bytes()
    assert (local / "Filters.sieve").read_bytes() == src_bytes
    local_binary = digest(binary.read_bytes())
    dump = subprocess.check_output([stage / "share/jaromail/sieve/run", "dump", binary])
    (work/"local-binary.dump").write_bytes(dump)
    assert b"FILEINTO" in dump.upper()
    inputs = {name:message(name, h) for name, _, h in CASES}
    expected = {name:dest for name, dest, _ in CASES}
    for name, data in inputs.items():
        (source / "new" / name).write_bytes(data)
    applied = subprocess.check_output([stage / "bin/jaro-sieve-filter", "--from", source, "--to", local,
                                      "--report0", gen, script], env=env)
    assert applied.startswith(b"JFD1\0" + gen + b"\0")
    (work / "local.report0").write_bytes(applied)
    local_mail = inventory(local, inputs, expected, True)
    assert digest(binary.read_bytes()) == local_binary
    standalone = work / "standalone"
    incoming = maildir(standalone / "incoming")
    for name, data in inputs.items():
        (incoming / "new" / name).write_bytes(data)
    subprocess.run([stage / "bin/jaro-sieve-filter", "--from", incoming, "--to", standalone,
                    local / "Filters.sieve"], check=True, env=env)
    result = dict(dovecot=version, generation=gen.decode(), source_sha256=digest(src_bytes),
                  local_runtime_sha256=digest((stage / "share/jaromail/sieve/SHA256SUMS").read_bytes()),
                  local_binary_sha256=local_binary, local=local_mail, mail_from=SENDER, rcpt_to=RCPT,
                  standalone=inventory(standalone, inputs, expected, True))
    for missing in (False, True):
        root = work / ("missing-extension" if missing else "server")
        policy = root / "policy"
        policy.mkdir(parents=True)
        remote = policy / "Filters.sieve"
        shutil.copyfile(local / "Filters.sieve", remote)
        assert list(policy.iterdir()) == [remote] and remote.read_bytes() == src_bytes
        with server(root, prefix, missing) as client:
            selected = {"shop":inputs["shop"]} if missing else inputs
            for name, data in selected.items():
                assert client.sendmail(SENDER, [RCPT], data) == {}, name
        log = (root / "server.log").read_text()
        assert "Pigeonhole version 2.4.5 " in log, log[-3000:]
        if missing:
            assert "failed to compile script 'personal/filters'" in log.lower(), log[-3000:]
            assert "unknown Sieve capability 'mailbox'" in (policy / "Filters.sieve.log").read_text()
            assert not list(policy.glob("*.svbin"))
            result["missing_extension"] = inventory(root / "mail", selected, {"shop":"INBOX"}, False)
        else:
            svbin, = policy.glob("*.svbin")
            compiles = log.count("Script 'personal/Filters' successfully compiled")
            loads = log.count(f"Script binary {svbin} successfully loaded")
            assert (compiles, loads) == (1, len(inputs) - 1), (compiles, loads)
            assert not binary.samefile(svbin)
            result.update(server_source_sha256=digest(remote.read_bytes()), server_binary_sha256=digest(svbin.read_bytes()),
                          server_policy_compilations=compiles, server_policy_loads=loads,
                          server=inventory(root / "mail", selected, expected, False))
        write_json(work / "delivery.json", result)
    print(f"PASS {len(inputs)} matching local and LMTP physical deliveries; missing mailbox capability checked")


if __name__ == "__main__":
    retained = os.environ.get("JARO_SIEVE_RELEASE_WORK")
    if retained:
        workspace = Path(retained).resolve()
        workspace.mkdir(mode=0o700, parents=True)
        qualify(workspace)
    else:
        with tempfile.TemporaryDirectory(prefix="sieve-release-") as directory:
            qualify(Path(directory))
