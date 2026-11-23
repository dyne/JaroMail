"""Inspect the installed Pigeonhole CLI and independent physical Maildir inventories."""

import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import runpy
import shutil
import re
import select
import signal
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

from filter_batch_test import authored, authored_groups, write_policy_fixture
from filter_cross_device_test import prerequisite
from filter_storage_test import body_files, digest


REPO = Path(__file__).resolve().parents[2]


def maildir(root):
    for name in ("cur", "new", "tmp"):
        (root / name).mkdir(parents=True)
    return root


def tree(root):
    result = {}
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        value = (os.readlink(path) if stat.S_ISLNK(mode) else
                 path.read_bytes() if stat.S_ISREG(mode) else None)
        result[str(path.relative_to(root))] = (mode, value)
    return result


def message(subject, body):
    return f"From: user@example.org\nSubject: {subject}\n\n{body}\n".encode()


class MaildirTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="jaro-sieve-test-",
                                              dir=os.environ.get("JARO_FILTER_TEST_TMP"))
        cls.addClassCleanup(cls.work.cleanup)
        root = Path(cls.work.name)
        build, install = root / "build", root / "stage with spaces"
        launcher = ["-DCMAKE_C_COMPILER_LAUNCHER=ccache"] if shutil.which("ccache") else []
        subprocess.run(["cmake", "-S", REPO, "-B", build, "-DCMAKE_BUILD_TYPE=Release", *launcher], check=True)
        subprocess.run(["cmake", "--build", build, "--parallel", "--target", "jaro-filter-compile", "abook"], check=True)
        subprocess.run(["cmake", "--install", build, "--component", "sieve", "--prefix", install], check=True)
        cls.cli = install / "bin/jaro-sieve-filter"
        cls.compiler = build / "bin/jaro-filter-compile"
        cls.runtime = install / "share/jaromail/sieve"

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="mail with spaces ", dir=self.work.name)
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.base = self.root / "destination folders"
        self.base.mkdir()
        self.source = maildir(self.root / "incoming mail")
        self.script = self.root / "policy with spaces.sieve"
        self.script.write_text('require ["fileinto", "mailbox"]; fileinto :create "INBOX";')
        self.outside = maildir(self.root / "outside")
        (self.outside / "new/sentinel").write_bytes(b"outside sentinel")

    def invoke(self, *args, success=True, script=None, source=None, base=None):
        result = subprocess.run([self.cli, *args, "--from", source or self.source,
                                 "--to", base or self.base, script or self.script],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result)
        self.assertEqual((self.outside / "new/sentinel").read_bytes(), b"outside sentinel")
        return result

    @contextmanager
    def held_save(self, fail_expunge=False, after_publish=False):
        # The instrumented copy uses the real executable and runtime. Only its
        # final kernel publication call waits for an explicit release.
        stage = self.root / "held installation"
        shutil.copytree(self.cli.parents[1], stage)
        runtime = stage / "share/jaromail/sieve"
        worker = runtime / "bin/maildir-filter"
        native = worker.with_suffix(".real")
        worker.rename(native)
        shim = self.root / "save gate.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Werror",
                        REPO / "extras/test/sieve-save-gate.c", "-o", shim], check=True)
        ready, go = self.root / "ready", self.root / "go"
        for pipe in (ready, go):
            os.mkfifo(pipe)
        worker.write_text("#!/usr/bin/python3 -I\nimport os, sys\n"
                          f"ready = os.open({str(ready)!r}, os.O_WRONLY)\n"
                          f"go = os.open({str(go)!r}, os.O_RDONLY)\n"
                          f"lib = os.open({str(shim)!r}, os.O_RDONLY)\n"
                          "for fd in (ready, go, lib): os.set_inheritable(fd, True)\n"
                          f"os.environ['SIEVE_TEST_AFTER_PUBLISH'] = {str(int(after_publish))!r}\n"
                          f"os.environ['SIEVE_TEST_EXPUNGE_ERROR'] = {str(int(fail_expunge))!r}\n"
                          "os.environ.update(SIEVE_TEST_READY_FD=str(ready), SIEVE_TEST_GO_FD=str(go), "
                          "LD_PRELOAD=f'/proc/self/fd/{lib}')\n"
                          f"os.execv({str(native)!r}, sys.argv)\n")
        worker.chmod(0o755)
        sums = runtime / "SHA256SUMS"
        sums.write_text("".join(hashlib.sha256(worker.read_bytes()).hexdigest() + "  bin/maildir-filter\n"
                                if line.endswith("  bin/maildir-filter\n") else line
                                for line in sums.read_text().splitlines(True)))
        read_fd = os.open(ready, os.O_RDWR | os.O_NONBLOCK)
        go_fd = os.open(go, os.O_RDWR)
        child = subprocess.Popen([stage / "bin/jaro-sieve-filter", "--from", self.source,
                                  "--to", self.base, self.script], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=dict(os.environ, TMPDIR=str(self.root)))
        try:
            self.assertTrue(select.select([read_fd], [], [], 15)[0], "no destination publication reached")
            pid = int(os.read(read_fd, 32))
            yield child, pid
        finally:
            os.write(go_fd, b"G")
            os.close(go_fd)
            os.close(read_fd)
            try:
                output, err = child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate(timeout=5)
                raise
            self.held_result = subprocess.CompletedProcess(child.args, child.returncode, output, err)

    def test_empty_and_preview_preserve_mail_and_source(self):
        before = tree(self.root)
        self.invoke()
        self.assertEqual(tree(self.root), before)
        (self.source / "new/m").write_bytes(message("incoming", "preview"))
        before = tree(self.root)
        result = self.invoke("--dry-run")
        self.assertIn(b"store message in folder: INBOX", result.stdout)
        self.assertEqual(tree(self.root), before)

    def test_workflow_limit_precedes_delivery(self):
        (self.base / "Filters.txt").write_text("from-address user@example.org move Archive\n")
        subprocess.run([self.compiler, "--root", self.base], check=True)
        current = (self.base / "cache/filter-engine/current").resolve()
        (self.source / "new/retained").write_bytes(message("limit", "retain"))
        installed = runpy.run_path(str(self.runtime / "filter"))
        original = os.listdir
        def large_inventory(fd):
            if isinstance(fd, int) and os.path.samestat(os.fstat(fd), (self.source / "new").stat()):
                return [str(i) for i in range(100_001)]
            return original(fd)
        args = [str(self.cli), "--from", str(self.source), "--to", str(self.base),
                "--report0", current.name, str(current / "Filters.sieve")]
        with mock.patch("sys.argv", args), mock.patch("os.listdir", large_inventory):
            with self.assertRaises(installed["FilterError"]) as error:
                installed["main"]()
        self.assertEqual(error.exception.status, 2)
        self.assertEqual((self.source / "new/retained").read_bytes(), message("limit", "retain"))
        self.assertFalse((self.base / "Archive").exists())

    def test_preview_mailboxexists_uses_existing_folders(self):
        self.script.write_text('require ["fileinto","mailbox"]; if mailboxexists "A" '
                               '{ fileinto :create "B"; } else { fileinto :create "A"; }')
        for name in ("first", "second"):
            (self.source / "new" / name).write_bytes(message(name, name))
        before = tree(self.root)
        self.assertEqual(self.invoke("--dry-run").stdout.count(b"store message in folder: A"), 2)
        self.assertEqual(tree(self.root), before)
        self.invoke()
        for name, folder in (("first", "A"), ("second", "B")):
            self.assertEqual([p.read_bytes() for p in (self.base / folder / "cur").iterdir()],
                             [message(name, name)])

    def test_mirrored_folders_and_keeps(self):
        self.source = maildir(self.base / "incoming with spaces")
        self.script.write_text("""
require ["fileinto", "mailbox"];
if header :is "Subject" "inbox" { fileinto :create "INBOX"; stop; }
if header :is "Subject" "spam" { fileinto :create "Spam"; stop; }
if header :is "Subject" "nested" { fileinto :create "list/Archives 2026"; stop; }
if header :is "Subject" "explicit" { keep; stop; }
""")
        routes = {"inbox": self.base / "INBOX", "spam": self.base / "Spam",
                  "nested": self.base / "list/Archives 2026", "explicit": self.source,
                  "implicit": self.source}
        for name in routes:
            (self.source / "new" / name).write_bytes(message(name, name))
        self.invoke()
        for name, folder in routes.items():
            where = "new" if folder == self.source else "cur"
            actual = [p for p in (folder / where).iterdir() if p.read_bytes() == message(name, name)]
            self.assertEqual(len(actual), 1, (name, folder))
        self.assertEqual({p.name for p in (self.source / "new").iterdir()}, {"explicit", "implicit"})

    def test_invalid_roots_and_actions_preserve_all_inputs(self):
        (self.source / "new/starts-first").write_bytes(message("first", "retain"))
        missing = self.root / "missing"
        for kwargs in (dict(source=missing), dict(base=missing), dict(source=self.base)):
            before = tree(self.root)
            self.assertEqual(self.invoke(success=False, **kwargs).returncode, 3)
            self.assertEqual(tree(self.root), before)
        for command in ('redirect "nobody@example.org"', "discard", 'reject "reason"',
                        'vacation "out"', "invalid-syntax"):
            self.script.write_text('require ["fileinto","mailbox"]; if true {fileinto :create "Spam"; stop;} '
                                   'if false { ' + command + '; }')
            before = tree(self.root)
            self.assertEqual(self.invoke(success=False).returncode, 4)
            self.assertEqual(tree(self.root), before)

    def test_traversal_and_symlink_destinations_preserve_sentinel(self):
        (self.source / "new/input").write_bytes(message("input", "retain"))
        (self.base / "linked").symlink_to(self.outside, target_is_directory=True)
        box = maildir(self.base / "bad child")
        (box / "tmp").rmdir()
        (box / "tmp").symlink_to(self.outside / "tmp", target_is_directory=True)
        for folder in ("../outside", "/absolute", "a/../outside", "linked", "linked/nested",
                       "bad child", ".jaro-source/mail", "cur/new"):
            self.script.write_text('require ["fileinto","mailbox"]; fileinto :create "' + folder + '";')
            before = tree(self.root)
            self.invoke(success=False)
            self.assertEqual(tree(self.root), before)
        alias = self.root / "aliased root"
        alias.symlink_to(self.outside, target_is_directory=True)
        before = tree(self.root)
        self.invoke(success=False, base=alias)
        self.invoke(success=False, source=alias)
        self.assertEqual(tree(self.root), before)

    def test_verified_generation_and_stale_bytecode(self):
        policy = self.root / "policy"
        policy.mkdir()
        (policy / "Filters.txt").write_text("from-address user@example.org move Generated\n")
        subprocess.run([self.compiler, "--root", policy], check=True)
        generation = policy / "cache/filter-engine/current"
        script, binary = generation / "Filters.sieve", generation / "Filters.svbin"
        (self.source / "new/generated").write_bytes(message("generated", "cache hit"))
        before = tree(self.root)
        result = self.invoke("--dry-run", script=script)
        self.assertIn(b"store message in folder: Generated", result.stdout)
        self.assertEqual(tree(self.root), before)
        # Even a bytecode file with a matching manifest digest must obey the
        # delivery action allowlist when opened by the runtime.
        self.script.write_text('if header :is "Subject" "never" { discard; }')
        foreign = self.root / "other binary"
        subprocess.run([self.runtime / "run", "compile", self.script, foreign], check=True)
        binary.write_bytes(foreign.read_bytes())
        before = tree(self.root)
        self.assertEqual(self.invoke(script=script, success=False).returncode, 5)
        self.assertEqual(tree(self.root), before)
        manifest = generation / "manifest.json"
        metadata = json.loads(manifest.read_bytes())
        metadata["svbin_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(metadata, separators=(",", ":")) + "\n")
        before = tree(self.root)
        failure = self.invoke(script=script, success=False)
        self.assertEqual(failure.returncode, 5)
        self.assertIn(b"Unsupported operation in cached Sieve binary: DISCARD", failure.stderr)
        self.assertEqual(tree(self.root), before)

    def test_standalone_source_ignores_foreign_adjacent_binary(self):
        adjacent = self.script.with_suffix(".svbin")
        adjacent.write_bytes(b"foreign Sieve build")
        (self.source / "new/local").write_bytes(message("local", "portable source"))
        self.invoke()
        self.assertEqual(adjacent.read_bytes(), b"foreign Sieve build")
        self.assertEqual([p.read_bytes() for p in (self.base / "INBOX/cur").iterdir()],
                         [message("local", "portable source")])

    def test_distinct_devices(self):
        with tempfile.TemporaryDirectory(dir=os.environ["JARO_FILTER_CROSS_FS_DIR"]) as cross:
            source = maildir(Path(cross) / "incoming")
            self.assertNotEqual(source.stat().st_dev, self.base.stat().st_dev)
            blob = b"Subject: cross device\r\n\r\nbinary\x00\xff\nCRLF\r\nfinal CR\r"
            (source / "new/message:2,FS").write_bytes(blob)
            (source / "new/message:2,FS").chmod(0o640)
            self.invoke(source=source)
            self.assertEqual(list((source / "new").iterdir()), [])
            target = self.base / "INBOX/cur/message:2,FS"
            self.assertEqual((target.read_bytes(), stat.S_IMODE(target.stat().st_mode)), (blob, 0o600))

    def test_all_52_authored_delivery_occurrences(self):
        expected = {}
        witnesses = {}
        evidence = []
        policy = self.root / "policy"
        policy.mkdir()
        witness_base = self.root / "witness folders"
        witness_base.mkdir()
        for group, (cases, rules, contacts) in enumerate(authored_groups()):
            books = {key: contacts.get(key, []) for key in ("known", "blacklist", "own")}
            write_policy_fixture(policy, rules, **books)
            compiler_env = dict(os.environ, PATH=str(self.compiler.parent) + os.pathsep + os.environ["PATH"])
            subprocess.run([self.compiler, "--root", policy], env=compiler_env, check=True)
            product = policy / "cache/filter-engine/current"
            # Distinguish rules that intentionally share a mailbox. In a second
            # pass label every fileinto with its rule ID, retaining the generated
            # tests, order and stop statements. Expected IDs come from L1.
            from filter_compiler_test import provenance
            ids = (rule.decode() for _, rule, _, _ in provenance(product))
            witness = re.sub(r'fileinto :create "[^"]+";', lambda m: f'fileinto :create "{next(ids)}";',
                             (product / "Filters.sieve").read_text())
            self.assertIsNone(next(ids, None))
            self.script.write_text(witness)
            second_source = maildir(self.root / f"witness source {group}")
            for i, case in enumerate(cases):
                winner = authored.PolicyTests().expected_decision(case, rules, **books)
                name = f"{group}-{case['id']}:2,FS"
                headers = "".join(f"{key}: {val}\r\n" for key, val in case["headers"].items())
                blob = (headers + "Subject: Batch fixture\r\n\r\n").encode() + b"Body untouched\x00\xff\r\nFrom: blocked@example.org\n"
                subdir = "cur" if i % 2 else "new"
                for incoming in (self.source, second_source):
                    (incoming / subdir / name).write_bytes(blob)
                metadata = (len(blob), digest(blob), 0o600)
                expected[self.base / winner["sieve"] / "cur" / name] = metadata
                witnesses[witness_base / winner["id"] / "cur" / name] = metadata
                evidence.append(dict(case=case["id"], source=f"{subdir}/{name}", winner=winner["id"],
                                     destination=f"{winner['sieve']}/cur/{name}", bytes=len(blob),
                                     sha256=digest(blob), mode="0600"))
            self.invoke(script=product / "Filters.sieve")
            self.invoke(script=self.script, source=second_source, base=witness_base)
            for base, inventory in ((self.base, expected), (witness_base, witnesses)):
                actual = {Path(os.fsdecode(bytes.fromhex(row["path_hex"]))):
                          (row["bytes"], row["sha256"], row["mode"]) for row in body_files(base)}
                self.assertEqual(actual, inventory)
            for incoming in (self.source, second_source):
                self.assertEqual(body_files(incoming), [])
        self.assertEqual(len(expected), 52)
        if os.environ.get("JARO_TEST_LOG_DIR"):
            with tempfile.NamedTemporaryFile(mode="w", prefix="sieve-occurrences-", suffix=".json",
                                             dir=os.environ["JARO_TEST_LOG_DIR"], delete=False) as artifact:
                json.dump(evidence, artifact, indent=2)
                artifact.write("\n")

    def test_collisions_and_partial_retry(self):
        self.script.write_text('require "fileinto"; if header :is "Subject" "later" {fileinto "later";}'
                               'else {fileinto "done";}')
        dest = maildir(self.base / "done")
        foreign = dest / "cur/same:2,FS"
        foreign.write_bytes(b"Subject: foreign\n\nretain")
        bodies = [message("same headers", b) for b in ("first body", "second body")]
        for folder, data in zip(("new", "cur"), bodies):
            (self.source / folder / "same:2,FS").write_bytes(data)
        later = self.source / "new/later:2,T"
        later.write_bytes(message("later", "deferred"))
        broken = self.source / "cur/broken:2,D"
        broken.write_bytes(b"Subject missing colon\n\nbody")
        excluded = self.source / "tmp/ignored"
        excluded.write_bytes(message("new arrival", "do not ingest"))
        nested = maildir(self.source / "nested") / "cur/sibling"
        nested.write_bytes(message("nested", "do not ingest"))
        keep = {p: p.read_bytes() for p in (later, broken, excluded, nested, foreign)}
        failure = self.invoke(success=False)
        self.assertEqual(failure.returncode, 7)
        self.assertIn(b"Malformed message", failure.stderr)
        self.assertEqual({p: p.read_bytes() for p in keep}, keep)
        saved = list((dest / "cur").iterdir())
        self.assertEqual(sorted(p.read_bytes() for p in saved), sorted([*bodies, keep[foreign]]))
        self.assertTrue(all(p.name.endswith(":2,FS") for p in saved))
        later_dest = maildir(self.base / "later")
        broken.rename(self.root / "malformed retained")
        self.invoke()
        self.assertEqual((later_dest / "cur" / later.name).read_bytes(), keep[later])
        self.assertFalse(later.exists())
        before = tree(self.root)
        self.invoke()
        self.assertEqual(tree(self.root), before)

    def test_pinned_source_and_script_while_filing(self):
        self.source = maildir(self.base / "incoming")
        old = self.base / "original moved"
        self.script.write_text('require ["fileinto","mailbox"]; if header :is "Subject" "self" '
                               '{fileinto "incoming";} else {fileinto :create "Archive";}')
        one, two = message("send", "moved"), message("self", "stays in original")
        (self.source / "new/a").write_bytes(one)
        (self.source / "new/b").write_bytes(two)
        with self.held_save():
            self.source.rename(old)
            maildir(self.source)
            (self.source / "new/a").write_bytes(message("replacement", "never selected"))
            replacement = tree(self.source)
            (old / "new/c").write_bytes(message("later", "not in initial selection"))
            self.script.write_text("discard;")
            busy = self.invoke(source=old, success=False)
            self.assertIn(b"temporarily unavailable", busy.stderr)
        self.assertEqual(self.held_result.returncode, 0, self.held_result.stderr)
        self.assertEqual(tree(self.source), replacement)
        self.assertEqual({p.name: p.read_bytes() for p in (old / "new").iterdir()},
                         {"b": two, "c": message("later", "not in initial selection")})
        self.assertEqual((self.base / "Archive/cur/a").read_bytes(), one)

    def test_interruption_during_destination_save_and_retry(self):
        data = message("interrupt", "whole source")
        selected = self.source / "cur/message:2,FR"
        selected.write_bytes(data)
        with self.held_save() as (child, worker_pid):
            self.assertFalse(selected.exists())
            self.assertEqual((self.source / "tmp/.jaro-sieve-transaction/cur/0").read_bytes(), data)
            self.assertEqual([p.read_bytes() for p in (self.base / "INBOX/tmp").iterdir()], [data])
            self.assertEqual(list((self.base / "INBOX/cur").iterdir()), [])
            os.kill(worker_pid, signal.SIGTERM)
            child.wait(timeout=10)
        self.assertEqual(self.held_result.returncode, 128 + signal.SIGTERM, self.held_result.stderr)
        self.assertEqual(selected.read_bytes(), data)
        self.assertFalse(any(p.is_file() for p in self.base.rglob("*")))
        self.invoke()
        self.assertFalse(selected.exists())
        self.assertEqual((self.base / "INBOX/cur" / selected.name).read_bytes(), data)

    def test_concurrent_publish_with_long_flag_suffix(self):
        cur = maildir(self.base / "INBOX") / "cur"
        limit = os.pathconf(cur, "PC_NAME_MAX")
        name = "x:2," + "a" * (limit - 4)
        data, concurrent = message("long flags", "deliver"), message("competitor", "retain")
        (self.source / "new" / name).write_bytes(data)
        with self.held_save():
            (cur / name).write_bytes(concurrent)
        self.assertEqual(self.held_result.returncode, 0, self.held_result.stderr)
        self.assertEqual((cur / name).read_bytes(), concurrent)
        published, = [p for p in cur.iterdir() if p.name != name]
        self.assertTrue(published.name.endswith(name[1:]))
        self.assertEqual(published.read_bytes(), data)
        self.assertFalse(any(p.is_file() for p in self.source.rglob("*")))

    def test_interruption_after_publication_preserves_both_copies(self):
        chosen = self.source / "cur/chosen:2,F"
        data = message("interrupt", "committed destination")
        chosen.write_bytes(data)
        with self.held_save(after_publish=True) as (child, pid):
            published = self.base / "INBOX/cur" / chosen.name
            self.assertEqual(published.read_bytes(), data)
            os.kill(pid, signal.SIGTERM)
            child.wait(timeout=10)
        self.assertEqual(self.held_result.returncode, 128 + signal.SIGTERM, self.held_result.stderr)
        self.assertIn(b"retaining source", self.held_result.stderr)
        self.assertEqual(chosen.read_bytes(), data)
        self.assertEqual(published.read_bytes(), data)

    def test_source_expunge_error_preserves_both_copies(self):
        selected = self.source / "new/preserved:2,S"
        data = message("retain", "source expunge error")
        selected.write_bytes(data)
        selected.chmod(0o640)
        with self.held_save(fail_expunge=True):
            pass
        self.assertEqual(self.held_result.returncode, 7, self.held_result.stderr)
        self.assertEqual((selected.read_bytes(), stat.S_IMODE(selected.stat().st_mode)), (data, 0o640))
        filed = self.base / "INBOX/cur" / selected.name
        self.assertEqual((filed.read_bytes(), stat.S_IMODE(filed.stat().st_mode)), (data, 0o600))

    def test_replaced_delivery_directory_retains_source(self):
        mailbox = maildir(self.base / "INBOX")
        selected = self.source / "new/selected"
        data = message("filing", "retain the incoming copy")
        selected.write_bytes(data)
        with self.held_save():
            (mailbox / "cur").rename(mailbox / "old-cur")
            (mailbox / "cur").mkdir()
        self.assertEqual(self.held_result.returncode, 7, self.held_result.stderr)
        self.assertEqual(selected.read_bytes(), data)
        self.assertEqual(list((mailbox / "cur").iterdir()), [])
        self.assertEqual((mailbox / "old-cur" / selected.name).read_bytes(), data)

    def test_changed_destination_staging_retains_source(self):
        data = message("stage", "original")
        selected = self.source / "cur/a"
        selected.write_bytes(data)
        with self.held_save():
            staged, = (self.base / "INBOX/tmp").iterdir()
            staged.write_bytes(message("foreign", "concurrent replacement"))
        self.assertEqual(self.held_result.returncode, 7, self.held_result.stderr)
        self.assertIn(b"changed before source expunge", self.held_result.stderr)
        self.assertEqual(selected.read_bytes(), data)
        self.assertEqual((self.base / "INBOX/cur/a").read_bytes(), message("foreign", "concurrent replacement"))

    def test_concurrent_arrival_preserves_recovery_inventory(self):
        self.script.write_text('require ["fileinto","mailbox"]; if header :is "Subject" "send" '
                               '{fileinto :create "delivered";} else {keep;}')
        first, keep, arrival = (message(subject, subject) for subject in ("send", "keep", "arrival"))
        (self.source / "new/a").write_bytes(first)
        (self.source / "new/b").write_bytes(keep)
        with self.held_save():
            (self.source / "new/b").write_bytes(arrival)
        self.assertEqual(self.held_result.returncode, 7, self.held_result.stderr)
        self.assertIn(b"retained", self.held_result.stderr)
        self.assertEqual((self.source / "new/b").read_bytes(), arrival)
        self.assertEqual((self.base / "delivered/cur/a").read_bytes(), first)
        txn = self.source / "tmp/.jaro-sieve-transaction"
        rows = [json.loads(line) for line in (txn / "inventory.jsonl").read_text().splitlines()]
        self.assertEqual([(r[0], r[1], r[2]) for r in rows], [("0", "new", "a"), ("1", "new", "b")])
        self.assertEqual((txn / "cur/1").read_bytes(), keep)
        self.assertIn(b"unfinished Maildir transaction", self.invoke(success=False).stderr)

    def test_modified_claim_after_classification_retains_edited_source(self):
        original = message("selected", "message before delivery")
        edited = message("changed", "body changed while saving")
        selected = self.source / "new/edited:2,S"
        selected.write_bytes(original)
        with self.held_save():
            (self.source / "tmp/.jaro-sieve-transaction/cur/0").write_bytes(edited)
        self.assertNotEqual(self.held_result.returncode, 0)
        self.assertEqual(selected.read_bytes(), edited)
        self.assertEqual((self.base / "INBOX/cur/edited:2,S").read_bytes(), original)

    def test_published_replacement_retains_source_and_foreign_mail(self):
        original = message("selected", "original complete body")
        foreign = message("other", "replacement after publication")
        selected = self.source / "cur/keep:2,F"
        selected.write_bytes(original)
        destination = self.base / "INBOX/cur/keep:2,F"
        with self.held_save(after_publish=True):
            replacement = destination.parent.parent / "tmp/foreign"
            replacement.write_bytes(foreign)
            replacement.replace(destination)
        self.assertNotEqual(self.held_result.returncode, 0)
        self.assertEqual(destination.read_bytes(), foreign)
        self.assertEqual(selected.read_bytes(), original)

    def test_foreign_transaction_directory_blocks_before_claim(self):
        selected = self.source / "new/message"
        selected.write_bytes(message("selected", "entire body"))
        private = self.source / "tmp/.jaro-sieve-transaction"
        private.mkdir()
        (private / "sentinel").write_bytes(b"foreign data")
        before = tree(self.root)
        result = self.invoke(success=False)
        self.assertIn(b'unfinished Maildir transaction', result.stderr)
        self.assertEqual(tree(self.root), before)

    def test_hard_kill_leaves_durable_source_inventory(self):
        selected = self.source / "new/a"
        data = message("kill", "recoverable")
        selected.write_bytes(data)
        with self.held_save() as (child, worker_pid):
            child.kill()
            child.wait(timeout=10)
        self.assertEqual(self.held_result.returncode, -signal.SIGKILL)
        txn = self.source / "tmp/.jaro-sieve-transaction"
        self.assertEqual((txn / "cur/0").read_bytes(), data)
        self.assertEqual(json.loads((txn / "inventory.jsonl").read_text())[:3], ["0", "new", "a"])
        self.assertIn(b"unfinished Maildir transaction", self.invoke(success=False).stderr)


if __name__ == "__main__":
    missing = prerequisite()
    if missing:
        print("SKIP: " + missing)
        raise SystemExit(77)
    unittest.main(verbosity=2)
