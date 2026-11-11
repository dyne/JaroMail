"""Isolated source + installed outbox CLI contracts. No live transport."""
from pathlib import Path
import hashlib
import json
import os
import re
import stat
import shutil
import subprocess
import sys
import unittest
from datetime import datetime, timezone

REPO, PREFIX, WORK = map(Path, sys.argv[1:4])
sys.argv[1:] = []
RUNTIME = PREFIX / 'share/jaromail'


def manifest(root):
    result = {}
    for path in sorted([root, *root.rglob('*')]):
        info = path.lstat()
        result[str(path.relative_to(root))] = (stat.S_IMODE(info.st_mode), info.st_mtime_ns,
            os.readlink(path) if path.is_symlink() else
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else 'directory')
    return result


class OutboxControls(unittest.TestCase):
    def setUp(self):
        self.root = WORK / self._testMethodName
        self.mail = self.root / 'mail with spaces'
        self.home = self.root / 'home'
        self.bin = self.root / 'bin'
        for path in (self.home, self.bin, *(self.mail/'outbox'/d for d in ('new','cur','tmp'))):
            path.mkdir(parents=True, exist_ok=True)
        self.calls = self.root/'smtp-calls'
        self.env = dict(os.environ, HOME=str(self.home), JAROMAILDIR=str(self.mail),
                        JAROWORKDIR=str(RUNTIME), PATH=str(self.bin)+os.pathsep+os.environ['PATH'],
                        JARO_TEST_CALLS=str(self.calls), JARO_TEST_MESSAGES=str(self.root),
                        LOG=str(self.root/'must-not-log'), JARO_KEYRING='pass')
        for name in ('gpg','pinentry','pass','secret-tool','openssl','fetchmail','mutt','neomutt'):
            p = self.bin/name
            p.write_text('#!/bin/sh\nprintf "%s\\n" "'+name+'" >> "$JARO_TEST_MESSAGES/forbidden-calls"\nexit 97\n')
            p.chmod(0o700)
        smtp = self.bin/'msmtp'
        smtp.write_text('''#!/bin/sh
printf 'called\n' >> "$JARO_TEST_CALLS"
number=$(wc -l < "$JARO_TEST_CALLS" | tr -d ' ')
cat > "$JARO_TEST_MESSAGES/transport-$number"
awk '$1 == "from" { print $2 }' "$2" > "$JARO_TEST_MESSAGES/from-$number"
exit "${JARO_TEST_SMTP_STATUS:-0}"
''')
        smtp.chmod(0o700)
        self.message = b'From: fixture@example.org\r\nTo: PRIVATE-RECIPIENT@example.org\r\nSubject: PRIVATE-SUBJECT\r\n\r\nPRIVATE-BODY\x00original\xff\r\n'

    def cli(self, *args, expected=0, data=None, installed=False):
        exe = PREFIX/'bin/jaro' if installed else REPO/'src/jaro'
        result = subprocess.run([str(exe), *args], input=data, capture_output=True, env=self.env, timeout=15)
        self.assertEqual(result.returncode, expected,
                         f'{args}: exit {result.returncode}, stderr={result.stderr.decode(errors="replace")[-800:]}')
        return result

    def helper(self, action, name, expected=0):
        result = subprocess.run([str(RUNTIME/'bin/jaro-outbox'), action, str(self.mail), name],
                                capture_output=True, env=self.env, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr.decode())
        return result.stdout.decode().split()

    def queued(self, name, held=True):
        (self.mail/'outbox/new'/name).write_bytes(self.message)
        if held:
            identity = self.helper('prepare', 'new/'+name)[0]
            self.helper('inflight', 'new/'+name)
            self.helper('uncertain', 'new/'+name)
        else:
            identity = self.helper('inspect', 'new/'+name)[0]
        return identity

    def configure(self):
        accounts = self.mail/'Accounts'
        accounts.mkdir()
        (accounts/'default').write_text('name Fixture\nemail fixture@example.org\nlogin fixture@example.org\nimap fixture.invalid\nsmtp fixture.invalid\npassword local-fixture\n')
        (self.mail/'Filters.txt').write_text('')
        (self.mail/'whitelist.abook').write_text('[format]\nversion=1\n')

    def smtp_count(self):
        return len(self.calls.read_text().splitlines()) if self.calls.exists() else 0

    def test_list_readonly_without_account_or_bootstrap_files(self):
        held = self.queued('PRIVATE-RECIPIENT@example.org')
        eligible = self.queued('eligible', held=False)
        before = manifest(self.root)
        for installed in (False, True):
            result = self.cli('outbox','list',installed=installed)
            lines = result.stdout.decode().splitlines()
            self.assertEqual(lines[0], 'ID STATE OUTCOME')
            rows = {parts[0]: parts[1:] for parts in map(str.split, lines[1:])}
            self.assertEqual(rows[held], ['uncertain','transport-unknown'])
            self.assertEqual(rows[eligible], ['eligible','no-attempt'])
            self.assertNotRegex(result.stdout+result.stderr, b'PRIVATE-(?:SUBJECT|BODY|RECIPIENT)')
            self.assertEqual(manifest(self.root), before)
        self.assertFalse((self.root/'forbidden-calls').exists())
        self.assertEqual(self.smtp_count(), 0)

    def test_list_without_mail_dependencies_on_path(self):
        self.queued('held')
        minimal = self.root/'minimal-path'
        minimal.mkdir()
        for tool in ('zsh','dirname'):
            (minimal/tool).symlink_to(shutil.which(tool))
        self.env['PATH'] = str(minimal)
        before = manifest(self.root)
        self.cli('outbox','list')
        self.cli('outbox','list',installed=True)
        self.assertEqual(manifest(self.root), before)

    def test_list_corrupt_record_is_readonly_and_classified(self):
        identity = self.queued('held')
        (self.mail/'.outbox-state'/identity/'state').write_text('broken\n')
        before = manifest(self.root)
        result = self.cli('outbox','list',expected=1)
        self.assertIn(f'{identity} held invalid-state-or-content'.encode(), result.stdout)
        self.assertEqual(manifest(self.root), before)

    def test_missing_mail_root_never_initializes(self):
        self.env['JAROMAILDIR'] = str(self.root/'absent')
        before = manifest(self.root)
        self.cli('outbox','list',expected=1)
        self.assertEqual(manifest(self.root), before)

    def test_retry_requires_duplicate_confirmation_without_mutations(self):
        identity = self.queued('held')
        before = manifest(self.root)
        result = self.cli('outbox','retry',identity,expected=2)
        self.assertIn(b'duplicate mail', result.stderr)
        self.assertIn(b'--confirm-duplicate', result.stderr)
        self.assertEqual(manifest(self.root), before)
        self.assertEqual(self.smtp_count(), 0)

    def test_dry_runs_are_readonly(self):
        identity = self.queued('held')
        before = manifest(self.root)
        for args in (('-n','outbox','retry',identity,'--confirm-duplicate'),
                     ('outbox','archive',identity,'-n')):
            self.assertIn(b'dry-run:', self.cli(*args).stdout)
            self.assertEqual(manifest(self.root), before)
        self.assertEqual(self.smtp_count(), 0)

    def test_invalid_unknown_and_stale_ids_never_mutate(self):
        identity = self.queued('held')
        for invalid, code in (('../'+identity,2), ('/tmp/x',2), ('',2), ('A'*64,2), ('0'*64,1)):
            for action in ('retry','archive'):
                before = manifest(self.root)
                args = ['outbox', action, invalid]
                if action == 'retry': args += ['--confirm-duplicate']
                self.cli(*args, expected=code)
                self.assertEqual(manifest(self.root), before)
        (self.mail/'outbox/new/held').unlink()
        before = manifest(self.root)
        self.cli('outbox','archive',identity,expected=1)
        self.assertEqual(manifest(self.root), before)

    def test_changed_content_refuses_both_actions(self):
        identity = self.queued('held')
        (self.mail/'outbox/new/held').write_bytes(b'changed')
        before = manifest(self.root)
        self.cli('outbox','archive',identity,expected=1)
        self.cli('outbox','retry',identity,'--confirm-duplicate',expected=1)
        self.assertEqual(manifest(self.root), before)
        self.assertEqual(self.smtp_count(), 0)

    def test_retry_selection_and_mixed_queue(self):
        self.configure()
        first, second = self.queued('first'), self.queued('second')
        eligible = self.queued('eligible', held=False)
        self.cli('outbox','retry',first,'--confirm-duplicate')
        self.assertEqual(self.smtp_count(), 1)
        self.assertEqual((self.root/'transport-1').read_bytes(), self.message)
        self.assertFalse((self.mail/'outbox/new/first').exists())
        self.assertTrue((self.mail/'outbox/new/second').exists())
        self.assertTrue((self.mail/'outbox/new/eligible').exists())
        result = self.cli('send',expected=1)
        self.assertIn(b'1 outbox item(s) held', result.stderr)
        self.assertEqual(self.smtp_count(), 2)
        self.assertEqual((self.root/'transport-2').read_bytes(), self.message)
        self.cli('outbox','archive',second)
        self.assertEqual(self.smtp_count(), 2)
        self.assertEqual(len(list((self.mail/'sent/new').iterdir())), 3)
        for copy in (self.mail/'sent/new').iterdir(): self.assertEqual(copy.read_bytes(), self.message)
        self.assertEqual(len(self.cli('outbox','list').stdout.splitlines()), 1)
        before = manifest(self.root)
        self.cli('outbox','retry',first,'--confirm-duplicate',expected=1)
        self.assertEqual(manifest(self.root), before)

    def test_selected_account_and_repeated_uncertain_outcome(self):
        self.configure()
        (self.mail/'Accounts/alternate').write_text('name Alternate\nemail alternate@example.org\nlogin alternate@example.org\nimap fixture.invalid\nsmtp fixture.invalid\npassword fixture\n')
        identity = self.queued('held')
        self.env['JARO_TEST_SMTP_STATUS'] = '69'
        self.cli('-a','alternate','outbox','retry',identity,'--confirm-duplicate',expected=69)
        self.assertEqual((self.root/'from-1').read_text().strip(), 'alternate@example.org')
        self.assertEqual(self.smtp_count(), 1)
        self.assertIn(b'uncertain transport-unknown', self.cli('outbox','list').stdout)
        self.cli('send',expected=1)
        self.assertEqual(self.smtp_count(), 1)

    def test_eligible_and_ambiguous_selections_are_rejected(self):
        eligible = self.queued('eligible', held=False)
        before = manifest(self.root)
        self.cli('outbox','retry',eligible,'--confirm-duplicate',expected=1)
        self.cli('outbox','archive',eligible,expected=1)
        self.assertEqual(manifest(self.root), before)
        identity = self.queued('held')
        (self.mail/'outbox/cur/held:2,S').write_bytes(self.message)
        before = manifest(self.root)
        self.cli('outbox','retry',identity,'--confirm-duplicate',expected=1)
        self.cli('outbox','archive',identity,expected=1)
        self.assertEqual(manifest(self.root), before)

    def test_archive_only_has_zero_transport_or_secret_calls(self):
        identity = self.queued('held')
        self.cli('outbox','archive',identity,installed=True)
        self.assertEqual(self.smtp_count(), 0)
        self.assertFalse((self.root/'forbidden-calls').exists())
        copies = list((self.mail/'sent/new').iterdir())
        self.assertEqual(len(copies), 1)
        self.assertEqual(copies[0].read_bytes(), self.message)
        self.assertFalse((self.home/'.gnupg').exists())
        self.assertFalse((self.mail/'Filters.txt').exists())

    def test_archive_collision_preserves_queue_and_existing_sent(self):
        identity = self.queued('held')
        dest = self.mail/'sent/new'
        dest.mkdir(parents=True)
        conflict = dest/('jaro-outbox-'+identity)
        conflict.write_bytes(b'keep-existing')
        self.cli('outbox','archive',identity,expected=1)
        self.assertEqual(conflict.read_bytes(), b'keep-existing')
        self.assertEqual((self.mail/'outbox/new/held').read_bytes(), self.message)
        self.assertEqual(self.smtp_count(), 0)
        # Verification is persisted before archive publication; ordinary send
        # must not transport this externally confirmed message after the error.
        self.assertIn(b'verified externally-verified', self.cli('outbox','list').stdout)

    def test_queue_stdin_semantics_and_outbox_argument(self):
        self.configure()
        data = b'From: fixture@example.org\nTo: queued@example.org\nSubject: stdin\n\nqueue body\n'
        self.cli('queue','outbox',data=data)
        queued = list((self.mail/'outbox/new').iterdir())
        self.assertEqual(len(queued), 1)
        self.assertEqual(queued[0].read_bytes(), data)
        self.assertEqual(self.smtp_count(), 0)

    def test_help_completions_and_invalid_extra_arguments(self):
        text = self.cli('outbox','--help').stderr
        self.assertIn(b'--confirm-duplicate', text)
        self.assertIn(b'outbox archive ID', text)
        before = manifest(self.root)
        for args in (('outbox','list','extra'), ('outbox','unknown'), ('outbox','archive','0'*64,'--confirm-duplicate')):
            self.cli(*args,expected=2)
            self.assertEqual(manifest(self.root), before)
        bash = subprocess.run(['bash','-c', 'source "$1"; COMP_WORDS=(jaro outbox ""); COMP_CWORD=2; _jaro; printf "%s\\n" "${COMPREPLY[@]}"',
                               'fixture', str(REPO/'extras/shell_completion/jaromail.bash')], capture_output=True, timeout=5)
        self.assertEqual(bash.returncode, 0)
        self.assertEqual(set(bash.stdout.decode().splitlines()), {'list','retry','archive'})
        zsh = subprocess.run(['zsh','-c', 'curcontext=""; words=(jaro outbox ""); CURRENT=3; compadd() { print -l -- "$@"; }; source "$1"',
                              'fixture', str(REPO/'extras/shell_completion/_jaromail')], capture_output=True, timeout=5)
        self.assertEqual(zsh.returncode, 0)
        self.assertEqual(set(zsh.stdout.decode().splitlines()), {'list','retry','archive'})
        bash = subprocess.run(['bash','-c', 'source "$1"; COMP_WORDS=(jaro outbox retry fixture ""); COMP_CWORD=4; _jaro; printf "%s\\n" "${COMPREPLY[@]}"',
                               'fixture', str(REPO/'extras/shell_completion/jaromail.bash')], capture_output=True, timeout=5)
        self.assertEqual(bash.stdout.decode().splitlines(), ['--confirm-duplicate'])
        zsh = subprocess.run(['zsh','-c', 'curcontext=""; words=(jaro outbox retry fixture ""); CURRENT=5; compadd() { [[ "$1" == -- ]] && shift; print -l -- "$@"; }; source "$1"',
                              'fixture', str(REPO/'extras/shell_completion/_jaromail')], capture_output=True, timeout=5)
        self.assertEqual(zsh.stdout.decode().splitlines(), ['--confirm-duplicate'])


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OutboxControls)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print('Outbox CLI tests: '+json.dumps({'executed':result.testsRun,'failed':len(result.failures)+len(result.errors),
          'skipped':len(result.skipped),'observedAt':datetime.now(timezone.utc).isoformat()}))
    sys.exit(not result.wasSuccessful())
