"""Run the ranked preview/apply/retry contract on isolated Maildirs."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile


def quote(path):
    return b'"' + b''.join(bytes([b]) if 32 <= b < 127 and b not in (34, 92)
                           else f'\\x{b:02x}'.encode() for b in os.fsencode(path)) + b'"'


def fixture(folder):
    for sub in ('cur', 'new', 'tmp'):
        (folder / sub).mkdir(parents=True)
    return folder


def state(root):
    files = {}
    for p in root.rglob('*'):
        s = p.stat()
        files[p.relative_to(root)] = (s.st_ino, s.st_mode, s.st_nlink, s.st_mtime_ns,
                                      s.st_ctime_ns, p.read_bytes() if p.is_file() else None)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    kind = parser.add_mutually_exclusive_group(required=True)
    kind.add_argument('--cli', type=Path)
    kind.add_argument('--helper', type=Path)
    parser.add_argument('--emulator')
    args = parser.parse_args()
    exe = (args.cli or args.helper).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='dedup-cli-', dir=os.environ.get('JARO_DEDUP_TEST_TMP')) as tmp:
        base = Path(tmp)
        home = base / 'home'
        home.mkdir()
        mail = base / 'mail root'
        scratch = base / 'scratch space'
        scratch.mkdir()
        env = dict(os.environ, HOME=str(home), JAROMAILDIR=str(mail), TMPDIR=str(scratch))
        env.pop('JAROWORKDIR', None)

        def invoke(*roots, dry=False, quiet=False, extra=(), ok=True):
            if args.cli:
                cmd = [exe] + (['-n'] if dry else []) + (['-q'] if quiet else []) + ['dedup']
            else:
                cmd = ([args.emulator] if args.emulator else []) + [exe, '--preview' if dry else '--apply']
                if quiet:
                    cmd.append('--quiet')
            cmd += [*extra, '--', *roots]
            p = subprocess.run(cmd, cwd=base, env=env, capture_output=True, timeout=30)
            assert (p.returncode == 0) == ok, (cmd, p.returncode, p.stderr)
            return p

        primary = fixture(mail / 'Primary')
        archive = fixture(mail / 'Archive')
        dash = fixture(mail / '- folder\n\udcff')
        keep = primary / 'cur/quote"\\:2,FS'
        early = primary / 'new/earlier name'
        later = archive / 'cur/a'
        last = dash / 'new/\udcfe\n'
        payload = b'Message-ID: <same>\r\n\r\n\x00\xffbody\r\n'
        keep.write_bytes(payload)
        keep.chmod(0o640)
        early.write_bytes(payload)
        later.write_bytes(payload)
        os.link(later, last)
        different = archive / 'new/same-id'
        different.write_bytes(payload.replace(b'body', b'BODY'))
        original_inode, original_mode = keep.stat().st_ino, keep.stat().st_mode
        roots = ('Primary', 'Archive', dash.name, 'Primary')
        before = state(mail)
        preview = invoke(*roots, dry=True, extra=('--scratch-dir', scratch))
        lines = [b'keep ' + quote(keep) + b' remove ' + quote(p) for p in (early, later, last)]
        assert preview.stdout.splitlines() == lines, preview.stdout
        assert b'examined=5 groups=1 proposed=3 removed=0 conflicts=0' in preview.stderr
        assert state(mail) == before and not list(scratch.iterdir())
        quiet = invoke(*roots, dry=True, quiet=True)
        assert quiet.stdout == preview.stdout and not quiet.stderr
        print('PASS ranked preview, exact raw path escaping, unchanged Maildirs and quiet output')

        for invalid in ((), ('Primary', ''), ('Primary', 'missing')):
            failure = invoke(*invalid, quiet=True, ok=False)
            assert failure.stderr and state(mail) == before
        nested = invoke(*roots, dry=True, extra=('--scratch-dir', primary / 'cur'), ok=False)
        assert b'outside the input Maildirs' in nested.stderr and state(mail) == before
        if args.cli:
            help_result = invoke(extra=('--help',))
            assert b'Usage: jaro [-n] [-q] dedup' in help_result.stdout
        print('PASS usage and invalid operands preserve all input files')

        fd = os.open(primary / 'new', os.O_RDONLY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            busy = invoke(*roots, quiet=True, ok=False)
            assert b'acquire apply lock' in busy.stderr and b'removed=0' in busy.stderr
            assert state(mail) == before
        finally:
            os.close(fd)
        print('PASS busy physical apply lock propagates failure without removal')

        last_block_bytes = (early.stat().st_blocks + later.stat().st_blocks) * 512
        applied = invoke(*roots)
        assert not applied.stdout
        assert b'examined=5 groups=1 removed=3 conflicts=0' in applied.stderr
        assert f'logical-bytes={3 * len(payload)} last-link-block-bytes={last_block_bytes}'.encode() in applied.stderr
        assert all(not p.exists() for p in (early, later, last))
        assert (keep.read_bytes(), keep.stat().st_ino, keep.stat().st_mode) == (payload, original_inode, original_mode)
        assert different.read_bytes() == payload.replace(b'body', b'BODY')
        assert set(state(mail)) == set(before) - {p.relative_to(mail) for p in (early, later, last)}
        after = state(mail)
        repeat = invoke(*roots)
        assert b'examined=2 groups=0 removed=0 conflicts=0' in repeat.stderr
        assert not invoke(*roots, quiet=True).stderr and state(mail) == after and not list(scratch.iterdir())
        print('PASS apply, logical and last-link byte counts, original winner and no-op retry')

        literal = fixture(mail / 'lastlog')
        (literal / 'cur/a').write_bytes(b'literal')
        (literal / 'new/b').write_bytes(b'literal')
        assert b'removed=1 conflicts=0' in invoke('lastlog').stderr
        assert (literal / 'cur/a').exists() and not (literal / 'new/b').exists()
        print('PASS lastlog is an ordinary dedup operand')

        if os.geteuid() != 0:
            first = fixture(mail / 'first-partial')
            second = fixture(mail / 'second-partial')
            for root, body in ((first, b'A'), (second, b'BB')):
                (root / 'cur/keep').write_bytes(body)
                (root / 'new/remove').write_bytes(body)
            (second / 'new').chmod(0o555)
            try:
                partial = invoke(first, second, quiet=True, ok=False)
                assert b'removed=1 conflicts=1' in partial.stderr, partial.stderr
                assert not (first / 'new/remove').exists() and (second / 'new/remove').read_bytes() == b'BB'
            finally:
                (second / 'new').chmod(0o700)
            assert b'removed=1 conflicts=0' in invoke(first, second).stderr
            print('PASS partial filesystem denial, quiet diagnostics, retained bytes and retry')


if __name__ == '__main__':
    main()
