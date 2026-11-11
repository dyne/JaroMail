#!/usr/bin/env zsh
# The fixture's entire HOME/mail/runtime trees are manifested around each call.
set -euo pipefail
repo_root="${0:A:h:h:h}"
command -v python3 >/dev/null || exit 77
[[ -x "$repo_root/build/gnu/jaro-outbox" ]] || exit 77
python3 - "$repo_root" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

repo = Path(sys.argv[1])
zsh = shutil.which('zsh')
required = 'fetchmail msmtp gpg pinentry notmuch maddr mdeliver mpick openssl awk sed grep find hostname sqlite3 neomutt'.split()
optional = 'pass secret-tool wipe srm alot mixmaster nc tomb gnome-keyring-daemon'.split()
helpers = 'fetchaddr parsedate dotlock gpgewrap abook jaro-filter-compile jaro-filter-legacy jaro-filter'.split()
cases = 0

def manifest(root):
    result = {}
    for path in [root, *sorted(root.rglob('*'))]:
        info = path.lstat()
        kind = stat.S_IFMT(info.st_mode)
        digest = (os.readlink(path) if path.is_symlink() else
                  hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else '')
        result[str(path.relative_to(root))] = (kind, stat.S_IMODE(info.st_mode), info.st_size, info.st_mtime_ns, digest)
    return result

