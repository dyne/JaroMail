"""Isolated message snapshot contract; Python is a test dependency only."""
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
BIN_DIR = Path(os.environ.get('JARO_SNAPSHOT_TEST_BIN_DIR', REPO / 'build/gnu'))
HELPER = BIN_DIR / 'jaro-snapshot'
FAULT_HELPER = BIN_DIR / 'jaro-snapshot-test'


def tree(root):
    result = {}
    for path in root.rglob('*'):
        relative = os.fsencode(path.relative_to(root))
        if path.is_symlink():
            result[relative] = ('symlink', os.readlink(path))
        elif path.is_file():
            result[relative] = ('file', path.stat().st_mode & 0o777, path.read_bytes())
        elif path.is_dir():
            result[relative] = ('dir', path.stat().st_mode & 0o777)
    return result


class Snapshots(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='jaro-snapshot-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.mail = self.root / 'mail'
        self.mail.mkdir()
        self.home = self.root / 'empty home'
        self.home.mkdir()
        self.dest = self.root / 'snapshot'
        self.restored = self.root / 'restored mail'
        self.env = dict(os.environ, HOME=str(self.home), JAROMAILDIR=str(self.mail))
        self.env.pop('JARO_SNAPSHOT_FAULT', None)
        self.env.pop('JARO_SNAPSHOT_PAUSE', None)
        self.paths = [
            'incoming/new/white space [glob] λ\ntrailing\n',
            'incoming/cur/flagged:2,FRS',
            'archive/nested/new/identical one',
            'archive/nested/cur/identical two:2,S',
            '.hidden-folder/new/hidden message',
            'outbox/cur/queued:2,DF',
            'incoming/new/tmp',
            'incoming/cur/Accounts',
        ]
        self.message_bytes = b'From: fixture@example.org\r\nSubject: bytes\r\n\r\noriginal\x00\xff\r\n'
        for relative in self.paths:
            path = self.mail / relative
            for folder in ('new', 'cur', 'tmp'):
                (path.parent.parent / folder).mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.message_bytes)
        for relative in ('incoming/tmp/unfinished', 'Accounts/account', '.outbox-state/journal',
                         'outbox/.recovery-claims/claim', '.notmuch/index', '.password-store/password',
                         'Filters.txt', 'executable-config', 'keyrings/store'):
            path = self.mail / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'excluded private configuration')
        self.before = tree(self.mail)

    def run_helper(self, *args, succeeds=True, fault=None, production=False):
        env = dict(self.env)
        if fault:
            env['JARO_SNAPSHOT_FAULT'] = fault
        binary = HELPER if production else FAULT_HELPER
        result = subprocess.run([str(binary), *map(str, args)], env=env, capture_output=True, timeout=20)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def capture(self):
        self.run_helper('create', self.mail, self.dest)

    def intact(self):
        self.assertEqual(tree(self.mail), self.before, 'source names, bytes and modes must be preserved')

    def incomplete(self):
        self.assertFalse((self.dest / 'complete').exists())
        self.assertFalse((self.dest / 'manifest').exists())
        self.assertEqual(list(self.dest.glob('*.pending')), [])
        if self.dest.exists():
            self.run_helper('verify', self.dest, succeeds=False)

    def manifest(self):
        data = (self.dest / 'manifest').read_bytes()
        fields = data.split(b'\0')
        self.assertEqual(fields[0], b'JMS1')
        rows = []
        index = 1
        while fields[index]:
            rows.append(fields[index:index+4])
            index += 4
        self.assertEqual(fields[index:], [b'', str(len(rows)).encode(), b''])
        return rows

    def replace_manifest(self, data):
        (self.dest / 'manifest').write_bytes(data)
        (self.dest / 'complete').write_text(hashlib.sha256(data).hexdigest() + '\n')

    def test_create_independent_verification_and_exact_filenames_flags(self):
        self.capture()
        self.run_helper('verify', self.dest, production=True)
        rows = self.manifest()
        self.assertEqual([row[0] for row in rows], sorted(os.fsencode(p) for p in self.paths))
        for i, (relative, flags, size, digest) in enumerate(rows):
            original = (self.mail / os.fsdecode(relative)).read_bytes()
            self.assertEqual((self.dest / 'messages' / str(i)).read_bytes(), original)
            self.assertEqual(int(size), len(original))
            self.assertEqual(digest.decode(), hashlib.sha256(original).hexdigest())
            basename = relative.rsplit(b'/', 1)[1]
            self.assertEqual(flags, basename.split(b':2,', 1)[1] if b':2,' in basename else b'')
        self.assertEqual(len(list((self.dest / 'messages').iterdir())), len(self.paths))
        self.assertEqual((self.dest / 'complete').read_text().strip(),
                         hashlib.sha256((self.dest / 'manifest').read_bytes()).hexdigest())
        self.intact()

    def test_private_permissions(self):
        self.capture()
        for path in [self.dest, *self.dest.rglob('*')]:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700 if path.is_dir() else 0o600)

    def test_empty_capture_and_root_maildir(self):
        empty = self.root / 'empty'
        empty.mkdir()
        self.run_helper('create', empty, self.dest)
        self.run_helper('verify', self.dest)
        self.assertEqual(self.manifest(), [])
        for folder in ('new', 'cur', 'tmp'):
            (empty / folder).mkdir()
        (empty / 'new' / 'root mail').write_bytes(b'root')
        other = self.root / 'root-snapshot'
        self.run_helper('create', empty, other)
        self.run_helper('verify', other)

    def test_existing_destination_preserved(self):
        self.dest.mkdir()
        (self.dest / 'keep').write_bytes(b'existing')
        before = tree(self.dest)
        self.run_helper('create', self.mail, self.dest, succeeds=False)
        self.assertEqual(tree(self.dest), before)
        self.intact()

    def test_nested_destination_and_parent_alias_refused(self):
        for destination in (self.mail / 'capture', self.mail / 'incoming' / '..' / 'capture'):
            self.run_helper('create', self.mail, destination, succeeds=False)
            self.assertFalse((self.mail / 'capture').exists())
        self.intact()

    def test_symlink_source_root_and_ancestor_refused(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.mail, target_is_directory=True)
        for source in (alias, alias / 'archive'):
            self.run_helper('create', source, self.dest, succeeds=False)
            self.assertFalse(self.dest.exists())
        self.intact()

    def test_symlink_destination_parent_and_existing_link_refused(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        self.run_helper('create', self.mail, alias / 'snapshot', succeeds=False)
        self.assertFalse(self.dest.exists())
        self.dest.symlink_to(self.root / 'absent')
        self.run_helper('create', self.mail, self.dest, succeeds=False)
        self.assertTrue(self.dest.is_symlink())
        self.assertFalse((self.root / 'absent').exists())
        self.intact()

    def test_symlink_message_and_folder_refused(self):
        for kind in ('message', 'folder', 'new'):
            with self.subTest(kind=kind):
                link = self.mail / ('incoming/new/link' if kind == 'message' else 'linked-folder')
                if kind == 'new':
                    link = self.mail / 'other' / 'new'
                    link.parent.mkdir()
                link.symlink_to(self.mail / 'incoming', target_is_directory=True)
                self.run_helper('create', self.mail, self.dest, succeeds=False)
                self.assertFalse(self.dest.exists())
                link.unlink()
                if kind == 'new':
                    link.parent.rmdir()
        self.intact()

    def test_fifo_message_refused_without_hanging(self):
        fifo = self.mail / 'incoming/new/fifo'
        os.mkfifo(fifo)
        self.run_helper('create', self.mail, self.dest, succeeds=False)
        fifo.unlink()
        self.intact()

    def test_missing_and_unreadable_source(self):
        self.run_helper('create', self.root / 'absent', self.dest, succeeds=False)
        message = self.mail / self.paths[0]
        message.chmod(0)
        try:
            self.run_helper('create', self.mail, self.dest, succeeds=False)
            self.incomplete()
        finally:
            message.chmod(self.before[os.fsencode(self.paths[0])][1])
        self.intact()

    def test_failed_destination_write_preserves_source(self):
        parent = self.root / 'unwritable'
        parent.mkdir(mode=0o500)
        try:
            self.run_helper('create', self.mail, parent / 'snapshot', succeeds=False)
            self.assertEqual(list(parent.iterdir()), [])
        finally:
            parent.chmod(0o700)
        self.intact()

    def test_failed_reads_writes_sync_and_publication(self):
        # At least two write chunks: the ENOSPC hook must leave fewer bytes
        # than this message, rather than merely failing after a complete copy.
        first = sorted(self.paths, key=os.fsencode)[0]
        (self.mail / first).write_bytes(self.message_bytes * 3000)
        self.before = tree(self.mail)
        for phase in ('source-read', 'payload-write', 'payload-sync', 'manifest-sync',
                      'manifest-publish', 'complete-publish', 'complete-sync'):
            with self.subTest(phase=phase):
                if self.dest.exists():
                    shutil.rmtree(self.dest)
                result = self.run_helper('create', self.mail, self.dest, succeeds=False, fault=phase)
                self.assertIn(('SNAPSHOT-TEST fault reached: ' + phase).encode(), result.stderr)
                if phase == 'payload-write':
                    size = (self.dest / 'messages/0').stat().st_size
                    self.assertGreater(size, 0, 'ENOSPC must follow an actual write')
                    self.assertLess(size, (self.mail / first).stat().st_size, 'ENOSPC must leave a partial payload')
                self.incomplete()
                self.intact()

    def paused(self, phase, restore=False):
        env = dict(self.env, JARO_SNAPSHOT_PAUSE=phase)
        args = ['restore', str(self.dest), str(self.restored)] if restore else ['create', str(self.mail), str(self.dest)]
        child = subprocess.Popen([str(FAULT_HELPER), *args],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if child.poll() is None:
                child.terminate()
            child.communicate(timeout=10)
        self.addCleanup(cleanup)
        ready, _, _ = select.select([child.stderr], [], [], 10)
        self.assertTrue(ready, 'owned helper did not reach injected pause')
        self.assertEqual(child.stderr.readline(), ('SNAPSHOT-TEST pause reached: ' + phase + '\n').encode())
        return child

    def test_interruption_cleans_staging_and_owned_process(self):
        for phase in ('inventory-captured', 'copied', 'complete-publish'):
            with self.subTest(phase=phase):
                if self.dest.exists():
                    shutil.rmtree(self.dest)
                child = self.paused(phase)
                child.send_signal(signal.SIGTERM)
                child.communicate(timeout=10)
                self.assertEqual(child.returncode, 128 + signal.SIGTERM)
                self.assertRaises(ProcessLookupError, os.kill, child.pid, 0)
                self.incomplete()
                self.intact()

    def test_corrupt_copy_before_publication_refused(self):
        child = self.paused('copied')
        (self.dest / 'messages/0').write_bytes(b'bad storage changed copied bytes')
        child.send_signal(signal.SIGUSR1)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.incomplete()
        self.intact()

    def test_change_between_inventory_and_copy_refused(self):
        child = self.paused('inventory-captured')
        message = self.mail / self.paths[0]
        message.write_bytes(b'external writer before copy')
        expected = tree(self.mail)
        child.send_signal(signal.SIGUSR1)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(tree(self.mail), expected)
        self.incomplete()

    def test_source_mutation_during_partial_copy_refused(self):
        first = self.mail / sorted(self.paths, key=os.fsencode)[0]
        first.write_bytes(self.message_bytes * 3000)
        child = self.paused('payload-write')
        copied = (self.dest / 'messages/0').stat().st_size
        self.assertGreater(copied, 0)
        self.assertLess(copied, first.stat().st_size, 'mutation must occur during a partial copy')
        first.write_bytes(b'X' * first.stat().st_size)
        expected = tree(self.mail)
        child.send_signal(signal.SIGUSR1)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(tree(self.mail), expected)
        self.incomplete()

    def test_changed_source_content_names_and_added_messages_refused(self):
        for change in ('bytes', 'rename', 'added', 'removed', 'symlink'):
            with self.subTest(change=change):
                if self.dest.exists():
                    shutil.rmtree(self.dest)
                child = self.paused('copied')
                message = self.mail / self.paths[0]
                moved = message.with_name('moved:2,S')
                added = message.with_name('new external message')
                if change == 'bytes':
                    message.write_bytes(b'external writer changed these bytes')
                elif change == 'rename':
                    message.rename(moved)
                elif change == 'added':
                    added.write_bytes(b'external addition')
                elif change == 'removed':
                    message.unlink()
                else:
                    message.unlink()
                    message.symlink_to(self.root / 'outside')
                expected = tree(self.mail)
                child.send_signal(signal.SIGUSR1)
                child.communicate(timeout=10)
                self.assertNotEqual(child.returncode, 0)
                self.assertEqual(tree(self.mail), expected, 'snapshot must not undo an external mutation')
                self.incomplete()
                if change == 'rename':
                    moved.rename(message)
                elif change == 'added':
                    added.unlink()
                elif change == 'symlink':
                    message.unlink()
                message.write_bytes(self.message_bytes)
        self.intact()

    def test_altered_missing_and_extra_payload_refused(self):
        self.capture()
        payload = self.dest / 'messages/0'
        original = payload.read_bytes()
        payload.write_bytes(b'corrupt')
        self.run_helper('verify', self.dest, succeeds=False)
        payload.unlink()
        self.run_helper('verify', self.dest, succeeds=False)
        payload.write_bytes(original)
        extra = self.dest / 'messages/undeclared'
        extra.write_bytes(b'extra')
        self.run_helper('verify', self.dest, succeeds=False)
        extra.unlink()
        self.run_helper('verify', self.dest)
        self.intact()

    def test_corrupt_missing_manifest_or_complete_refused(self):
        self.capture()
        for name in ('manifest', 'complete'):
            path = self.dest / name
            original = path.read_bytes()
            path.write_bytes(b'corrupt')
            self.run_helper('verify', self.dest, succeeds=False)
            path.unlink()
            self.run_helper('verify', self.dest, succeeds=False)
            path.write_bytes(original)
        self.intact()

    def test_manifest_canonical_paths_and_fields_are_validated(self):
        self.capture()
        data = (self.dest / 'manifest').read_bytes()
        fields = data.split(b'\0')
        for path in (b'/absolute/new/x', b'../escape/new/x', b'incoming/../new/x',
                     b'incoming//new/x', b'incoming/tmp/x', b'Accounts/new/x', b'incoming/new/',
                     b'incoming/./new/x', b'incoming/cur/../../escape', b'incoming/new/child/cur/x'):
            with self.subTest(path=path):
                changed = fields.copy()
                changed[1], changed[2] = path, b''
                self.replace_manifest(b'\0'.join(changed))
                self.run_helper('verify', self.dest, succeeds=False)
        for index, value in ((0, b'JMS9'), (2, b'wrong-flags'), (3, b'-1'), (3, b'0001'),
                             (3, b'9' * 100), (4, b'x' * 64), (-2, b'999')):
            with self.subTest(index=index, value=value):
                changed = fields.copy()
                changed[index] = value
                self.replace_manifest(b'\0'.join(changed))
                self.run_helper('verify', self.dest, succeeds=False)
        self.replace_manifest(data + b'extra')
        self.run_helper('verify', self.dest, succeeds=False)
        self.replace_manifest(data[:-1])
        self.run_helper('verify', self.dest, succeeds=False)
        self.replace_manifest(data)
        self.run_helper('verify', self.dest)

    def test_duplicate_and_reordered_manifest_paths_refused(self):
        self.capture()
        fields = (self.dest / 'manifest').read_bytes().split(b'\0')
        for duplicate in (True, False):
            changed = fields.copy()
            if duplicate:
                changed[5:9] = changed[1:5]
            else:
                changed[1:5], changed[5:9] = changed[5:9], changed[1:5]
            self.replace_manifest(b'\0'.join(changed))
            self.run_helper('verify', self.dest, succeeds=False)

    def test_snapshot_symlink_objects_and_ancestor_refused(self):
        self.capture()
        for relative in ('messages/0', 'manifest', 'complete'):
            path = self.dest / relative
            original = path.read_bytes()
            outside = self.root / 'outside'
            outside.write_bytes(original)
            path.unlink()
            path.symlink_to(outside)
            self.run_helper('verify', self.dest, succeeds=False)
            self.assertEqual(outside.read_bytes(), original)
            path.unlink()
            path.write_bytes(original)
        alias = self.root / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        self.run_helper('verify', alias / 'snapshot', succeeds=False)
        self.intact()

    def test_production_ignores_fault_hooks(self):
        self.run_helper('create', self.mail, self.dest, production=True, fault='payload-write')
        self.run_helper('verify', self.dest, production=True)
        self.intact()

    def test_both_completion_actions_and_exact_bash_directory(self):
        bash = REPO / 'extras/shell_completion/jaromail.bash'
        result = subprocess.run(['bash', '-c',
                                 'source "$1"; COMP_WORDS=(jaro snapshot ""); COMP_CWORD=2; '
                                 '_jaro; printf "%s\\0" "${COMPREPLY[@]}"', '_', str(bash)],
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(b'\0'), [b'create', b'verify', b'restore', b''])
        directory = self.root / 'with space\nand newline'
        directory.mkdir()
        result = subprocess.run(['bash', '-c',
                                 'source "$1"; COMP_WORDS=(jaro snapshot create "$2"); COMP_CWORD=3; '
                                 '_jaro; printf "%s\\0" "${COMPREPLY[@]}"', '_', str(bash), str(directory)],
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, os.fsencode(directory) + b'\0')
        zsh = REPO / 'extras/shell_completion/_jaromail'
        result = subprocess.run(['zsh', '-c',
                                 'words=(jaro snapshot ""); CURRENT=3; curcontext=""; '
                                 'compadd() { printf "%s\\0" "$@"; }; source "$1"', '_', str(zsh)],
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(b'\0'), [b'create', b'verify', b'restore', b''])

    def test_source_cli_without_bootstrap_or_transport(self):
        tools = self.root / 'tools'
        tools.mkdir()
        log = self.root / 'unexpected-calls'
        for name in ('msmtp', 'fetchmail', 'notmuch', 'gpg', 'pinentry'):
            path = tools / name
            path.write_text('#!/bin/sh\nprintf unexpected >> "$SNAPSHOT_CALL_LOG"\nexit 91\n')
            path.chmod(0o700)
        env = dict(self.env, PATH=str(tools) + os.pathsep + self.env['PATH'], SNAPSHOT_CALL_LOG=str(log))
        home_before = tree(self.home)
        for args in (('snapshot', 'create', str(self.dest)), ('snapshot', 'verify', str(self.dest)),
                     ('snapshot', 'restore', str(self.dest), str(self.restored)),
                     ('snapshot', '--help')):
            result = subprocess.run([str(REPO / 'src/jaro'), *args], env=env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
        for args in (('snapshot',), ('snapshot', 'create'), ('snapshot', 'delete', str(self.dest)),
                     ('snapshot', 'verify', str(self.dest), 'extra'), ('snapshot', 'create', ''),
                     ('snapshot', 'restore', str(self.dest)), ('snapshot', 'restore', str(self.dest), ''),
                     ('-n', 'snapshot', 'create', str(self.root / 'dry-run')),
                     ('-a', 'account', 'snapshot', 'verify', str(self.dest))):
            result = subprocess.run([str(REPO / 'src/jaro'), *args], env=env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(tree(self.home), home_before)
        self.assertFalse(log.exists())
        self.intact()

    def no_restore_stage(self):
        self.assertEqual(list(self.root.glob('.jaro-restore-*')), [], 'every owned staging tree must be removed')

    def assert_roundtrip(self):
        held = '.jaro-recovered-outbox-0'
        if any(p.startswith(held + '/') for p in self.paths):
            held = '.jaro-recovered-outbox-1'
        expected = {}
        for path in self.paths:
            mapped = held + '/' + path if path.startswith('outbox/') else path
            expected[os.fsencode(mapped)] = (self.mail / path).read_bytes()
            actual = self.restored / mapped
            self.assertEqual(actual.read_bytes(), expected[os.fsencode(mapped)])
            for branch in ('new', 'cur', 'tmp'):
                self.assertTrue((actual.parent.parent / branch).is_dir())
        actual = {os.fsencode(p.relative_to(self.restored)): p.read_bytes()
                  for p in self.restored.rglob('*') if p.is_file()}
        self.assertEqual(actual, expected, 'recover every captured message and no configuration/index/journal/tmp files')
        self.assertFalse((self.restored / 'outbox').exists(), 'no restored automatically sendable queue')
        for path in [self.restored, *self.restored.rglob('*')]:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700 if path.is_dir() else 0o600)
        self.no_restore_stage()

    def test_restore_roundtrip_exact_bytes_placement_flags_and_held_outbox(self):
        self.capture()
        snapshot_before = tree(self.dest)
        result = self.run_helper('restore', self.dest, self.restored)
        self.assertIn(b'outbox message(s) held', result.stdout)
        self.assert_roundtrip()
        self.assertEqual(tree(self.dest), snapshot_before)
        self.intact()

    def test_restore_existing_empty_directory_and_collision_free_recovery(self):
        path = '.jaro-recovered-outbox-0/new/already held:2,S'
        self.paths.append(path)
        for branch in ('new', 'cur', 'tmp'):
            (self.mail / '.jaro-recovered-outbox-0' / branch).mkdir(parents=True)
        (self.mail / path).write_bytes(b'prior held message')
        self.before = tree(self.mail)
        self.capture()
        self.restored.mkdir(mode=0o755)
        result = self.run_helper('restore', self.dest, self.restored)
        self.assertIn(b'.jaro-recovered-outbox-1/outbox', result.stdout)
        self.assert_roundtrip()
        self.intact()

    def test_restore_empty_snapshot_and_root_maildir(self):
        empty = self.root / 'empty source'
        empty.mkdir()
        self.run_helper('create', empty, self.dest)
        self.run_helper('restore', self.dest, self.restored)
        self.assertEqual(tree(self.restored), {})
        self.restored.rmdir()
        shutil.rmtree(self.dest)
        for branch in ('new', 'cur', 'tmp'):
            (empty / branch).mkdir()
        (empty / 'cur' / 'root flags:2,RS').write_bytes(b'root message')
        self.run_helper('create', empty, self.dest)
        self.run_helper('restore', self.dest, self.restored)
        self.assertEqual((self.restored / 'cur' / 'root flags:2,RS').read_bytes(), b'root message')
        self.assertTrue((self.restored / 'new').is_dir())
        self.assertTrue((self.restored / 'tmp').is_dir())
        self.no_restore_stage()

    def test_restore_populated_directory_and_existing_file_preserved(self):
        self.capture()
        self.restored.mkdir()
        (self.restored / 'existing mail').write_bytes(b'keep every byte')
        before = tree(self.restored)
        self.run_helper('restore', self.dest, self.restored, succeeds=False)
        self.assertEqual(tree(self.restored), before)
        self.no_restore_stage()
        shutil.rmtree(self.restored)
        self.restored.write_bytes(b'keep this file')
        self.run_helper('restore', self.dest, self.restored, succeeds=False)
        self.assertEqual(self.restored.read_bytes(), b'keep this file')
        self.no_restore_stage()
        self.intact()

    def test_restore_symlink_roots_parents_and_nested_destination_refused(self):
        self.capture()
        snapshot_before = tree(self.dest)
        alias = self.root / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        self.run_helper('restore', alias / 'snapshot', self.restored, succeeds=False)
        self.run_helper('restore', self.dest, alias / 'restored mail', succeeds=False)
        self.run_helper('restore', self.dest, self.dest / 'nested restore', succeeds=False)
        self.restored.symlink_to(self.root / 'outside')
        self.run_helper('restore', self.dest, self.restored, succeeds=False)
        self.assertTrue(self.restored.is_symlink())
        self.assertFalse((self.root / 'outside').exists())
        self.assertEqual(tree(self.dest), snapshot_before)
        self.no_restore_stage()
        self.intact()

    def test_restore_corruption_and_traversal_refused_before_any_destination_write(self):
        self.capture()
        data = (self.dest / 'manifest').read_bytes()
        original = (self.dest / 'messages/0').read_bytes()
        for kind in ('payload', 'traversal', 'structural-path', 'manifest', 'marker', 'missing', 'symlink'):
            with self.subTest(kind=kind):
                payload = self.dest / 'messages/0'
                if kind == 'payload':
                    payload.write_bytes(b'corrupt payload')
                elif kind in ('traversal', 'structural-path'):
                    fields = data.split(b'\0')
                    fields[1] = b'../outside/new/x' if kind == 'traversal' else b'folder/new/child/cur/x'
                    fields[2] = b''
                    self.replace_manifest(b'\0'.join(fields))
                elif kind == 'manifest':
                    (self.dest / 'manifest').write_bytes(b'corrupt manifest')
                elif kind == 'marker':
                    (self.dest / 'complete').write_bytes(b'corrupt marker')
                elif kind == 'missing':
                    payload.unlink()
                else:
                    payload.unlink()
                    payload.symlink_to(self.root / 'absent')
                expected = tree(self.dest)
                self.run_helper('restore', self.dest, self.restored, succeeds=False)
                self.assertEqual(tree(self.dest), expected)
                self.assertFalse(self.restored.exists())
                self.no_restore_stage()
                if payload.is_symlink():
                    payload.unlink()
                payload.write_bytes(original)
                self.replace_manifest(data)
        self.intact()

    def test_restore_read_write_sync_and_publication_failures_preserve_both_sources(self):
        self.capture()
        snapshot_before = tree(self.dest)
        for existing in (False, True):
            if existing:
                self.restored.mkdir(mode=0o751)
            for phase in ('restore-read', 'payload-write', 'restore-file-sync', 'restore-publish', 'restore-directory-sync'):
                with self.subTest(existing=existing, phase=phase):
                    result = self.run_helper('restore', self.dest, self.restored, succeeds=False, fault=phase)
                    self.assertIn(('SNAPSHOT-TEST fault reached: ' + phase).encode(), result.stderr)
                    if existing:
                        self.assertEqual(tree(self.restored), {})
                        self.assertEqual(stat.S_IMODE(self.restored.stat().st_mode), 0o751)
                    else:
                        self.assertFalse(self.restored.exists())
                    self.no_restore_stage()
                    self.assertEqual(tree(self.dest), snapshot_before)
                    self.intact()

    def test_restore_interruption_at_each_boundary_settles_child_and_staging(self):
        self.capture()
        snapshot_before = tree(self.dest)
        for phase in ('restore-verified', 'restore-staged', 'payload-write', 'restore-copied',
                      'restore-publish', 'restore-directory-sync'):
            with self.subTest(phase=phase):
                child = self.paused(phase, restore=True)
                if phase == 'restore-staged':
                    stages = list(self.root.glob('.jaro-restore-*'))
                    self.assertEqual(len(stages), 1)
                    self.assertEqual(stat.S_IMODE(stages[0].stat().st_mode), 0o700)
                    self.assertEqual(stages[0].stat().st_dev, self.restored.parent.stat().st_dev)
                child.send_signal(signal.SIGTERM)
                child.communicate(timeout=10)
                self.assertEqual(child.returncode, 128 + signal.SIGTERM)
                self.assertRaises(ProcessLookupError, os.kill, child.pid, 0)
                self.assertFalse(self.restored.exists())
                self.no_restore_stage()
                self.assertEqual(tree(self.dest), snapshot_before)
                self.intact()

    def test_restore_changed_snapshot_after_verification_or_copy_refused(self):
        self.capture()
        original = (self.dest / 'messages/0').read_bytes()
        data = (self.dest / 'manifest').read_bytes()
        for phase in ('restore-verified', 'restore-copied', 'restore-publish'):
            for damage in ('payload', 'manifest', 'missing', 'symlink'):
                with self.subTest(phase=phase, damage=damage):
                    child = self.paused(phase, restore=True)
                    payload = self.dest / 'messages/0'
                    if damage == 'payload':
                        payload.write_bytes(b'externally modified snapshot')
                    elif damage == 'manifest':
                        (self.dest / 'manifest').write_bytes(b'external manifest edit')
                    elif damage == 'missing':
                        payload.unlink()
                    else:
                        payload.unlink()
                        payload.symlink_to(self.root / 'outside')
                    expected = tree(self.dest)
                    child.send_signal(signal.SIGUSR1)
                    child.communicate(timeout=10)
                    self.assertNotEqual(child.returncode, 0)
                    self.assertEqual(tree(self.dest), expected)
                    self.assertFalse(self.restored.exists())
                    self.no_restore_stage()
                    if payload.is_symlink():
                        payload.unlink()
                    payload.write_bytes(original)
                    self.replace_manifest(data)
        self.intact()

    def test_restore_snapshot_mutated_during_partial_copy_refused(self):
        first = self.mail / sorted(self.paths, key=os.fsencode)[0]
        first.write_bytes(self.message_bytes * 3000)
        self.before = tree(self.mail)
        self.capture()
        child = self.paused('payload-write', restore=True)
        stage = list(self.root.glob('.jaro-restore-*'))[0]
        copied = list(stage.rglob('hidden message'))[0].stat().st_size
        self.assertGreater(copied, 0)
        self.assertLess(copied, first.stat().st_size)
        (self.dest / 'messages/0').write_bytes(b'X' * first.stat().st_size)
        expected = tree(self.dest)
        child.send_signal(signal.SIGUSR1)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(tree(self.dest), expected)
        self.assertFalse(self.restored.exists())
        self.no_restore_stage()
        self.intact()

    def test_restore_staged_symlink_escape_and_corrupt_staged_bytes_refused(self):
        self.capture()
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'keep').write_bytes(b'never touch outside')
        before = tree(outside)
        for kind in ('symlink', 'bytes'):
            with self.subTest(kind=kind):
                child = self.paused('restore-copied', restore=True)
                stage = list(self.root.glob('.jaro-restore-*'))[0]
                target = stage / 'incoming'
                if kind == 'symlink':
                    shutil.rmtree(target)
                    target.symlink_to(outside, target_is_directory=True)
                else:
                    (stage / self.paths[0]).write_bytes(b'storage changed staged bytes')
                child.send_signal(signal.SIGUSR1)
                child.communicate(timeout=10)
                self.assertNotEqual(child.returncode, 0)
                self.assertFalse(self.restored.exists())
                self.no_restore_stage()
                self.assertEqual(tree(outside), before)
                self.intact()

    def test_restore_destination_populated_before_publish_is_preserved(self):
        self.capture()
        child = self.paused('restore-publish', restore=True)
        self.restored.mkdir()
        (self.restored / 'new external mail').write_bytes(b'preserve concurrent destination mail')
        before = tree(self.restored)
        child.send_signal(signal.SIGUSR1)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(tree(self.restored), before)
        self.no_restore_stage()
        self.intact()

    def test_restore_production_ignores_fault_hooks(self):
        self.capture()
        self.run_helper('restore', self.dest, self.restored, production=True, fault='restore-file-sync')
        self.assert_roundtrip()
        self.intact()


if __name__ == '__main__':
    result = unittest.main(verbosity=2, exit=False).result
    summary = dict(executed=result.testsRun, failed=len(result.errors) + len(result.failures),
                   skipped=len(result.skipped))
    print('Snapshot cases: ' + json.dumps(summary, sort_keys=True))
    raise SystemExit(0 if result.wasSuccessful() and result.testsRun and not result.skipped else 1)
