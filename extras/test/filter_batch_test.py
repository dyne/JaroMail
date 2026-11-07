"""Decision-only batch contract, authored expectations and pinned Sieve oracle."""
from datetime import datetime, timezone
from pathlib import Path
import errno
import fcntl
import hashlib
import json
import os
import select
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import contextmanager
sys.dont_write_bytecode = True
import filter_native_test as authored
from filter_compiler_test import records
from sieve_oracle import SieveOracle

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / 'extras/test/fixtures/filter-policy'
CATALOGUE = REPO / 'docs/filter-regex-qualification.json'
PROFILE = ('version 1\nprofile pigeonhole-2.4.5-posix-ere-v1\ncapability fileinto\n'
           'capability mailbox\ncapability regex\nregex-qualification ' +
           hashlib.sha256(CATALOGUE.read_bytes()).hexdigest() + '\n')
FIELDS = 'index path rule_id action logical local sieve status source line detail'.split()


def tree(path):
    """Complete raw-path/mode/type/content snapshot; reads may update atime."""
    base = os.fsencode(path)
    st = os.lstat(base)
    result = {b'.': (stat.S_IFMT(st.st_mode),stat.S_IMODE(st.st_mode),b'')}
    for parent, directories, files in os.walk(base, followlinks=False):
        for name in directories + files:
            full = os.path.join(parent, name)
            st = os.lstat(full)
            payload = os.readlink(full) if stat.S_ISLNK(st.st_mode) else (
                hashlib.sha256(Path(os.fsdecode(full)).read_bytes()).digest() if stat.S_ISREG(st.st_mode) else b'')
            result[os.path.relpath(full, base)] = (stat.S_IFMT(st.st_mode), stat.S_IMODE(st.st_mode), payload)
    return result


class BatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get('JARO_FILTER_BATCH_WORK')
        cls.temp = None
        if retained:
            cls.work = Path(retained)
            assert cls.work.is_absolute()
            cls.work.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            cls.temp = tempfile.TemporaryDirectory(prefix='jaro-batch-', dir=os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR'))
            cls.work = Path(cls.temp.name)
        cls.objects = cls.work / 'objects'
        subprocess.run(['make', '-f', 'build/filter-core.mk', '-j2', 'filter-compile', 'filter-compiler-driver',
                        'filter-batch-tools',
                        'FILTER_BUILD_DIR=' + str(cls.objects), 'CFLAGS=' + os.environ.get(
                            'JARO_FILTER_COMPILER_CFLAGS', '-O2 -Wall -Wextra -Werror -pedantic')], cwd=REPO, check=True)
        cls.compiler, cls.batch = (cls.objects / n for n in ['jaro-filter-compile', 'jaro-filter'])
        cls.oracle = SieveOracle(cls.work / 'oracle', os.environ['SIEVE_DEPS_PREFIX'])
        cls.events, cls.counts = [], dict(invocations=0, target_decisions=0, oracle_decisions=0,
                                         messages=0, snapshots=0, controlled_children=0, unsettled_children=0)

    @classmethod
    def tearDownClass(cls):
        (cls.work / 'commands.json').write_text(json.dumps(cls.events, indent=2) + '\n')
        (cls.work / 'counts.json').write_text(json.dumps(cls.counts, sort_keys=True) + '\n')
        if cls.temp:
            cls.temp.cleanup()

    def setUp(self):
        self.case = self.work / self._testMethodName
        self.case.mkdir()
        self.root = self.case / 'mail root'
        self.root.mkdir()
        self.incoming = self.root / 'incoming'
        self.maildir(self.incoming)

    def maildir(self, path):
        for name in ['cur', 'new', 'tmp']:
            (path / name).mkdir(parents=True)

    def command(self, args, code=0, data=None, cwd=None):
        args = list(map(os.fspath, args))
        result = subprocess.run(args, input=data, cwd=cwd or REPO, capture_output=True, timeout=60)
        self.capture(args,result,cwd)
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def capture(self,args,result,cwd=None,extra=None):
        number = len(self.events)
        logs = {}
        for stream in ['stdout', 'stderr']:
            raw = getattr(result, stream)
            log = self.case / ('command-%05d.%s' % (number, stream))
            log.write_bytes(raw)
            logs[stream] = dict(path=str(log), sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw))
        self.events.append(dict(command=args, cwd=str(cwd or REPO), exit=result.returncode,
                                observedAt=datetime.now(timezone.utc).isoformat(), **logs, **(extra or {})))
        self.counts['invocations'] += 1

    def snapshot(self,before,after):
        self.assertEqual(after,before)
        self.counts['snapshots'] += 1
        rows = [dict(path_hex=path.hex(),type=values[0],permissions=values[1],payload_hex=values[2].hex())
                for path,values in sorted(before.items())]
        artifact = self.case/('snapshot-%05d.json' % self.counts['snapshots'])
        artifact.write_text(json.dumps(dict(root=str(self.root),rows=rows,after_equal=True),sort_keys=True)+'\n')

    def compile(self, rules=(), known=(), blacklist=(), own=(), profile=PROFILE):
        (self.root / 'Filters.txt').write_text('\n'.join(rules) + '\n')
        (self.root / 'Aliases.txt').write_text('\n'.join(json.dumps(v) for v in own))
        for filename, values in [('whitelist.abook', known), ('blacklist.abook', blacklist)]:
            text = '[format]\nprogram=abook\nversion=0.6.0\n'
            for i, address in enumerate(values):
                text += '\n[%d]\nname=Fixture\nemail=%s\n' % (i, address)
            (self.root / filename).write_text(text if values else '')
        (self.root / 'FilterProfile.txt').write_text(profile)
        output = self.command([self.compiler, '--root', self.root, '--report0']).stdout.split(b'\0')
        self.generation = output[1]
        self.product = Path(os.fsdecode(output[3]))
        return self.product

    def decode(self, result, generation=None):
        values = result.stdout.split(b'\0')
        self.assertEqual(values[-1], b'')
        self.assertEqual(values[:2], [b'JFD1', generation or self.generation])
        values = values[2:-1]
        self.assertEqual(len(values) % 11, 0)
        rows = [dict(zip(FIELDS, values[i:i+11])) for i in range(0, len(values), 11)]
        self.assertEqual([row['index'] for row in rows], [str(i+1).encode() for i in range(len(rows))])
        self.counts['messages'] += len(rows)
        return rows

    def batch_paths(self, paths, code=0, generation=None):
        before = tree(self.root)
        result = self.command([self.batch, '--root', self.root,
                               *(['--generation', generation.decode()] if generation else []),
                               '--files0-from', '-'], code, b''.join(os.fsencode(p)+b'\0' for p in paths))
        self.snapshot(before,tree(self.root))
        return self.decode(result, generation)

    @contextmanager
    def held(self, phase, selection):
        notify, writer = os.pipe()
        reader, release = os.pipe()
        args = list(map(str, [self.objects/'jaro-filter-test','--root',self.root,*selection]))
        env = dict(os.environ, JF_TEST_HOLD=phase, JF_TEST_NOTIFY_FD=str(writer), JF_TEST_RELEASE_FD=str(reader))
        child = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(writer,reader))
        os.close(writer)
        os.close(reader)
        released = False
        try:
            self.assertTrue(select.select([notify],[],[],10)[0], 'held phase not reached')
            self.assertEqual(os.read(notify,1),b'1')
            self.assertIsNone(child.poll())
            self.events.append(dict(control='ready', phase=phase, pid=child.pid,
                                    observedAt=datetime.now(timezone.utc).isoformat()))
            def finish(code=0):
                nonlocal released
                try:
                    os.write(release,b'1')
                except BrokenPipeError:
                    pass
                released = True
                out,err = child.communicate(timeout=10)
                self.capture(args,subprocess.CompletedProcess(args,child.returncode,out,err),extra=dict(
                    phase=phase,environment={key:env[key] for key in ['JF_TEST_HOLD','JF_TEST_NOTIFY_FD','JF_TEST_RELEASE_FD']}))
                self.assertEqual(child.returncode,code,err)
                return subprocess.CompletedProcess(args,child.returncode,out,err)
            yield child,finish
        finally:
            if not released:
                try:
                    os.write(release,b'1')
                except BrokenPipeError:
                    pass
            try:
                child.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.communicate(timeout=5)
            os.close(notify)
            os.close(release)
            self.counts['controlled_children'] += 1
            self.counts['unsettled_children'] += child.poll() is None
            self.events.append(dict(control='settled', phase=phase, command=args, pid=child.pid, exit=child.returncode,
                                    observedAt=datetime.now(timezone.utc).isoformat()))

    def message(self, name, headers=None, raw=None, folder=None):
        path = (folder or self.incoming) / name
        if raw is None:
            pairs = (headers or {}).items() if isinstance(headers or {}, dict) else headers
            raw = (''.join('%s: %s\r\n' % tuple(p) for p in pairs) + 'Subject: Batch fixture\r\n\r\n').encode()
            raw += b'Body untouched\x00\xff\r\nFrom: blocked@example.org\n'
        path.write_bytes(raw)
        return path

    def assert_target(self, row, case, rules, books):
        expected = authored.PolicyTests().expected_decision(case, rules, **books)
        self.assertEqual(row['rule_id'], expected['id'].encode(), case['id'])
        self.assertEqual(row['logical'], expected['logical'].encode(), case['id'])
        self.assertEqual(row['local'], os.fsencode(self.root / expected['local']), case['id'])
        self.assertEqual(row['sieve'], expected['sieve'].encode(), case['id'])
        source = expected['source'].replace('/mail/', str(self.root) + '/')
        self.assertEqual(row['source'], os.fsencode(source), case['id'])
        self.assertEqual(row['line'], expected['line'].encode(), case['id'])
        self.assertEqual((row['action'], row['status'], row['detail']), (b'move', b'classified', b''))

    def test_original_52_authored_targets_and_actual_sieve(self):
        target = json.loads((FIXTURES / 'target.json').read_text())
        casing = json.loads((FIXTURES / 'casing.json').read_text())
        groups = [(target['cases'], target['rules'], target),
                  (target['domain_cases'], target['domain_rules'], {}), (casing['cases'], casing['rules'], casing)]
        groups += [([case], case['rules'], {}) for case in target['isolated_domain_cases']]
        count = 0
        for group, (cases, rules, contacts) in enumerate(groups):
            books = {key: contacts.get(key, []) for key in ['known', 'blacklist', 'own']}
            product = self.compile(rules, **books)
            self.oracle.compile(product / 'Filters.sieve', extensions='+regex')
            paths = [self.message(('cur/' if i % 2 else 'new/') + '%d-%s:2,FS' % (group, case['id']), case['headers'])
                     for i, case in enumerate(cases)]
            rows = self.batch_paths(paths)
            self.assertEqual(len(rows), len(cases))
            for row, path, case in zip(rows, paths, cases):
                self.assertEqual(row['path'], os.fsencode(path))
                self.assert_target(row, case, rules, books)
                self.oracle.expect(product / 'Filters.sieve', path, case['sieve'], True, extensions='+regex')
                count += 1
        self.assertEqual(count, 52)
        self.counts['target_decisions'] += count
        self.counts['oracle_decisions'] += count
        print('Independent batch target identities: 52/52; same-policy Sieve winners: 52/52')

    def test_normalization_and_qualified_regex_corpus(self):
        corpus = json.loads((REPO / 'extras/test/fixtures/filter-contract/regex-subset.json').read_text())
        previous = None
        for i, case in enumerate(corpus['cases']):
            if case['pattern'] != previous:
                product = self.compile(['from '+json.dumps(case['pattern'])+' move matched'])
                previous = case['pattern']
            path = self.message('new/regex-%d' % i, {case['field']: case['address']})
            row = self.batch_paths([path])[0]
            expected = case['expected']
            self.assertEqual((row['local'], row['sieve']), (os.fsencode(self.root/expected), expected.encode()))
            expected_id = authored.explicit('from', case['pattern'], 'matched') if expected=='matched' else 'builtin:unsorted'
            self.assertEqual(row['rule_id'], expected_id.encode())
            self.oracle.expect(product/'Filters.sieve', path, expected, True, extensions='+regex')
            self.counts['oracle_decisions'] += 1
        normal = json.loads((REPO/'extras/test/fixtures/filter-contract/mailbox-normalization.json').read_text())
        for i, group in enumerate(normal['groups']):
            product = self.compile(['from-address %s move normalized' % json.dumps(group['canonical'])])
            paths = [self.message('new/normal-%d-%d' % (i, j), {'From': form}) for j, form in enumerate(group['forms'])]
            for row, path in zip(self.batch_paths(paths), paths):
                expected = authored.explicit('from-address', authored.ascii_fold(group['canonical']), 'normalized')
                self.assertEqual(row['rule_id'], expected.encode())
                self.oracle.expect(product/'Filters.sieve', path, 'normalized', True, extensions='+regex')
                self.counts['oracle_decisions'] += 1

    def test_raw_byte_paths_occurrences_sorting_and_selection(self):
        self.compile()
        names = [b'new/space name', b'cur/new\nline:2,RS', b'new/-leading', b'cur/non-utf8-\xff:2,FS',
                 b'new/same', b'cur/same']
        paths = []
        for name in names:
            path = os.fsencode(self.incoming) + b'/' + name
            with open(path, 'wb') as file:
                file.write(b'Subject: bytes\n\n\x00body\xff')
            paths.append(path)
        self.message('tmp/excluded')
        (self.incoming/'cur/directory').mkdir()
        os.symlink(paths[0], os.fsencode(self.incoming)+b'/new/link')
        os.mkfifo(self.incoming/'new/pipe')
        before = tree(self.root)
        result = self.command([self.batch, '--root', self.root, '--maildir', self.incoming])
        rows = self.decode(result)
        self.assertEqual([row['path'] for row in rows], sorted(paths))
        self.assertEqual(tree(self.root), before)
        explicit = [paths[3], paths[0], paths[3], paths[1]]
        self.assertEqual([r['path'] for r in self.batch_paths(explicit)], explicit)
        leading = self.case/'-literal'
        leading.write_bytes(b'X: y\n\nBody')
        row = self.decode(self.command([self.batch, '--root', self.root, '--', '-literal'], cwd=self.case))[0]
        self.assertEqual(row['path'], b'-literal')
        listed = self.case/'-'
        listed.write_bytes(b'-literal\0-literal\0')
        rows = self.decode(self.command([self.batch, '--root', self.root, '--files0-from', './-'], cwd=self.case))
        self.assertEqual([r['path'] for r in rows], [b'-literal', b'-literal'])
        self.snapshot(before,tree(self.root))

    def test_empty_selection_in_classify_and_explicit_apply(self):
        self.compile()
        self.assertEqual(self.batch_paths([]), [])
        before = tree(self.root)
        self.assertEqual(self.decode(self.command([self.batch, '--root', self.root, '--maildir', self.incoming])), [])
        result = self.command([self.batch, '--root', self.root, '--mode', 'apply', '--maildir', self.incoming])
        self.assertEqual(self.decode(result), [])
        self.assertEqual(tree(self.root), before)

    def test_message_errors_continue_and_retain_all_bytes(self):
        self.compile()
        paths = [self.message('new/empty', raw=b''), self.message('new/truncated', raw=b'From: x@example.org\n'),
                 self.message('new/bad', raw=b'From: blocked@example.org\nTo: invalid\n\nBody'),
                 self.message('new/long', raw=b'X: '+b'x'*65536+b'\n\nBody'),
                 self.message('new/control', raw=b'X: bad\0value\n\n'), self.incoming/'new/missing',
                 self.incoming/'new/directory', self.incoming/'new/link', self.incoming/'new/fifo',
                 self.message('new/valid')]
        paths[6].mkdir()
        paths[7].symlink_to(paths[-1])
        os.mkfifo(paths[8])
        rows = self.batch_paths(paths, 7)
        codes = [b'E_MESSAGE_TRUNCATED', b'E_MESSAGE_TRUNCATED', b'E_MESSAGE_ADDRESS', b'E_MESSAGE_LIMIT',
                 b'E_MESSAGE_TEXT', b'E_MESSAGE_OPEN', b'E_MESSAGE_TYPE', b'E_MESSAGE_TYPE', b'E_MESSAGE_TYPE']
        self.assertEqual([r['detail'] for r in rows[:-1]], codes)
        for row in rows[:-1]:
            self.assertEqual([row[k] for k in ['rule_id','action','logical','local','sieve','status','source','line']],
                             [b'', b'none', b'', b'', b'', b'error', b'', b'0'])
        self.assertEqual(rows[-1]['status'], b'classified')

    def test_directory_symlinks_are_never_followed(self):
        self.compile()
        path = self.message('new/message')
        alias = self.case/'linked-maildir'
        alias.symlink_to(self.incoming,target_is_directory=True)
        rows = self.batch_paths([alias/'new/message'],7)
        self.assertEqual(rows[0]['detail'],b'E_MESSAGE_OPEN')
        result = self.command([self.batch,'--root',self.root,'--maildir',alias],3)
        self.assertEqual(result.stdout,b'')
        nested = self.case/'nested'
        nested.mkdir()
        (nested/'redirect').symlink_to(self.root,target_is_directory=True)
        result = self.command([self.batch,'--root',self.root,'--maildir',nested/'redirect/incoming'],3)
        self.assertEqual(result.stdout,b'')
        self.assertEqual(path.read_bytes()[-13:],b'@example.org\n')

    def test_unreadable_message_and_failed_stdout(self):
        self.compile()
        path = self.message('new/private')
        original = path.read_bytes()
        original_mode = stat.S_IMODE(path.stat().st_mode)
        path.chmod(0)
        try:
            result = self.command([self.batch,'--root',self.root,'--',path],7)
            self.assertEqual(self.decode(result)[0]['detail'],b'E_MESSAGE_OPEN')
        finally:
            path.chmod(original_mode)
        self.assertEqual(path.read_bytes(),original)
        with open('/dev/full','wb') as output:
            args = list(map(str,[self.batch,'--root',self.root,'--',path]))
            result = subprocess.run(args,stdout=output,stderr=subprocess.PIPE,timeout=10)
        self.assertEqual(result.returncode,3,result.stderr)
        self.events.append(dict(command=args,cwd=str(REPO),exit=result.returncode,stdout_destination='/dev/full',
                                stderr_hex=result.stderr.hex(),observedAt=datetime.now(timezone.utc).isoformat()))

    def test_source_equals_destination_is_noop(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        paths = [self.message('new/already:2,FS', folder=self.root/'unsorted'),
                 self.message('cur/already:2,RS', folder=self.root/'unsorted')]
        for row in self.batch_paths(paths):
            self.assertEqual((row['action'], row['status'], row['detail']), (b'none', b'noop', b''))
            self.assertEqual(row['rule_id'], b'builtin:unsorted')

    def test_noop_uses_parent_identity_with_lexical_dot_components(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        path = self.message('cur/already:2,FS',folder=self.root/'unsorted')
        spelling = str(path.parent)+'/'+('./'*2100)+path.name
        self.assertGreater(len(os.fsencode(spelling)),4096)
        row = self.batch_paths([spelling])[0]
        self.assertEqual((row['path'],row['action'],row['status']),
                         (os.fsencode(spelling),b'none',b'noop'))

    def test_cli_validation_and_complete_list_framing(self):
        self.compile()
        prefix = [self.batch, '--root', self.root]
        cases = [[], ['--root', ''], ['--root', self.root], ['--maildir', self.incoming, '--files0-from', '-'],
                 ['--mode', 'unknown', '--maildir', self.incoming], ['--mode', 'classify', '--mode', 'classify', '--maildir', self.incoming],
                 ['--generation', '', '--maildir', self.incoming], ['--maildir', self.incoming, '--', 'path'],
                 ['--unknown'], ['--'], ['--', ''], ['--maildir'], ['--files0-from', '']]
        before = tree(self.root)
        for args in cases:
            self.assertEqual(self.command(prefix+args, 2).stdout, b'')
        for data in [b'partial', b'\0', b'path\0\0', b'valid\0partial']:
            self.assertEqual(self.command(prefix+['--mode','apply','--files0-from','-'], 2, data).stdout, b'')
        self.assertEqual(self.command(prefix+['--files0-from', self.case/'absent'], 3).stdout, b'')
        self.assertEqual(tree(self.root), before)
        for option in ['--help', '--version']:
            self.command([self.batch, option])

    def test_named_list_stream_waits_for_complete_eof_framing(self):
        self.compile()
        path = self.message('new/streamed')
        fifo = self.case/'paths.fifo'
        os.mkfifo(fifo)
        for data,code in [(os.fsencode(path)+b'\0',0),(os.fsencode(path),2)]:
            args = list(map(str,[self.batch,'--root',self.root,'--files0-from',fifo]))
            child = subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            writer = None
            try:
                deadline = time.monotonic()+5
                while writer is None and time.monotonic()<deadline:
                    self.assertIsNone(child.poll(),'list reader exited before a writer connected')
                    try:
                        writer = os.open(fifo,os.O_WRONLY|os.O_NONBLOCK)
                    except OSError as error:
                        if error.errno!=errno.ENXIO: # Reader has not opened the FIFO yet.
                            raise
                        time.sleep(0.01)
                self.assertIsNotNone(writer,'named-list read precondition was not reached')
                self.assertEqual(os.write(writer,data),len(data))
                self.events.append(dict(control='ready',phase='list-writer-held-before-eof',pid=child.pid,
                                        observedAt=datetime.now(timezone.utc).isoformat()))
                self.assertFalse(select.select([child.stdout],[],[],0.1)[0], 'output appeared before list EOF')
                self.assertIsNone(child.poll())
                os.close(writer)
                writer = None
                out,err = child.communicate(timeout=10)
                result = subprocess.CompletedProcess(args,child.returncode,out,err)
                self.capture(args,result)
                self.assertEqual(child.returncode,code,err)
                if code:
                    self.assertEqual(out,b'')
                else:
                    self.assertEqual(self.decode(result)[0]['path'],os.fsencode(path))
            finally:
                if writer is not None:
                    os.close(writer)
                try:
                    child.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    child.terminate()
                    child.communicate(timeout=5)
                self.counts['controlled_children'] += 1
                self.counts['unsettled_children'] += child.poll() is None
                self.events.append(dict(control='settled',phase='list-writer-held-before-eof',pid=child.pid,
                                        exit=child.returncode,observedAt=datetime.now(timezone.utc).isoformat()))

    def test_path_count_boundary(self):
        self.compile()
        prefix = [self.batch, '--root', self.root, '--files0-from', '-']
        result = self.command(prefix, 7, b'missing\0'*100000)
        self.assertEqual(len(self.decode(result)), 100000)
        result = self.command(prefix, 2, b'missing\0'*100001)
        self.assertEqual(result.stdout, b'')

    def test_complete_path_byte_limit_before_any_application(self):
        self.compile()
        no_cache = self.case/'uncached-root'
        no_cache.mkdir()
        args = [self.batch,'--root',no_cache,'--mode','apply','--files0-from','-']
        # Exactly 64 MiB including delimiters; 1024 nonempty occurrences.
        data = (b'a'*65535+b'\0')*1024
        self.assertEqual(len(data),64*1024*1024)
        result = self.command(args,5,data)
        self.assertIn(b'E_GENERATION',result.stderr)
        self.assertEqual(result.stdout,b'')
        self.assertEqual(self.command(args,2,data+b'x').stdout,b'')

    def test_hundred_thousand_user_rules_exclude_three_builtins(self):
        self.compile(['known-domain d%d.example.org' % i for i in range(100000)])
        path = self.message('new/boundary',{'From':'x@d99999.example.org'})
        row = self.batch_paths([path])[0]
        self.assertEqual(row['rule_id'],authored.explicit('known-domain','d99999.example.org','known').encode())
        self.assertEqual((row['logical'],row['sieve']),(b'known',b'INBOX'))

    def test_one_header_read_per_message_and_no_external_command_dependency(self):
        self.compile()
        paths = [self.message('new/message-%04d' % i) for i in range(1000)]
        listing = self.case/'paths.bin'
        listing.write_bytes(b''.join(os.fsencode(p)+b'\0' for p in paths))
        env = dict(os.environ, PATH='/nonexistent-fixture-path', JF_TEST_TRACE='1')
        result = subprocess.run([self.objects/'jaro-filter-test','--root',self.root,'--files0-from',listing],
                                capture_output=True,env=env,timeout=20)
        self.capture(list(map(str,result.args)),result,extra=dict(environment={'PATH':env['PATH'],'JF_TEST_TRACE':'1'}))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(len(self.decode(result)),1000)
        self.assertEqual(result.stderr.splitlines().count(b'TRACE:batch-pinned'),1)
        self.assertEqual(result.stderr.splitlines().count(b'TRACE:headers-read'),1000)
        # Test controls are absent from the production executable.
        env['JF_TEST_CRASH']='batch-pinned'
        result = subprocess.run([self.batch,'--root',self.root,'--files0-from',listing],capture_output=True,env=env,timeout=20)
        self.capture(list(map(str,result.args)),result,extra=dict(environment={k:env[k] for k in ['PATH','JF_TEST_TRACE','JF_TEST_CRASH']}))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr,b'')

    def test_snapshot_selection_and_prior_generation(self):
        self.compile(['from-address x@example.org move old'])
        old = self.generation
        path = self.message('new/message', {'From': 'x@example.org'})
        # Direct helper deliberately uses immutable selected policy, even when
        # authored sources change. Wrapper fingerprint checks belong to L5.3.
        (self.root/'Filters.txt').write_text('from-address x@example.org move new\n')
        self.assertEqual(self.batch_paths([path])[0]['logical'], b'old')
        self.compile(['from-address x@example.org move new'])
        self.assertNotEqual(old, self.generation)
        self.assertEqual(self.batch_paths([path])[0]['logical'], b'new')
        self.assertEqual(self.batch_paths([path], generation=old)[0]['logical'], b'old')

    def test_batch_pins_once_across_concurrent_publication(self):
        self.compile(['from-address x@example.org move old'])
        old = self.generation
        path = self.message('new/message', {'From':'x@example.org'})
        with self.held('batch-pinned',['--',str(path),str(path)]) as (_,finish):
            self.compile(['from-address x@example.org move new'])
            self.assertNotEqual(self.generation,old)
            rows = self.decode(finish(),old)
            self.assertEqual([r['logical'] for r in rows],[b'old',b'old'])
        self.assertEqual(self.batch_paths([path])[0]['logical'],b'new')

    def test_maildir_snapshot_excludes_additions_and_rejects_replacements(self):
        self.compile()
        path = self.message('new/selected')
        with self.held('batch-selected',['--maildir',str(self.incoming)]) as (_,finish):
            self.message('new/added-later')
            path.rename(self.incoming/'cur/renamed:2,S')
            self.message('new/selected',raw=b'Subject: replacement\n\nKeep me')
            rows = self.decode(finish(7))
            self.assertEqual(len(rows),1)
            self.assertEqual((rows[0]['path'],rows[0]['detail']), (os.fsencode(path),b'E_MESSAGE_CHANGED'))
        self.assertEqual(path.read_bytes(),b'Subject: replacement\n\nKeep me')

    def test_opened_source_replacement_and_edit_are_errors(self):
        self.compile()
        for phase in ['message-opened','headers-read']:
            path = self.message('new/'+phase)
            with self.held(phase,['--',str(path)]) as (_,finish):
                if phase=='message-opened':
                    path.rename(self.incoming/'cur/saved-original')
                    self.message('new/'+phase,raw=b'Subject: replace\n\nReplacement')
                else:
                    with path.open('ab') as file:
                        file.write(b'changed body')
                rows = self.decode(finish(7))
                self.assertEqual(rows[0]['detail'],b'E_MESSAGE_CHANGED')

    def test_signal_status_and_readonly_cleanup(self):
        self.compile()
        path = self.message('new/message')
        before = tree(self.root)
        with self.held('batch-pinned',['--',str(path)]) as (child,finish):
            child.send_signal(signal.SIGTERM)
            result = finish(-signal.SIGTERM)
            self.assertEqual(result.stdout,b'')
        self.assertEqual(tree(self.root),before)

    def rewrite_cache(self, transform):
        path = self.product/'policy.bin'
        data = path.read_bytes()
        payload = struct.pack('<I', len(records(data)))
        for tag, fields in records(data):
            fields = transform(tag, list(fields))
            payload += tag+struct.pack('<I', len(fields))
            payload += b''.join(struct.pack('<I', len(f))+f for f in fields)
        data = data[:16]+struct.pack('<Q',len(payload))+data[24:56]+hashlib.sha256(payload).digest()+payload
        path.write_bytes(data)
        manifest = self.product/'manifest.json'
        info = json.loads(manifest.read_text())
        info['policy_sha256'] = hashlib.sha256(data).hexdigest()
        manifest.write_text(json.dumps(info, separators=(',', ':'))+'\n')

    def test_corrupt_incompatible_and_missing_generations_fail_before_output(self):
        self.compile(['known-domain *.example.org'])
        prefix = [self.batch, '--root', self.root, '--maildir', self.incoming]
        original = {p:p.read_bytes() for p in self.product.iterdir()}
        mutations = [lambda tag,f: [b'jaromail-filter-compiler-v1']+f[1:] if tag==b'M' else f,
                     lambda tag,f: f[:5]+[b'*.bad..example']+f[6:] if tag==b'R' and f[2]==b'domain-subdomains' else f,
                     lambda tag,f: f[:5]+[b'example.org']+f[6:] if tag==b'R' and f[2]==b'domain-subdomains' else f]
        for mutation in mutations:
            self.rewrite_cache(mutation)
            before = tree(self.root)
            self.assertEqual(self.command(prefix, 5).stdout, b'')
            self.assertEqual(tree(self.root), before)
            for path,data in original.items():
                path.write_bytes(data)
        for filename in ['policy.bin', 'filters.zsh', 'Filters.sieve', 'manifest.json']:
            path = self.product/filename
            path.write_bytes(path.read_bytes()+b'corrupt')
            self.assertEqual(self.command(prefix, 5).stdout, b'')
            path.write_bytes(original[path])
        self.assertEqual(self.command([self.batch,'--root',self.case/'absent','--maildir',self.incoming], 3).stdout, b'')
        no_cache = self.case/'no-cache'
        no_cache.mkdir()
        self.assertEqual(self.command([self.batch,'--root',no_cache,'--maildir',self.incoming], 5).stdout, b'')
        self.assertEqual(self.command([self.batch,'--root',self.root,'--generation','0'*64,'--maildir',self.incoming], 5).stdout, b'')
        (self.root/'cache/filter-engine/journal').write_bytes(b'pending')
        self.assertEqual(self.command(prefix, 5).stdout, b'')

    def test_cache_reconstruction_allocator_failures(self):
        product = self.compile(['from ^x@example[.]org$ move route', 'known-domain *.example.org'],
                               known=['known@example.org'], blacklist=['blocked@example.org'], own=['own@example.org'])
        self.command([self.objects/'filter-compiler-driver', '--restore', product/'policy.bin'])

    def test_busy_lock_has_no_decision_stream(self):
        self.compile()
        lock = self.root/'cache/filter-engine/lock'
        # A separate process is necessary: POSIX record locks are per-process.
        reader, writer = os.pipe()
        child = subprocess.Popen([sys.executable, '-c',
            'import fcntl,os,sys; f=open(sys.argv[1],"r+"); fcntl.lockf(f,fcntl.LOCK_EX); '
            'os.write(int(sys.argv[2]),b"held"); os.read(0,1)', str(lock), str(writer)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, pass_fds=(writer,))
        os.close(writer)
        try:
            self.assertTrue(select.select([reader],[],[],5)[0])
            self.assertEqual(os.read(reader,4), b'held')
            self.assertIsNone(child.poll())
            result = self.command([self.batch,'--root',self.root,'--maildir',self.incoming],6)
            self.assertEqual(result.stdout,b'')
        finally:
            os.close(reader)
            child.communicate(b'x',timeout=5)
            self.counts['controlled_children'] += 1
            self.counts['unsettled_children'] += child.poll() is None
        self.assertEqual(child.returncode,0)


if __name__ == '__main__':
    program = unittest.main(exit=False, verbosity=2)
    print('Batch counts: '+json.dumps(getattr(BatchTests,'counts',{}),sort_keys=True))
    raise SystemExit(not program.result.wasSuccessful())
