"""Focused corpus checks using the installed CLI and the exhaustive small oracle."""
import argparse
import contextlib
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import dedup_corpus as corpus
import dedup_full_hash as baseline

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "test"))
from dedup_classify_test import exhaustive_survivors


class Contract(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(dir=args.work)
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name).resolve()
        self.scratch = self.base / "scratch"
        self.scratch.mkdir()
        self.home = self.base / "home"
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), JAROMAILDIR=str(self.base / "unused-mail-root"))
        self.env.pop("JAROWORKDIR", None)
        self.source = self.base / "source"

    def corpus(self, *argv):
        return subprocess.run([sys.executable, corpus.__file__, *argv], env=self.env, check=True,
                              capture_output=True).stdout

    def generate(self, *argv):
        self.corpus("generate", self.source, "--seed", "73", *argv)

    def oracle(self, root, reverse):
        folders = [root / "mail" / name for name in corpus.FOLDERS]
        if reverse:
            folders.reverse()

        def path(key):
            folder, sub, name = key
            return folders[folder] / corpus.SUBDIRS[sub] / os.fsdecode(name)

        all_rows = exhaustive_survivors(folders)
        pairs = sorted(b"keep " + corpus.quote(path(win)) + b" remove " + corpus.quote(path(row))
                       for row, win in all_rows if row != win)
        return folders, pairs, [path(row) for row, win in all_rows if row == win]

    def ranked(self, reverse):
        self.generate("--count", "147", "--duplicate-percent", "75")
        choices = ["--reverse"] if reverse else []
        for implementation in ("installed", "full-hash"):
            root = self.base / implementation
            self.corpus("clone", self.source, root)
            folders, expected, winners = self.oracle(root, reverse)
            self.assertEqual(len(expected), 110)
            self.assertEqual(sorted(self.corpus("expected-preview", root, *choices).splitlines()), expected)
            keep = [(p, p.stat().st_ino, p.stat().st_mode, p.read_bytes()) for p in winners]
            for mode in ("preview", "apply", "retry"):
                if implementation == "installed":
                    cmd = [args.cli, *(["-n"] if mode == "preview" else []), "dedup",
                           "--scratch-dir", self.scratch, "--", *folders]
                else:
                    cmd = [sys.executable, baseline.__file__, root, "--scratch-dir", self.scratch,
                           *choices, *([] if mode == "preview" else ["--apply"])]
                result = subprocess.run(cmd, check=True, capture_output=True, env=self.env)
                if mode == "preview":
                    self.assertEqual(sorted(result.stdout.splitlines()), expected)
                    self.corpus("verify", root, *choices)
                    if implementation == "full-hash":
                        meta = corpus.read_meta(root)
                        self.assertIn(f'hash-bytes={meta["logical_bytes"]} '.encode(), result.stderr)
                else:
                    self.assertEqual(result.stdout, b"")
                    self.assertIn(f"removed={110 if mode == 'apply' else 0} ".encode(), result.stderr)
                    self.corpus("verify", root, "--survivors", *choices)
                for p, ino, bits, body in keep:
                    self.assertEqual((p.stat().st_ino, p.stat().st_mode, p.read_bytes()), (ino, bits, body))
                self.assertEqual(list(self.scratch.iterdir()), [])

    def test_forward_priority(self):
        self.ranked(False)

    def test_reversed_priority(self):
        self.ranked(True)

    def test_seed_and_winners(self):
        options = ("--count", "125", "--duplicate-percent", "80")
        self.generate(*options)
        other = self.base / "replica"
        self.corpus("generate", other, "--seed", "73", *options)
        for name in ("paths.tsv", "groups.tsv"):
            self.assertEqual((self.source / name).read_bytes(), (other / name).read_bytes())
        self.corpus("verify", other)
        _, _, forward = self.oracle(self.source, False)
        _, _, reverse = self.oracle(self.source, True)
        self.assertNotEqual(set(forward), set(reverse))

    def test_equal_ends_and_hash_collisions(self):
        self.generate("--count", "12", "--profile", "attachment-repeated", "--sizes-mib", "1",
                      "--duplicate-percent", "50")
        clone = self.base / "collision"
        self.corpus("clone", self.source, clone)
        files = list(corpus.entries(clone))
        bodies = [p.read_bytes() for p in files]
        self.assertEqual(len(set(bodies)), 6)
        self.assertEqual(len({(b[:16384], b[-16384:]) for b in bodies}), 1)
        self.assertTrue(all(p.stat().st_blocks * 512 >= p.stat().st_size for p in files))
        _, _, winners = self.oracle(clone, False)

        class Collision:
            def update(self, _):
                pass

            def digest(self):
                return bytes(32)

        with patch.object(hashlib, "sha256", Collision), contextlib.redirect_stderr(io.StringIO()):
            baseline.run(clone, self.scratch, False, True)
        self.assertEqual(set(corpus.entries(clone)), set(winners))
        self.corpus("verify", clone, "--survivors")

    def test_unique_and_source_preservation(self):
        self.generate("--count", "4", "--profile", "attachment-unique", "--sizes-mib", "1,2")
        self.assertEqual(len({p.stat().st_size for p in corpus.entries(self.source)}), 4)
        with self.assertRaisesRegex(ValueError, "disposable clone"):
            baseline.run(self.source, self.scratch, False, True)
        self.corpus("cache", self.source, "warm")
        self.corpus("cache", self.source, "cold-hint")
        self.corpus("verify", self.source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=lambda s: Path(s).resolve(strict=True), required=True)
    parser.add_argument("--work", type=Path, required=True)
    args, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
