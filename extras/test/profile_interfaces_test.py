"""Local installed CLI contracts for the native stores, policy and Maildirs."""

from concurrent.futures import ThreadPoolExecutor
import base64
import json
import importlib.util
from unittest.mock import patch
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PREFIX, WORK = map(Path, sys.argv[1:3])
sys.argv[1:] = []
MSG = b"From: author@example.test\nTo: second@example.test\nMessage-ID: <fixture@example.test>\nSubject: fixture\n\nbody\n"


class Contracts(unittest.TestCase):
    def setUp(self):
        self.root = WORK / self._testMethodName
        self.root.mkdir(mode=0o700)
        self.home = self.root / "home"
        self.home.mkdir(mode=0o700)
        self.mail = self.root / "mail"
        self.mail.mkdir(mode=0o700)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.env = dict(
            os.environ,
            HOME=str(self.home),
            JAROMAILDIR=str(self.mail),
            JAROACCOUNT=str(self.mail / "Accounts"),
            PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
        )
        self.cli("init")
        (self.mail / "Accounts/default.txt").write_text(
            "email owner@example.test\nhost local.example.test\n"
        )

    def run_cli(self, *args, data=b""):
        return subprocess.run(
            [PREFIX / "bin/jaro", *args],
            input=data,
            cwd=self.root,
            env=self.env,
            capture_output=True,
            timeout=40,
        )

    def cli(self, *args, data=b"", code=0):
        res = self.run_cli(*args, data=data)
        self.assertEqual(res.returncode, code, (args, res.stderr[-300:]))
        return res.stdout

    def obj(self, *args, data=b""):
        return json.loads(self.cli(*args, data=data))

    def md(self, folder):
        for part in ("new", "tmp", "cur"):
            (self.mail / folder / part).mkdir(parents=True, exist_ok=True)
        return self.mail / folder

    def test_account_edit_race_and_protected_credential(self):
        data = json.dumps(
            {
                "email": "second@example.test",
                "imap": "one.example.test",
                "smtp": "two.example.test",
                "folders": ["Folder A", "Folder B"],
            }
        ).encode()
        a = self.obj("account", "put", "second", "--expect", "absent", data=data)
        self.assertEqual(a["fields"]["folders"], ["Folder A", "Folder B"])
        self.cli("account", "put", "second", "--expect", "absent", data=data, code=6)
        self.cli(
            "account",
            "put",
            "second",
            "--expect",
            a["revision"],
            data=b'{"imap_port":"65536"}',
            code=4,
        )
        self.assertEqual(self.obj("account", "get", "second"), a)

        def change(n):
            return self.run_cli(
                "account",
                "put",
                "second",
                "--expect",
                a["revision"],
                data=json.dumps({"name": n}).encode(),
            ).returncode

        with ThreadPoolExecutor(2) as pool:
            self.assertEqual(sorted(pool.map(change, ["First", "Last"])), [0, 6])
        f = self.mail / "Accounts/second"
        with f.open("a") as out:
            out.write("# preserve comment\npassword hidden-native-fixture\n")
        a = self.obj("account", "get", "second")
        self.assertNotIn("hidden-native-fixture", json.dumps(a))
        changed = self.obj(
            "account",
            "put",
            "second",
            "--expect",
            a["revision"],
            data=b'{"name":"Owner"}',
        )
        self.assertIn(
            "# preserve comment\npassword hidden-native-fixture\n", f.read_text()
        )
        mock = self.bin / "secret-tool"
        mock.write_text(
            "#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\nassert sys.stdin.buffer.read()==b'opaque-fixture'\nPath(os.environ['HOME'],'args').write_text(repr(sys.argv))\n"
        )
        mock.chmod(0o700)
        self.env["JARO_KEYRING"] = "secret-tool"
        self.assertTrue(
            self.obj(
                "account",
                "credential",
                "second",
                "--service",
                "smtp",
                "--expect",
                changed["revision"],
                data=b"opaque-fixture",
            )["stored"]
        )
        self.assertNotIn("opaque-fixture", (self.home / "args").read_text())
        self.assertIn("two.example.test", (self.home / "args").read_text())
        mock.write_text("#!/bin/sh\nexit 1\n")
        self.cli(
            "account",
            "credential",
            "second",
            "--service",
            "smtp",
            "--expect",
            changed["revision"],
            data=b"opaque-fixture",
            code=7,
        )

    def test_contact_and_group_revisions(self):
        def records(*args, **kwargs):
            return self.cli("contacts", *args, **kwargs).split(b"\0")[:-1]

        gen = records("books")[1]
        newgen = records("create-book", "people", gen)[1]
        payload = b"Alex\0a\0note\0first@example.test\0second@example.test\0"
        _, newgen, ident = records("put", "people", "0", newgen, data=payload)
        row = records("list", "people")
        self.assertEqual(row[2:7], [ident, b"1", b"Alex", b"a", b"note"])
        self.assertIn(b"second@example.test", self.cli("-l", "people", "export"))

        def edit(name):
            return self.run_cli(
                "contacts",
                "put",
                "people",
                ident,
                b"1",
                data=payload.replace(b"Alex", name),
            ).returncode

        with ThreadPoolExecutor(2) as pool:
            self.assertEqual(sorted(pool.map(edit, [b"One", b"Two"])), [0, 6])
        self.cli("contacts", "delete", "people", ident, "1", code=6)
        self.cli("contacts", "put", "people", ident, "2", data=b"unterminated", code=4)
        result = records("delete", "people", ident, "2")
        gen = records("rename-book", "people", "team", result[1])[1]
        self.cli("contacts", "rename-book", "team", "block", gen, code=4)
        records("delete-book", "team", gen)
        self.assertNotIn(b"team", records("books"))
        self.assertEqual(self.obj("group", "list")["ids"], [])
        grp = self.obj(
            "group",
            "put",
            "team",
            "--expect",
            "absent",
            data=b'{"mode":"bcc","members":["Alex <a@example.test>"]}',
        )
        (self.mail / "Groups/team").write_text(
            "#mode cc\n# owner comment\nAlex <a@example.test>\n"
        )
        self.cli(
            "group",
            "put",
            "team",
            "--expect",
            grp["revision"],
            data=b'{"mode":"individual"}',
            code=6,
        )
        grp = self.obj("group", "get", "team")
        self.cli(
            "group",
            "put",
            "team",
            "--expect",
            grp["revision"],
            data=b'{"mode":"unknown"}',
            code=4,
        )
        self.obj(
            "group",
            "put",
            "team",
            "--expect",
            grp["revision"],
            data=b'{"members":["B <b@example.test>"]}',
        )
        self.assertEqual(
            (self.mail / "Groups/team").read_text(),
            "#mode cc\n# owner comment\nB <b@example.test>\n",
        )

    def test_policy_candidate_publication_and_stale_inputs(self):
        rel = "Shared/Nested/new/example"
        msg = self.md("Shared/Nested") / "new/example"
        msg.write_bytes(MSG)
        config = self.mail / "Filters.txt"
        candidate = b'# retained comment\nfrom-address author@example.test move "Reviewed Folder"\n'
        original = config.read_bytes()

        def preview():
            p = self.obj("policy", "preview", "Filters.txt", rel, data=candidate)
            self.assertEqual(p["decision"]["folder"], "Reviewed Folder")
            return p["token"]

        def publish(token, code):
            return self.cli(
                "policy",
                "publish",
                "Filters.txt",
                rel,
                "--expect",
                token,
                data=candidate,
                code=code,
            )

        token = preview()
        self.assertEqual(config.read_bytes(), original)
        noop = candidate.replace(b"Reviewed Folder", b"Shared/Nested")
        nochange = self.obj("policy", "preview", "Filters.txt", rel, data=noop)
        self.assertEqual(nochange["decision"]["status"], "noop")
        self.cli(
            "policy",
            "publish",
            "Filters.txt",
            rel,
            "--expect",
            token,
            data=noop,
            code=6,
        )
        (self.mail / "Aliases.txt").write_text("another@example.test\n")
        publish(token, 6)
        token = preview()
        msg.write_bytes(MSG + b"another body\n")
        publish(token, 6)
        token = preview()
        with (self.mail / "Accounts/default.txt").open("a") as f:
            f.write("# edit\n")
        publish(token, 6)
        token = preview()
        self.cli("-l", "block", "import", data=b"Blocked <blocked@example.test>\n")
        publish(token, 6)
        token = preview()
        self.assertTrue(json.loads(publish(token, 0))["generation"])
        self.assertEqual(config.read_bytes(), candidate)
        gen = os.readlink(self.mail / "cache/filter-engine/current")
        self.assertNotEqual(
            self.run_cli(
                "policy", "preview", "Filters.txt", rel, data=b"invalid syntax\n"
            ).returncode,
            0,
        )
        self.assertEqual(os.readlink(self.mail / "cache/filter-engine/current"), gen)
        msg.rename(msg.with_name("original"))
        msg.symlink_to(msg.with_name("original"))
        self.assertNotEqual(
            self.run_cli(
                "policy", "preview", "Filters.txt", rel, data=candidate
            ).returncode,
            0,
        )

    def test_recursive_maildir_moves_merge_and_selection(self):
        src = self.md("Shared/Nested")
        dest = self.md("Archive")
        for name in ("a", "b"):
            (src / "cur" / (name + ":2,F")).write_bytes(MSG + name.encode())
        (self.mail / "Linked").symlink_to(src, target_is_directory=True)
        held = ".jaro-recovered-outbox-0/outbox"
        (self.md(held) / "new/held").write_bytes(MSG)
        self.cli("maildir", "messages", held, code=3)
        self.cli("maildir", "merge", held, "outbox", "--apply", code=3)
        folders = [json.loads(s) for s in self.cli("maildir", "folders").splitlines()]
        self.assertIn("Shared/Nested", folders)
        self.assertNotIn("Linked", folders)
        self.assertNotIn(held, folders)
        self.cli("maildir", "messages", "../outside", code=3)
        items = self.obj("maildir", "merge", "Shared/Nested", "Archive")["items"]
        self.assertEqual(len(items), 2)
        (src / items[0]["part"] / items[0]["name"]).write_bytes(b"replacement")
        result = json.loads(
            self.cli(
                "maildir", "apply", "Archive", data=json.dumps(items).encode(), code=7
            )
        )
        self.assertEqual([r["status"] for r in result["results"]], ["error", "moved"])
        moved = list((dest / "cur").iterdir())
        self.assertEqual(len(moved), 1)
        self.assertTrue(moved[0].name.endswith(":2,F"))
        self.md("Shared/Nested/Child")
        self.cli(
            "maildir",
            "merge",
            "Shared/Nested",
            "Archive",
            "--apply",
            "--remove-empty",
            code=7,
        )
        self.assertTrue((src / "Child/new").is_dir() and (src / "cur").is_dir())
        self.md("Empty")
        self.obj("merge", "Empty", "Archive")
        self.assertFalse((self.mail / "Empty").exists())
        one = self.md("One")
        two = self.md("Two")
        (one / "new/a").write_bytes(MSG)
        (two / "new/b").write_bytes(MSG + b"conflicting bytes")
        duplicates = self.obj("maildir", "legacy-preview", "One", "Two")["items"]
        self.assertEqual([i["folder"] for i in duplicates], ["Two"])
        self.obj(
            "maildir",
            "legacy-remove",
            "--confirm-message-id",
            data=json.dumps({"items": duplicates, "folders": ["One", "Two"]}).encode(),
        )
        self.assertTrue((one / "new/a").exists())
        self.cli("maildir", "legacy-preview", "lastlog", code=3)
        runtime = PREFIX / "share/jaromail/maildir/operations.py"
        no_formail = subprocess.run(
            [sys.executable, "-I", runtime, self.mail, "legacy-preview", "One", "Two"],
            env=dict(self.env, PATH=str(self.bin)),
            capture_output=True,
        )
        self.assertEqual(no_formail.returncode, 3)
        self.cli("index")
        self.assertTrue(
            self.obj("maildir", "backup-preview", "Archive", "subject:fixture")["items"]
        )
        (one / "new/unindexed").write_bytes(MSG + b"unique-unindexed-text")
        found = self.obj(
            "maildir", "backup-preview", "Archive", "unique-unindexed-text"
        )["items"]
        self.assertEqual([i["name"] for i in found], ["unindexed"])
        self.obj("maildir", "apply", "Archive", data=json.dumps(found).encode())
        self.cli("fresh-search")
        self.assertFalse((one / "new/unindexed").exists())
        indexed = self.cli("search", "id:fixture@example.test").decode().splitlines()
        self.assertTrue(
            any(
                Path(path).parent.parent == dest
                and b"unique-unindexed-text" in Path(path).read_bytes()
                for path in indexed
            )
        )

    def test_received_date_and_keywords_survive_flag_edits_and_refiling(self):
        inbox = self.md("Inbox")
        archive = self.md("Archive")
        original = inbox / "cur/fixture:2,Fa,opaque=F"
        original.write_bytes(MSG)
        historical = 1104537600123456789
        os.utime(original, ns=(historical, historical))

        def selection():
            return json.loads(self.cli("maildir", "messages", "Inbox"))

        def apply(standard, *, code=0):
            return self.cli(
                "maildir",
                "apply",
                "Inbox",
                "--flags",
                standard,
                data=json.dumps([selection()]).encode(),
                code=code,
            )

        def preserved(path):
            self.assertEqual(path.stat().st_mtime_ns, historical)
            self.assertEqual(path.read_bytes(), MSG)

        apply("SF")
        seen = inbox / "cur/fixture:2,FSa,opaque=F"
        preserved(seen)
        apply("F")
        preserved(original)
        apply("")
        unread = inbox / "cur/fixture:2,a,opaque=F"
        preserved(unread)
        # A colliding flag name must retain its own bytes and timestamp.
        collision = inbox / "cur/fixture:2,Sa,opaque=F"
        collision.write_bytes(b"other")
        other_time = collision.stat().st_mtime_ns
        item = next(
            json.loads(row)
            for row in self.cli("maildir", "messages", "Inbox").splitlines()
            if json.loads(row)["name"] == unread.name
        )
        self.cli(
            "maildir",
            "apply",
            "Inbox",
            "--flags",
            "S",
            data=json.dumps([item]).encode(),
            code=7,
        )
        preserved(unread)
        self.assertEqual(
            (collision.read_bytes(), collision.stat().st_mtime_ns),
            (b"other", other_time),
        )
        collision.unlink()
        bootstrap = PREFIX / "share/jaromail/zlibs/bootstrap"

        def refile(mode):
            subprocess.run(
                [
                    "zsh",
                    "-fc",
                    'source "$1" source && maildir_refile "$2" "$3" "$4"',
                    "fixture",
                    bootstrap,
                    unread,
                    archive,
                    mode,
                ],
                env=dict(self.env, JAROWORKDIR=str(bootstrap.parent.parent)),
                cwd=self.root,
                capture_output=True,
                check=True,
                timeout=30,
            )

        refile("copy")
        preserved(unread)
        refile("move")
        self.assertFalse(unread.exists())
        copies = list((archive / "cur").iterdir())
        self.assertEqual(len(copies), 2)
        for item in copies:
            self.assertTrue(item.name.endswith(":2,a,opaque=F"))
            preserved(item)

        cross = self.env.get("JARO_FILTER_CROSS_FS_DIR")
        if cross:
            # Supply real Maildir roots on two devices to the installed transfer
            # routine. Only the folder lookup is adapted, avoiding bind mounts.
            with tempfile.TemporaryDirectory(dir=cross) as remote:
                for part in ("cur", "new", "tmp"):
                    (Path(remote) / "Far" / part).mkdir(parents=True)
                self.assertNotEqual(archive.stat().st_dev, Path(remote).stat().st_dev)
                spec = importlib.util.spec_from_file_location(
                    "maildir_cross", PREFIX / "share/jaromail/maildir/operations.py"
                )
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                root = os.open(self.mail, os.O_DIRECTORY)
                far = os.open(remote, os.O_DIRECTORY)
                native_folder = module.folder

                def routed(fd, box):
                    return native_folder(far if box == "Far" else fd, box)

                try:
                    item = next(module.messages(root, "Archive"))
                    with patch.object(module, "folder", routed):
                        self.assertEqual(module.transfer(root, item, "Far"), "moved")
                finally:
                    os.close(far)
                    os.close(root)
                (transferred,) = (Path(remote) / "Far/cur").iterdir()
                self.assertTrue(transferred.name.endswith(":2,a,opaque=F"))
                preserved(transferred)

    def test_move_preserves_external_writer_and_destination_collision(self):
        src = self.md("Race")
        dest = self.md("Destination")
        item = src / "new/source"
        item.write_bytes(MSG)
        selected = json.loads(self.cli("maildir", "messages", "Race"))
        final = dest / "cur" / ("jaro-refile-" + selected["revision"])
        final.write_bytes(b"existing")
        self.cli(
            "maildir",
            "apply",
            "Destination",
            data=json.dumps([selected]).encode(),
            code=7,
        )
        self.assertEqual(final.read_bytes(), b"existing")
        self.assertEqual(item.read_bytes(), MSG)
        final.unlink()
        flags = self.obj(
            "maildir",
            "apply",
            "Race",
            "--flags",
            "FS",
            data=json.dumps([selected]).encode(),
        )
        self.assertEqual(flags["results"][0]["status"], "moved")
        selected = json.loads(self.cli("maildir", "messages", "Race"))
        self.assertEqual((selected["part"], selected["name"]), ("cur", "source:2,FS"))
        item = src / "cur" / selected["name"]
        # Interpose an independent writer after the durable destination link,
        # just before source disposition. The installed routine performs both.
        spec = importlib.util.spec_from_file_location(
            "maildir_contract", PREFIX / "share/jaromail/maildir/operations.py"
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        native = module.drop_original
        replacement = src / "tmp" / "replacement"
        replacement.write_bytes(b"external writer")

        def replace_then_dispose(*args):
            subprocess.run(["mv", "--", replacement, item], check=True)
            return native(*args)

        root = os.open(self.mail, os.O_DIRECTORY)
        try:
            with patch.object(
                module, "drop_original", replace_then_dispose
            ), self.assertRaises(ValueError):
                module.transfer(root, selected, "Destination")
        finally:
            os.close(root)
        self.assertEqual(item.read_bytes(), b"external writer")
        self.assertEqual([f.read_bytes() for f in (dest / "cur").iterdir()], [MSG])

    def test_replay_and_statistics(self):
        self.cli("replay-record", "last", code=3)
        cache = self.mail / "cache"
        data = "Béa Smith <a@example.test>\n".encode()
        (cache / "replay.extract").write_bytes(data)
        (cache / "replay.last").symlink_to(cache / "replay.extract")
        result = self.obj("replay-record", "last")
        self.assertEqual(
            (result["producer"], base64.b64decode(result["data"])), ("extract", data)
        )
        (cache / "replay.extract").unlink()
        (cache / "replay.extract").symlink_to(self.mail / "Filters.txt")
        self.cli("replay-record", "last", code=3)
        for dim, label in [
            ("domain", "example.test"),
            ("email", "a@example.test"),
            ("name", "Béa Smith "),
            ("folder", "Work Stuff"),
        ]:
            line = data.strip() if dim != "folder" else b"/Work Stuff/new/item"
            fields = self.cli("stats0", dim, data=(line + b"\0") * 2).split(b"\0")
            self.assertEqual(fields, [b"JST1", label.encode(), b"2", b""])
            self.assertIn(
                label.encode(), self.cli("stat", dim, data=(line + b"\n") * 2)
            )
        self.cli("stats0", "name", data=b"unfinished", code=2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
