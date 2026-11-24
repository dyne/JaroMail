"""Exercise first-use contact migration with synthetic profiles."""

import argparse
from contextlib import closing, contextmanager
import fcntl
import os
from pathlib import Path
import resource
import selectors
import signal
import sqlite3
import subprocess
import tempfile
import time
import unittest


REPO = Path(__file__).resolve().parents[2]
DESTINATION = 'contacts.sqlite'
STAGE = '.contacts.sqlite.import'
LOCK = '.contacts.sqlite.init.lock'
APPLICATION_ID = 0x4A434E54


def legacy(name='Synthetic', *addresses):
    return f'[0]\nname={name}\nemail={",".join(addresses)}\n'.encode()


def readonly(path):
    return closing(sqlite3.connect(f'{path.as_uri()}?mode=ro', uri=True))


class MigrationTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='jaro-contact-migrate-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.profile = self.new_profile('Main profile')
        self.db = self.profile / DESTINATION

    def new_profile(self, name):
        profile = self.root / name
        profile.mkdir(mode=0o700)
        return profile

    def write_source(self, name, content, *, profile=None):
        path = (profile or self.profile) / name
        path.write_bytes(content)
        return path

    def write_historical(self):
        path = self.profile / 'Addressbook'
        with closing(sqlite3.connect(path)) as old:
            old.executescript('CREATE TABLE whitelist(email TEXT, name TEXT);'
                              'CREATE TABLE blacklist(email TEXT, name TEXT);')
            old.execute('INSERT INTO whitelist VALUES(?,?)', ('historical@example.org', 'Historical'))
            old.commit()
        return path

    def run_migrate(self, *, profile=None, status=0, full=False, **kwargs):
        env = os.environ.copy()
        if full:
            env['JARO_MIGRATION_FULL'] = '1'
        process = subprocess.run([self.driver, 'init', profile or self.profile], capture_output=True,
                                 timeout=15, env=env, **kwargs)
        self.assertEqual(process.returncode, status, process.stderr.decode(errors='replace'))
        return process

    def assert_no_stage(self, profile=None):
        profile = profile or self.profile
        self.assertFalse((profile / STAGE).exists())
        self.assertFalse((profile / (STAGE + '-journal')).exists())

    def records(self, path=None):
        with readonly(path or self.db) as db:
            return db.execute('SELECT b.name,c.name,c.revision FROM contacts c JOIN books b '
                              'ON c.book_id=b.id ORDER BY b.name,c.ordinal').fetchall()

    @contextmanager
    def held(self, phase, account='a'):
        ready, signal_ready = os.pipe()
        receive_release, release = os.pipe()
        env = dict(os.environ, JARO_MIGRATION_HOLD=phase, JARO_MIGRATION_READY_FD=str(signal_ready),
                   JARO_MIGRATION_RELEASE_FD=str(receive_release), JARO_TEST_ACCOUNT=account)
        child = None
        try:
            child = subprocess.Popen([self.driver, 'init', self.profile], env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, pass_fds=(signal_ready, receive_release))
            os.close(signal_ready)
            signal_ready = None
            os.close(receive_release)
            receive_release = None
            with selectors.DefaultSelector() as selector:
                selector.register(ready, selectors.EVENT_READ)
                self.assertTrue(selector.select(timeout=10), f'{phase} was not reached')
                self.assertEqual(os.read(ready, 1), b'R', f'{phase} closed without reaching the gate')
            released = False

            def resume():
                nonlocal released
                if not released:
                    os.write(release, b'R')
                    released = True

            yield child, resume
        finally:
            for descriptor in (ready, signal_ready, receive_release, release):
                if descriptor is not None:
                    os.close(descriptor)
            if child is not None:
                try:
                    child.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate(timeout=3)
                self.assertIsNotNone(child.poll())

    def test_empty_profile_and_historical_database_are_independent(self):
        historical = self.write_historical()
        original = historical.read_bytes()
        self.assertEqual(self.run_migrate().stdout, b'')
        self.assertEqual(self.records(), [])
        with readonly(self.db) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM books').fetchone(), (0,))
        before = self.db.read_bytes()
        self.assertEqual(self.run_migrate().stdout, b'')
        self.assertEqual(self.db.read_bytes(), before)
        self.assertEqual(historical.read_bytes(), original)
        self.assertEqual((self.db.stat().st_mode & 0o777, (self.profile / LOCK).stat().st_mode & 0o777),
                         (0o600, 0o600))
        self.assert_no_stage()

    def test_alias_merges_sorted_sources_and_never_reconsults_them(self):
        historical = self.write_historical()
        before_historical = historical.read_bytes()
        sources = {
            'block.abook': legacy('Block', 'dup@example.org'),
            'whitelist.abook': legacy('Whitelist', 'DUP@example.org'),
            'blacklist.abook': legacy('Blacklist', 'dup@example.org'),
            'allow.abook': (legacy('Allow Zoë', '"A,B"@Example.org', 'dup@example.org') +
                            'note_custom=한글\t✨ %_'.encode()),
            'allow-block-漢字.abook': legacy('Custom', 'secondary@example.org'),
            'empty.abook': b'',
        }
        for name, content in sources.items():
            self.write_source(name, content)
        result = self.run_migrate()
        self.assertEqual(result.stdout, b'source 1: 1\nsource 2: 1\nsource 3: 1\nsource 4: 1\nsource 5: 0\nsource 6: 1\n')
        self.assertEqual(self.records(), [('allow', 'Allow Zoë', 1), ('allow', 'Whitelist', 1),
                                         ('allow-block-漢字', 'Custom', 1), ('block', 'Blacklist', 1),
                                         ('block', 'Block', 1)])
        with readonly(self.db) as db:
            self.assertEqual(db.execute("SELECT spelling FROM addresses a JOIN contacts c "
                                        "ON a.contact_id=c.id WHERE c.name=? ORDER BY a.ordinal",
                                        ('Allow Zoë',)).fetchall(),
                             [('"A,B"@Example.org',), ('dup@example.org',)])
            self.assertEqual(db.execute('SELECT key,value FROM fields').fetchall(), [('note_custom', '한글\t✨ %_')])
            self.assertEqual(db.execute('SELECT count(*) FROM books WHERE name=?', ('empty',)).fetchone(), (1,))
        for name, contents in sources.items():
            self.assertEqual((self.profile / name).read_bytes(), contents)
        self.assertEqual(historical.read_bytes(), before_historical)
        before = self.db.read_bytes()
        poison = self.write_source('whitelist.abook', b'broken legacy bytes')
        os.mkfifo(self.profile / 'future.abook')
        self.assertEqual(self.run_migrate().stdout, b'')
        self.assertEqual(poison.read_bytes(), b'broken legacy bytes')
        self.assertEqual(self.db.read_bytes(), before)
        self.assert_no_stage()

    def test_damaged_existing_database_takes_precedence_over_valid_legacy(self):
        for number, fixture in enumerate(('empty', 'text', 'unreadable', 'unsupported', 'wrong identity', 'wrong shape')):
            with self.subTest(fixture=fixture):
                profile = self.new_profile(str(number))
                db_path = profile / DESTINATION
                descriptor = os.open(db_path, os.O_CREAT | os.O_EXCL | os.O_RDWR,
                                     0 if fixture == 'unreadable' else 0o600)
                with os.fdopen(descriptor, 'wb') as db_file:
                    if fixture == 'text':
                        db_file.write(b'fake database bytes')
                if fixture in ('unsupported', 'wrong identity', 'wrong shape'):
                    with closing(sqlite3.connect(db_path)) as db:
                        if fixture != 'wrong identity':
                            db.execute(f'PRAGMA application_id={APPLICATION_ID}')
                        db.execute(f'PRAGMA user_version={99 if fixture == "unsupported" else 1}')
                        db.execute('CREATE TABLE unrelated(id INTEGER)')
                legacy_path = self.write_source('whitelist.abook', legacy('Good', 'ok@example.org'),
                                                profile=profile)
                before = None if fixture == 'unreadable' else db_path.read_bytes()
                result = self.run_migrate(profile=profile, status=5)
                self.assertEqual(result.stdout, b'')
                self.assertIn(b'source=0 record=0 line=0 field=0', result.stderr)
                self.assertFalse((profile / LOCK).exists())
                self.assertEqual(legacy_path.read_bytes(), legacy('Good', 'ok@example.org'))
                if before is not None:
                    self.assertEqual(db_path.read_bytes(), before)
                self.assert_no_stage(profile)

    def test_corrupt_data_page_is_detected_without_import(self):
        legacy_path = self.write_source('whitelist.abook', legacy('Good', 'ok@example.org'))
        self.run_migrate()
        with readonly(self.db) as db:
            page_size, = db.execute('PRAGMA page_size').fetchone()
            root_page, = db.execute("SELECT rootpage FROM sqlite_schema WHERE name='addresses'").fetchone()
        self.assertGreater(root_page, 1)
        with self.db.open('r+b') as stream:
            stream.seek((root_page - 1) * page_size)
            stream.write(b'\0')
        damaged = self.db.read_bytes()
        result = self.run_migrate(status=5)
        self.assertIn(b'contact database integrity check failed', result.stderr)
        self.assertEqual(self.db.read_bytes(), damaged)
        self.assertEqual(legacy_path.read_bytes(), legacy('Good', 'ok@example.org'))
        self.assert_no_stage()

    def test_malformed_and_unreadable_sources_leave_retryable_profile(self):
        first = self.write_source('allow.abook', legacy('Good', 'ok@example.org'))
        second = self.write_source('whitelist.abook', b'[0]\nname=private\nnotes=hidden\xed\xa0\x80')
        bad = second.read_bytes()
        result = self.run_migrate(status=1)
        self.assertIn(b'source=2 record=1 line=3 field=2', result.stderr)
        self.assertNotIn(b'hidden', result.stderr)
        self.assertEqual(second.read_bytes(), bad)
        self.assertFalse(self.db.exists())
        self.assert_no_stage()
        second.unlink()
        descriptor = os.open(second, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0)
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(b'not readable')
        result = self.run_migrate(status=5)
        self.assertIn(b'source=2 record=0 line=0 field=0', result.stderr)
        self.assertFalse(self.db.exists())
        self.assert_no_stage()
        second.unlink()
        self.run_migrate()
        self.assertEqual(first.read_bytes(), legacy('Good', 'ok@example.org'))
        self.assertEqual(self.records(), [('allow', 'Good', 1)])

    def test_nonregular_source_keeps_original_target_untouched(self):
        original = self.write_source('outside', legacy('Target', 'ok@example.org'))
        source = self.profile / 'whitelist.abook'
        source.symlink_to(original)
        result = self.run_migrate(status=5)
        self.assertIn(b'cannot open legacy source', result.stderr)
        self.assertEqual(source.readlink(), original)
        self.assertEqual(original.read_bytes(), legacy('Target', 'ok@example.org'))
        self.assertFalse(self.db.exists())
        source.unlink()
        os.mkfifo(source)
        result = self.run_migrate(status=5)
        self.assertIn(b'legacy source must be a readable regular file', result.stderr)
        self.assertTrue(source.is_fifo())
        self.assertFalse(self.db.exists())
        self.assert_no_stage()

    def test_many_sources_use_bounded_descriptors(self):
        for number in range(40):
            self.write_source(f'{number:02}.abook', legacy(f'Name {number}', 'ok@example.org'))

        def cap_descriptors():
            _, ceiling = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (16, ceiling))

        result = self.run_migrate(preexec_fn=cap_descriptors)
        self.assertEqual(len(result.stdout.splitlines()), 40)
        self.assertEqual(len(self.records()), 40)

    def test_concurrent_accounts_wait_and_import_once(self):
        self.write_source('whitelist.abook', legacy('Shared', 'ok@example.org'))
        with self.held('before_publish', 'account-a') as (first, publish):
            self.assertFalse(self.db.exists())
            self.assertTrue((self.profile / STAGE).exists())
            with (self.profile / LOCK).open('rb') as fd:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.held('before_lock', 'account-b') as (second, start):
                start()
                with self.assertRaises(subprocess.TimeoutExpired):
                    second.wait(timeout=0.1)
                self.assertIsNone(first.poll())
                publish()
                self.assertEqual(first.communicate(timeout=10), (b'source 1: 1\n', b''))
                self.assertEqual(second.communicate(timeout=10), (b'', b''))
                self.assertEqual((first.returncode, second.returncode), (0, 0))
        self.assertEqual(self.records(), [('allow', 'Shared', 1)])
        self.assert_no_stage()

    def test_source_change_before_publication_leaves_retryable_profile(self):
        source = self.write_source('whitelist.abook', legacy('Before', 'ok@example.org'))
        with self.held('before_publish') as (held, proceed):
            with source.open('r+b') as stream:
                stream.write(legacy('After!', 'ok@example.org'))
            proceed()
            out, diagnostics = held.communicate(timeout=10)
            self.assertEqual((held.returncode, out), (4, b''))
            self.assertIn(b'legacy source changed', diagnostics)
        self.assertFalse(self.db.exists())
        self.assert_no_stage()
        self.run_migrate()
        self.assertEqual(self.records(), [('allow', 'After!', 1)])

    def test_existing_opener_can_finish_sync_during_publication(self):
        source = self.write_source('whitelist.abook', legacy('Shared', 'ok@example.org'))
        with self.held('after_publish') as (child, proceed):
            self.assertEqual(self.db.stat().st_nlink, 2)
            self.assertTrue((self.profile / STAGE).exists())
            source.write_bytes(b'changed after one-time import')
            self.assertEqual(self.run_migrate().stdout, b'')
            self.assertEqual(self.records(), [('allow', 'Shared', 1)])
            self.assertEqual(self.db.stat().st_nlink, 1)
            self.assert_no_stage()
            proceed()
            self.assertEqual(child.communicate(timeout=10), (b'source 1: 1\n', b''))
            self.assertEqual(child.returncode, 0)

    def test_changed_verified_stage_is_never_published(self):
        self.write_source('whitelist.abook', legacy('Good', 'ok@example.org'))
        with self.held('before_publish') as (child, proceed):
            with closing(sqlite3.connect(self.profile / STAGE)) as stage:
                stage.execute('UPDATE contacts SET name=?', ('Changed outside the initializer',))
                stage.commit()
            proceed()
            stdout, stderr = child.communicate(timeout=10)
            self.assertEqual((child.returncode, stdout), (5, b''))
            self.assertIn(b'completed contact temporary changed', stderr)
        self.assertFalse(self.db.exists())
        self.assert_no_stage()
        self.run_migrate()
        self.assertEqual(self.records(), [('allow', 'Good', 1)])

    def test_commit_disk_full_rolls_back_and_can_retry(self):
        source = self.write_source('whitelist.abook', legacy('Full', 'ok@example.org'))
        original = source.read_bytes()
        result = self.run_migrate(status=5, full=True)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'fault: SQLITE_FULL on database write after a successful write', result.stderr)
        self.assertIn(b'native=13 cannot commit contact write', result.stderr)
        self.assertFalse(self.db.exists())
        self.assert_no_stage()
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(self.run_migrate().stdout, b'source 1: 1\n')
        self.assertEqual(self.records(), [('allow', 'Full', 1)])

    def test_termination_boundaries_recover(self):
        for phase in ('before_commit', 'before_publish', 'after_publish'):
            with self.subTest(phase=phase):
                self.profile = self.new_profile(phase)
                self.db = self.profile / DESTINATION
                source = self.write_source('whitelist.abook', legacy('Crash', 'ok@example.org'))
                original = source.read_bytes()
                with self.held(phase) as (child, _):
                    self.assertTrue((self.profile / STAGE).exists())
                    self.assertEqual(self.db.exists(), phase == 'after_publish')
                    if phase == 'after_publish':
                        self.assertEqual(self.db.stat().st_nlink, 2)
                    elif phase == 'before_commit':
                        self.assertTrue((self.profile / (STAGE + '-journal')).exists())
                    child.kill()
                    child.communicate(timeout=10)
                    self.assertEqual(child.returncode, -signal.SIGKILL)
                self.assertEqual(source.read_bytes(), original)
                if phase == 'after_publish':
                    source.write_bytes(b'broken to verify no re-import')
                result = self.run_migrate()
                self.assertEqual(result.stdout, b'' if phase == 'after_publish' else b'source 1: 1\n')
                self.assertEqual(self.records(), [('allow', 'Crash', 1)])
                self.assertEqual(self.db.stat().st_nlink, 1)
                self.assert_no_stage()

    def test_competing_publication_is_not_overwritten(self):
        competitor = self.new_profile('other')
        self.write_source('allow.abook', legacy('Competitor', 'ok@example.org'), profile=competitor)
        self.run_migrate(profile=competitor)
        original = (competitor / DESTINATION).read_bytes()
        self.write_source('allow.abook', legacy('Would overwrite', 'ok@example.org'))
        with self.held('before_publish') as (child, proceed):
            os.link(competitor / DESTINATION, self.db)
            proceed()
            self.assertEqual(child.communicate(timeout=10), (b'', b''))
            self.assertEqual(child.returncode, 0)
        self.assertEqual(self.db.read_bytes(), original)
        self.assertEqual(self.records(), [('allow', 'Competitor', 1)])
        self.assert_no_stage()

    def test_consistent_backup_includes_edits_after_import(self):
        self.write_source('whitelist.abook', legacy('Imported', 'ORIGINAL@Example.org', 'second@example.org'))
        self.run_migrate()
        before = self.records()
        edit = subprocess.run([self.driver, 'edit', self.db, 'allow', 'Űpdated'], capture_output=True, timeout=10)
        self.assertEqual(edit.returncode, 0, edit.stderr)
        self.assertEqual(before, [('allow', 'Imported', 1)])
        restored = self.new_profile('restore')
        copy = restored / DESTINATION
        fd = os.open(copy, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(fd)
        with readonly(self.db) as source, closing(sqlite3.connect(copy)) as backup:
            source.backup(backup)
        self.assertEqual(self.run_migrate(profile=restored).stdout, b'')
        self.assertEqual(self.records(copy), [('allow', 'Űpdated', 2)])
        with readonly(copy) as db:
            self.assertEqual(db.execute('SELECT spelling FROM addresses ORDER BY ordinal').fetchall(),
                             [('ORIGINAL@Example.org',), ('second@example.org',)])
            self.assertEqual(db.execute('PRAGMA integrity_check').fetchone(), ('ok',))
        self.assertFalse((restored / LOCK).exists())

    def test_legacy_startup_keeps_historical_addressbook(self):
        historical = self.write_historical()
        original = historical.read_bytes()
        primary = self.write_source('whitelist.abook', legacy('Keep', 'ok@example.org'))
        result = self.run_migrate()
        self.assertEqual(result.stdout, b'source 1: 1\n')
        self.assertEqual(result.stderr, b'')
        self.assertEqual(historical.read_bytes(), original)
        self.assertFalse((self.profile / 'Addressbook.old').exists())
        self.assertEqual(primary.read_bytes(), legacy('Keep', 'ok@example.org'))
        self.assertEqual(self.records(), [('allow', 'Keep', 1)])



class WriterTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='jaro-contact-write-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.profile = self.root / 'Main profile'
        self.profile.mkdir(mode=0o700)
        self.db = self.profile / DESTINATION
        self.write_accounts(self.profile)

    def write_accounts(self, profile):
        directory = profile / 'Accounts'
        directory.mkdir()
        for account in ('alice', 'bob'):
            (directory / account).write_text(
                f'name {account}\nlogin {account}\nemail {account}@example.org\nhost example.org\n')

    def argv(self, action, book, *args, account):
        return ['zsh', '-f', '-c',
                'source "$1" doctor || exit 90; source "$2" || exit 91; '
                'func() { :; }; act() { :; }; error() { print -u2 -r -- "$*"; }; '
                'account="$3"; read_account || exit 92; [[ $login == "$3" ]] || exit 93; '
                'shift 3; driver="$1" action="$2"; shift 2; exec "$driver" "$action" "$MAILDIRS" "$@"',
                'writer-account', REPO / 'src/zlibs/bootstrap', REPO / 'src/zlibs/accounts',
                account, self.driver, action, book, *map(str, args)]

    def profile_env(self, profile):
        return dict(os.environ, JAROMAILDIR=str(profile), JAROACCOUNT=str(profile / 'Accounts'))

    def run_writer(self, action, book, *args, account='alice', profile=None, status=0, full=False):
        env = self.profile_env(profile or self.profile)
        if full:
            env['JARO_WRITER_FULL'] = '1'
        result = subprocess.run(self.argv(action, book, *args, account=account), env=env,
                                capture_output=True, timeout=8)
        self.assertEqual(result.returncode, status, result.stderr)
        return result

    @contextmanager
    def held(self, phase, action, book, *args, account='alice'):
        ready, send_ready = os.pipe()
        receive, release = os.pipe()
        child = None
        try:
            env = dict(self.profile_env(self.profile), JARO_WRITER_HOLD=phase,
                       JARO_WRITER_READY_FD=str(send_ready), JARO_WRITER_RELEASE_FD=str(receive))
            child = subprocess.Popen(self.argv(action, book, *args, account=account), env=env,
                                     pass_fds=(send_ready, receive), stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
            os.close(send_ready)
            send_ready = None
            os.close(receive)
            receive = None
            with selectors.DefaultSelector() as selector:
                selector.register(ready, selectors.EVENT_READ)
                self.assertTrue(selector.select(timeout=6), f'{phase} not reached')
                self.assertEqual(os.read(ready, 1), b'R')

            def proceed():
                nonlocal release
                if release is not None:
                    os.write(release, b'R')
                    os.close(release)
                    release = None

            yield child, proceed
        finally:
            for fd in (ready, send_ready, receive, release):
                if fd is not None:
                    os.close(fd)
            if child is not None:
                try:
                    child.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate(timeout=3)
                self.assertIsNotNone(child.poll())

    def seed(self):
        (self.profile / 'whitelist.abook').write_bytes(
            legacy('Initial', 'Initial@example.org', 'secondary@example.org') +
            b'\n[1]\nname=Neighbor\nemail=neighbor@example.org\n')
        self.run_writer('member', 'allow', 'secondary@example.org')
        return {name: contact_id for contact_id, name, _ in self.records()}

    def records(self):
        with readonly(self.db) as db:
            return db.execute('SELECT id,name,revision FROM contacts ORDER BY id').fetchall()

    def generation(self):
        with readonly(self.db) as db:
            return db.execute('SELECT generation FROM metadata').fetchone()[0]

    def test_learn_during_draft_preserves_name_and_secondary_addresses(self):
        initial = self.seed()['Initial']
        with self.held('draft', 'save', 'allow', initial, 'Edited') as (editor, proceed):
            self.assertEqual(self.run_writer('learn', 'allow', 'INITIAL@example.org', 'From header',
                                            account='bob').stdout, b'present\n')
            self.assertEqual(self.run_writer('learn', 'allow', 'new@example.org', 'Sender',
                                            account='bob').stdout, b'new\n')
            proceed()
            self.assertEqual(editor.communicate(timeout=6), (b'', b''))
            self.assertEqual(editor.returncode, 0)
        edited = self.run_writer('query', 'allow', 'edit', account='bob').stdout
        self.assertEqual(edited, b'Initial@example.org\tEdited\t \nsecondary@example.org\tEdited\t \n')
        generation = self.generation()
        self.assertEqual(self.run_writer('import', 'allow', 'SECONDARY@example.org', 'Imported').stdout,
                         b'present\n')
        self.assertEqual(self.generation(), generation)
        self.assertEqual(self.records()[0], (initial, 'Edited', 2))

    def test_conflicting_edit_and_delete_report_revision_and_keep_pending_values(self):
        initial = self.seed()['Initial']
        with self.held('draft', 'save', 'allow', initial, 'Pending') as (editor, proceed):
            self.run_writer('save', 'allow', initial, 'Winner', account='bob')
            proceed()
            output, log = editor.communicate(timeout=6)
            self.assertEqual(editor.returncode, 3, log)
            self.assertEqual(output, b'pending revision=1 name=Pending addresses=2\n')
            self.assertIn(b'contact revision has changed', log)
        with self.held('draft', 'delete', 'allow', initial) as (editor, proceed):
            self.run_writer('save', 'allow', initial, 'Newer', account='bob')
            proceed()
            _, log = editor.communicate(timeout=6)
            self.assertEqual(editor.returncode, 3, log)
        self.assertEqual(self.records()[0], (initial, 'Newer', 3))
        self.run_writer('save', 'allow', initial, 'Pending')
        self.assertEqual(self.records()[0], (initial, 'Pending', 4))

    def test_distinct_concurrent_edits_and_cancel(self):
        contacts = self.seed()
        neighbor = contacts['Neighbor']
        with self.held('draft', 'save', 'allow', neighbor, 'Other') as (editor, proceed):
            self.run_writer('save', 'allow', contacts['Initial'], 'First', account='bob')
            proceed()
            self.assertEqual(editor.communicate(timeout=6), (b'', b''))
            self.assertEqual(editor.returncode, 0)
        generation = self.generation()
        self.run_writer('cancel', 'allow', neighbor, 'Discarded', account='bob')
        self.assertEqual(self.generation(), generation)
        self.assertEqual([row[1:] for row in self.records()], [('First', 2), ('Other', 2)])

    def test_two_accounts_learn_once_and_share_memberships(self):
        mailbox = 'Shared@example.org'
        self.run_writer('member', 'allow', mailbox)
        with self.held('before_learn', 'learn', 'allow', mailbox, 'Alice') as (first, allow_first):
            with self.held('before_learn', 'learn', 'allow', mailbox.lower(), 'Bob',
                           account='bob') as (second, allow_second):
                allow_first()
                allow_second()
                results = [child.communicate(timeout=6) for child in (first, second)]
                self.assertEqual((first.returncode, second.returncode), (0, 0), results)
                self.assertEqual(sorted(results), [(b'new\n', b''), (b'present\n', b'')])
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(self.run_writer('import', 'block', mailbox, 'Bob', account='bob').stdout,
                         b'new\n')
        self.run_writer('learn', 'block-session', 'custom@example.org', 'Custom')
        contact_id = self.records()[0][0]
        self.run_writer('save', 'allow', contact_id, 'From Bob', account='bob')
        self.assertIn(b'\tFrom Bob\t', self.run_writer('query', 'allow', 'shared').stdout)
        for account in ('alice', 'bob'):
            for book in ('allow', 'block'):
                self.assertEqual(self.run_writer('member', book, mailbox, account=account).stdout,
                                 b'present\n')
            self.assertEqual(self.run_writer('member', 'block-session', mailbox, account=account).stdout,
                             b'absent\n')
        self.run_writer('delete', 'allow', contact_id, account='bob')
        self.assertEqual(self.run_writer('member', 'allow', mailbox).stdout, b'absent\n')
        self.assertEqual(self.run_writer('member', 'block', mailbox).stdout, b'present\n')
        isolated = self.root / 'Isolated profile'
        isolated.mkdir(mode=0o700)
        self.write_accounts(isolated)
        self.assertEqual(self.run_writer('member', 'block', mailbox, profile=isolated).stdout, b'absent\n')
        self.assertNotEqual(self.db.stat().st_ino, (isolated / DESTINATION).stat().st_ino)

    def test_preview_is_read_only_even_before_migration(self):
        source = self.profile / 'blacklist.abook'
        source.write_bytes(legacy('Legacy', 'sender@example.org'))
        before = source.read_bytes()
        self.assertEqual(self.run_writer('preview', 'block', 'Sender@example.org', 'Candidate').stdout,
                         b'present\n')
        self.assertEqual(self.run_writer('preview', 'allow', 'Sender@example.org', 'Candidate').stdout,
                         b'new\n')
        self.assertEqual(sorted(path.name for path in self.profile.iterdir()), ['Accounts', source.name])
        self.assertEqual(source.read_bytes(), before)
        source.write_bytes(b'malformed legacy source\n')
        self.run_writer('preview', 'block', 'sender@example.org', '', status=1)
        self.assertEqual(sorted(path.name for path in self.profile.iterdir()), ['Accounts', source.name])
        source.write_bytes(before)
        self.run_writer('member', 'block', 'sender@example.org')
        generation = self.generation()
        source.write_bytes(b'damaged archive after migration')
        self.run_writer('preview', 'missing', 'new@example.org', '')
        self.assertEqual(self.generation(), generation)
        with readonly(self.db) as db:
            self.assertEqual(db.execute('SELECT name FROM books').fetchall(), [('block',)])

    def test_busy_commit_is_bounded_and_preserves_draft_for_retry(self):
        initial = self.seed()['Initial']
        generation = self.generation()
        with closing(sqlite3.connect(self.db, timeout=2)) as reader:
            reader.execute('BEGIN')
            self.assertEqual(reader.execute('SELECT name FROM contacts WHERE id=?', (initial,)).fetchone(),
                             ('Initial',))
            start = time.monotonic()
            result = self.run_writer('save', 'allow', initial, 'Retry', status=4)
            self.assertLess(time.monotonic() - start, 4)
            self.assertIn(b'cannot commit contact write', result.stderr)
            self.assertEqual(result.stdout, b'pending revision=1 name=Retry addresses=2\n')
            reader.rollback()
        self.assertEqual(self.generation(), generation)
        self.run_writer('save', 'allow', initial, 'Retry')
        self.assertEqual(self.records()[0], (initial, 'Retry', 2))

    def test_disk_full_rolls_back_edit_and_retries(self):
        initial = self.seed()['Initial']
        generation = self.generation()
        result = self.run_writer('save', 'allow', initial, 'Failed commit', status=5, full=True)
        self.assertIn(b'fault: SQLITE_FULL on database write after a successful write', result.stderr)
        self.assertIn(b'native=13 cannot commit contact write', result.stderr)
        self.assertEqual(result.stdout, b'pending revision=1 name=Failed commit addresses=2\n')
        self.assertEqual(self.records()[0], (initial, 'Initial', 1))
        self.assertEqual(self.generation(), generation)
        self.run_writer('save', 'allow', initial, 'Failed commit')
        self.assertEqual(self.records()[0], (initial, 'Failed commit', 2))

    def test_termination_at_commit_boundaries(self):
        initial = self.seed()['Initial']
        generation = self.generation()
        for phase in ('before_commit', 'inside_commit', 'after_commit'):
            with self.subTest(phase=phase):
                with self.held(phase, 'save', 'allow', initial, 'Committed') as (editor, _):
                    journal = Path(str(self.db) + '-journal')
                    self.assertEqual(journal.exists(), phase != 'after_commit')
                    editor.kill()
                    editor.communicate(timeout=6)
                    self.assertEqual(editor.returncode, -signal.SIGKILL)
                # A writer first opens read-write to recover a hot rollback journal.
                self.run_writer('learn', 'allow', 'initial@example.org', 'Header')
                committed = phase == 'after_commit'
                self.assertEqual(self.records()[0],
                                 (initial, 'Committed' if committed else 'Initial', 2 if committed else 1))
                self.assertEqual(self.generation(), generation + committed)
                if phase != 'before_commit':
                    self.assertFalse(journal.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    args = parser.parse_args()
    MigrationTests.driver = (args.bin_dir / 'contact-migrate-driver').resolve(strict=True)
    WriterTests.driver = MigrationTests.driver
    unittest.main(argv=[__file__], verbosity=2)