with tempfile.TemporaryDirectory(prefix='jaro-doctor-', dir=os.environ.get('TMPDIR')) as temp:
    base = Path(temp)

    def fixture(label):
        root = base / label
        home, mail, runtime, bins = (root / x for x in ('home', 'mail', 'runtime', 'bin'))
        for path in (home, mail, runtime / 'zlibs', runtime / 'bin', bins):
            path.mkdir(parents=True, exist_ok=True)
        for name in ('bootstrap', 'doctor', 'accounts', 'version'):
            shutil.copyfile(repo / 'src/zlibs' / name, runtime / 'zlibs' / name)
        (runtime / 'zlibs/zuper').touch()
        (runtime / 'VERSION').write_text('6.0.0\n')
        shutil.copyfile(repo / 'src/jaro', runtime / 'bin/jaro')
        (runtime / 'bin/jaro').chmod(0o700)
        shutil.copyfile(repo / 'build/gnu/jaro-outbox', runtime / 'bin/jaro-outbox')
        (runtime / 'bin/jaro-outbox').chmod(0o700)
        (bins / 'zsh').symlink_to(zsh)
        # Every dependency would leave a trace if doctor accidentally invoked it.
        stub = '#!/bin/sh\nprintf "%s\\n" "$0 $*" >> "$CALL_LOG"\nexit 91\n'
        for name in required + optional:
            (bins / name).write_text(stub)
            (bins / name).chmod(0o700)
        for name in helpers:
            (runtime / 'bin' / name).write_text(stub)
            (runtime / 'bin' / name).chmod(0o700)
        accounts = mail / 'Accounts'
        accounts.mkdir()
        (accounts / 'default').write_text('email fixture@example.org\nlogin fixture\nimap imap.example.org\nsmtp smtp.example.org\ncert check\n')
        for name in ('incoming', 'outbox'):
            for part in ('new', 'cur', 'tmp'):
                (mail / name / part).mkdir(parents=True)
        (mail / 'Filters.txt').write_text('# explicit policy\n')
        (home / '.password-store').mkdir()
        (home / '.password-store/.gpg-id').write_text('PRIVATE-KEY-ID\n')
        env = {'HOME': str(home), 'PATH': str(bins), 'JAROWORKDIR': str(runtime),
               'JAROMAILDIR': str(mail), 'JARO_KEYRING': 'pass', 'CALL_LOG': str(root / 'calls'),
               'LC_ALL': 'C'}
        return root, home, mail, runtime, bins, accounts, env

    def run(f, expected=0, args=('doctor',), includes=(), entry=None):
        global cases
        root, home, mail, runtime, bins, accounts, env = f
        before = manifest(root)
        p = subprocess.run([zsh, str(entry or runtime / 'bin/jaro'), *args], env=env,
                           cwd=root, text=True, capture_output=True, timeout=15)
        assert p.returncode == expected, (root.name, p.returncode, p.stdout, p.stderr)
        for needle in includes:
            assert needle in p.stdout, (root.name, needle, p.stdout)
        assert before == manifest(root), (root.name, 'doctor mutated fixture')
        assert not (root / 'calls').exists(), (root.name, 'dependency executed')
        assert p.stderr == '', (root.name, p.stderr)
        cases += 1
        digest = hashlib.sha256(json.dumps(before, sort_keys=True).encode()).hexdigest()
        print(f'PASS case: {root.name}; unchanged manifest={digest}; stub calls=0')
        return p.stdout

    expected_ids = {'paths', 'runtime', 'version', 'mail.root', 'maildirs', 'account', 'trust', 'keyring', 'filters', 'index', 'outbox'}
    expected_ids |= {'command.' + name for name in ['zsh', *required] if name != 'neomutt'} | {'command.mutt'}
    expected_ids |= {'optional.' + name for name in optional if name != 'gnome-keyring-daemon'}
    expected_ids |= {'helper.' + name for name in [*helpers, 'jaro-outbox']}

    def structured(f, expected=0, args=('doctor', '--json'), entry=None, complete=True):
        output = run(f, expected, args=args, entry=entry)
        document = json.loads(output)
        assert set(document) == {'schema_version', 'paths', 'checks', 'exit_status'}
        assert document['schema_version'] == 1 and document['exit_status'] == expected
        assert type(document['exit_status']) is int
        checks = document['checks']
        assert isinstance(checks, list) and checks
        ids = [check['id'] for check in checks]
        assert len(ids) == len(set(ids)), ids
        if complete:
            assert set(ids) == expected_ids, (set(ids) ^ expected_ids)
            assert set(document['paths']) == {'runtime', 'mail', 'accounts', 'account'}
            assert all(isinstance(value, str) for value in document['paths'].values())
        for check in checks:
            assert set(check) == {'id', 'status', 'explanation', 'next_action'}, check
            assert all(isinstance(value, str) for value in check.values()), check
            assert check['status'] in {'ok', 'warning', 'blocker', 'unknown'}, check
            assert check['explanation']
        assert (expected != 0) == any(check['status'] == 'blocker' for check in checks)
        return document, {check['id']: check for check in checks}

    f = fixture('configured')
    run(f, includes=('ok [account]', 'ok [keyring]', 'eligible=0 held=0 local-recovery=0 invalid=0', 'Version: 6.0.0'))
    f = fixture('configured-json')
    document, checks = structured(f)
    assert len(checks) == 45, checks.keys()
    assert checks['trust']['status'] == 'unknown' and checks['index']['status'] == 'warning'
    # Both renderers carry exactly the same findings and suggested actions.
    human = run(f)
    for check in document['checks']:
        assert f"{check['status']} [{check['id']}] {check['explanation']}" in human
        if check['next_action']: assert '  Next: ' + check['next_action'] in human
    f = fixture('empty-home-absent-mail')
    shutil.rmtree(f[1]); f[1].mkdir()
    shutil.rmtree(f[2])
    run(f, 1, includes=('blocker [mail.root]', 'blocker [account]', 'blocker [filters]', 'blocker [keyring]', 'unknown [outbox]'))
    f = fixture('empty-home-default-path')
    shutil.rmtree(f[1]); f[1].mkdir()
    del f[6]['JAROMAILDIR']
    run(f, 1, includes=(str(f[0]), 'blocker [account]'))
    f = fixture('incomplete-runtime')
    (f[3] / 'zlibs/accounts').unlink()
    (f[3] / 'zlibs/zuper').unlink()
    (f[3] / 'VERSION').unlink()
    (f[3] / 'bin/jaro-filter').unlink()
    run(f, 1, includes=('blocker [runtime]', 'blocker [version]', 'blocker [helper.jaro-filter]', 'blocker [account]', 'unknown [trust]'))
    f = fixture('all-dependencies-absent')
    for path in f[4].iterdir():
        if path.name != 'zsh': path.unlink()
    f[6]['JARO_KEYRING'] = ''
    out = run(f, 1, includes=('blocker [command.fetchmail]', 'blocker [command.msmtp]', 'warning [optional.alot]', 'ok [account]', 'unknown [keyring]'))
    assert out.count('blocker [command.') == 16, out
    f = fixture('optional-only-absent')
    for name in optional: (f[4] / name).unlink()
    f[6]['JARO_KEYRING'] = ''
    run(f, includes=('warning [optional.pass]', 'unknown [keyring]'))
    f = fixture('malformed-account')
    (f[5] / 'default').write_text('password MARKER-SECRET\nimap "malformed\n')
    out = run(f, 1, includes=('blocker [account]', 'unknown [trust]', 'ok [keyring]', '[filters]', '[index]', '[outbox]'))
    assert 'MARKER-SECRET' not in out
    f = fixture('missing-ca')
    with (f[5] / 'default').open('a') as file: file.write('certfile /missing/MARKER-SECRET\n')
    out = run(f, 1, includes=('blocker [trust]', 'path redacted'))
    assert 'MARKER-SECRET' not in out
    f = fixture('unreadable-maildir')
    (f[2] / 'incoming/new').chmod(0)
    # The suite runs as an ordinary user: mode bits must affect access checks.
    assert not os.access(f[2] / 'incoming/new', os.R_OK)
    run(f, 1, includes=('blocker [maildirs]',))
    (f[2] / 'incoming/new').chmod(0o700)
    f = fixture('unsearchable-maildir-parent')
    (f[2] / 'incoming').chmod(0)
    run(f, 1, includes=('blocker [maildirs]',))
    (f[2] / 'incoming').chmod(0o700)
    f = fixture('custom-paths')
    custom = f[0] / 'custom accounts [literal]'
    f[5].rename(custom)
    (custom / 'default').rename(custom / 'personal.txt')
    f[6]['JAROACCOUNT'] = str(custom)
    run(f, args=('-a', 'personal', 'doctor'), includes=(str(custom), 'ok [account]'))
    f = fixture('corrupt-outbox-state')
    msg = f[2] / 'outbox/new/one'
    msg.write_text('Subject: MARKER-SECRET\n\nMARKER-BODY\n')
    helper = f[3] / 'bin/jaro-outbox'
    p = subprocess.run([str(helper), 'prepare', str(f[2]), 'new/one'], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    identity = p.stdout.split()[0]
    (f[2] / '.outbox-state' / identity / 'state').write_text('corrupt\n')
    out = run(f, 1, includes=('blocker [outbox]', 'held=0 local-recovery=0 invalid=1'))
    assert 'MARKER-SECRET' not in out and 'MARKER-BODY' not in out and identity not in out
    f = fixture('held-outbox')
    (f[2] / 'outbox/new/one').write_text('Subject: private\n\nprivate\n')
    helper = f[3] / 'bin/jaro-outbox'
    for op in ('prepare', 'inflight'):
        p = subprocess.run([str(helper), op, str(f[2]), 'new/one'], capture_output=True)
        assert p.returncode == 0, p.stderr
    run(f, 1, includes=('held=1 local-recovery=0 invalid=0',))
    for args in [('doctor', 'extra'), ('doctor', '--unknown'), ('-a', '', 'doctor'), ('-a', '../escape', 'doctor')]:
        f = fixture('invalid-' + str(cases))
        run(f, 2, args=args, includes=('Usage:',))
    f = fixture('help')
    run(f, args=('doctor', '--help'), includes=('Usage:',))
    f = fixture('missing-doctor-module')
    (f[3] / 'zlibs/doctor').unlink()
    before = manifest(f[0])
    p = subprocess.run([zsh, str(f[3] / 'bin/jaro'), 'doctor'], env=f[6], cwd=f[0], capture_output=True, text=True)
    assert p.returncode == 2 and 'module missing' in p.stderr
    assert before == manifest(f[0])
    cases += 1
    print('PASS case: missing-doctor-module')

    f = fixture('multiple-failures-json')
    for name in required: (f[4] / name).unlink()
    for name in optional: (f[4] / name).unlink()
    (f[3] / 'VERSION').unlink()
    (f[3] / 'bin/jaro-filter').unlink()
    (f[5] / 'default').write_text('MARKER-UNKNOWN-DIRECTIVE MARKER-SECRET\n')
    (f[2] / 'Filters.txt').unlink()
    document, checks = structured(f, 1)
    for name in ('command.fetchmail', 'command.msmtp', 'command.mutt', 'version', 'helper.jaro-filter', 'account', 'filters', 'keyring'):
        assert checks[name]['status'] == 'blocker', (name, checks[name])
    assert checks['outbox']['status'] == 'ok' and checks['trust']['status'] == 'unknown'
    assert 'MARKER-' not in json.dumps(document)

    f = fixture('secrets-redacted-both-modes')
    (f[5] / 'default').write_text('name MARKER-NAME\nemail MARKER-EMAIL\nlogin MARKER-LOGIN\npassword MARKER-PASSWORD\nimap MARKER-IMAP.invalid\nsmtp MARKER-SMTP.invalid\ncert check\ncertfile /missing/MARKER-CA\nfolders MARKER-FOLDER\nmy_hdr From: MARKER-HEADER\n')
    (f[2] / 'outbox/new/MARKER-QUEUE-NAME').write_text('To: MARKER-RECIPIENT\nSubject: MARKER-SUBJECT\n\nMARKER-BODY\n')
    for args in (('doctor',), ('doctor', '--json'), ('-D', 'doctor', '--json')):
        output = run(f, 1, args=args)
        assert 'MARKER-' not in output, output
        if '--json' in args: assert json.loads(output)['exit_status'] == 1

    f = list(fixture('special-characters'))
    unusual = ' "quoted" \\ λ' + ''.join(chr(i) for i in range(1, 32)) + '\x7f'
    old_mail, old_runtime = f[2], f[3]
    f[2] = f[0] / ('mail' + unusual)
    f[3] = f[0] / ('runtime' + unusual)
    old_mail.rename(f[2]); old_runtime.rename(f[3])
    f[5] = f[2] / ('accounts' + unusual)
    (f[2] / 'Accounts').rename(f[5])
    f[6].update(JAROMAILDIR=str(f[2]), JAROWORKDIR=str(f[3]), JAROACCOUNT=str(f[5]))
    document, checks = structured(f)
    assert document['paths'] == {'runtime': str(f[3]), 'mail': str(f[2]), 'accounts': str(f[5]), 'account': str(f[5] / 'default')}
    human = run(f)
    assert not any(chr(i) in human for i in [*range(32), 127] if i != 10), repr(human)

    f = fixture('optional-missing-json')
    for name in optional: (f[4] / name).unlink()
    f[6]['JARO_KEYRING'] = ''
    document, checks = structured(f)
    assert checks['optional.pass']['status'] == 'warning' and checks['keyring']['status'] == 'unknown'

    for args in [('doctor', '--json', 'MARKER-INVALID-ARG'), ('doctor', 'MARKER-INVALID-ARG', '--json'),
                 ('-a', '', 'doctor', '--json'), ('-a', '../escape', 'doctor', '--json')]:
        f = fixture('invalid-json-' + str(cases))
        document, checks = structured(f, 2, args=args, complete=False)
        assert set(checks) == {'invocation'} and document['paths'] is None
        assert 'MARKER-' not in json.dumps(document)
    f = fixture('internal-failure-json')
    (f[3] / 'zlibs/bootstrap').write_text('return 1\n')
    document, checks = structured(f, 2, complete=False)
    assert set(checks) == {'diagnostic'} and document['paths'] is None

    f = fixture('actual-source-json')
    f[6]['JAROWORKDIR'] = str(repo / 'src')
    document, checks = structured(f, entry=repo / 'src/jaro')
    assert document['paths']['runtime'] == str(repo / 'src')

    f = fixture('actual-staged-install-json')
    prefix = f[0] / 'installed prefix'
    install_env = dict(os.environ, DESTDIR='')
    staged = subprocess.run(['cmake', '--install', str(repo / 'build/gnu/cmake'), '--prefix', str(prefix)],
                            env=install_env, capture_output=True, text=True, timeout=60)
    assert staged.returncode == 0, staged.stdout + staged.stderr
    wrapper = prefix / 'bin/jaro'
    document, checks = structured(f, entry=wrapper)
    assert document['paths']['runtime'] == str(prefix / 'share/jaromail')
    assert all(check['status'] == 'ok' for name, check in checks.items() if name.startswith('helper.'))
    run(f, entry=wrapper, includes=('ok [runtime]', 'ok [account]'))

    bash = shutil.which('bash')
    complete = subprocess.run([bash, '-c', 'source "$1"; COMP_WORDS=(jaro -a personal doctor --j); COMP_CWORD=4; _jaro; [[ ${COMPREPLY[*]} == --json ]]',
                               'test-completion', str(repo / 'extras/shell_completion/jaromail.bash')], capture_output=True, text=True)
    assert complete.returncode == 0, complete.stderr
    cases += 1
    print('PASS case: bash-doctor-completion')
    complete = subprocess.run([zsh, '-c', 'words=(jaro -a personal doctor --j); CURRENT=5; compadd() { print -rl -- "$@"; }; source "$1"',
                               'test-completion', str(repo / 'extras/shell_completion/_jaromail')], capture_output=True, text=True)
    assert complete.returncode == 0 and '--json' in complete.stdout.splitlines(), complete.stderr
    cases += 1
    print('PASS case: zsh-doctor-completion')
print(f'Doctor cases: {cases} passed, 0 failed, 0 skipped')
PY
