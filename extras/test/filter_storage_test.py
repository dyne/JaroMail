"""Native Maildir ownership/publication and exact full-body accounting."""
from contextlib import contextmanager
from pathlib import Path
import collections
import fcntl
import hashlib
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
import filter_batch_test as fixtures
from filter_generation_test import Held

REPO = fixtures.REPO
EVIDENCE = Path(os.environ.get('JARO_FILTER_CROSS_FS_DIR', REPO.parent/'.gestalt/evidence/native-filter-execution/l5'))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def body_files(root):
    rows = []
    for parent,dirs,files in os.walk(os.fsencode(root),followlinks=False):
        if b'cache' in dirs:
            dirs.remove(b'cache')
        for name in files:
            path = os.path.join(parent,name)
            st = os.lstat(path)
            relative = os.path.relpath(path,os.fsencode(root)).split(b'/')
            if stat.S_ISREG(st.st_mode) and any(part in [b'cur',b'new',b'tmp'] for part in relative[:-1]):
                data = Path(os.fsdecode(path)).read_bytes()
                rows.append(dict(path_hex=path.hex(),bytes=len(data),sha256=digest(data),mode=stat.S_IMODE(st.st_mode)))
    return sorted(rows,key=lambda row:bytes.fromhex(row['path_hex']))


class StorageTests(unittest.TestCase):
    setUp = fixtures.BatchTests.setUp
    maildir = fixtures.BatchTests.maildir
    compile = fixtures.BatchTests.compile
    message = fixtures.BatchTests.message
    decode = fixtures.BatchTests.decode
    command = fixtures.BatchTests.command
    capture = fixtures.BatchTests.capture
    assert_target = fixtures.BatchTests.assert_target

    @classmethod
    def setUpClass(cls):
        previous = os.environ.get('JARO_FILTER_BATCH_WORK')
        if os.environ.get('JARO_FILTER_STORAGE_WORK'):
            os.environ['JARO_FILTER_BATCH_WORK'] = os.environ['JARO_FILTER_STORAGE_WORK']
        try:
            fixtures.BatchTests.setUpClass.__func__(cls)
        finally:
            if previous is None:
                os.environ.pop('JARO_FILTER_BATCH_WORK',None)
            else:
                os.environ['JARO_FILTER_BATCH_WORK'] = previous
        cls.counts.update(applied_targets=0,body_inventories=0,real_cross_filesystem=0,fault_cases=0)

    @classmethod
    def tearDownClass(cls):
        fixtures.BatchTests.tearDownClass.__func__(cls)

    def fresh_root(self,label):
        self.root = self.case/label
        self.root.mkdir()
        self.incoming = self.root/'incoming'
        self.maildir(self.incoming)
        self.compile()

    def argv(self,paths,testing=False):
        return list(map(str,[self.objects/('jaro-filter-test' if testing else 'jaro-filter'),
                             '--root',self.root,'--mode','apply','--',*paths]))

    def apply(self,paths,code=0,extra=None):
        args = self.argv(paths,bool(extra))
        result = subprocess.run(args,env=dict(os.environ,**(extra or {})),capture_output=True,timeout=30)
        self.capture(args,result,extra=dict(environment=extra or {}))
        self.assertEqual(result.returncode,code,result.stderr)
        return self.decode(result)

    def event(self,args,code,out,err,env):
        self.capture(args,subprocess.CompletedProcess(args,code,out,err),extra=dict(
            environment={k:v for k,v in env.items() if k.startswith('JF_TEST_')}))

    @contextmanager
    def held(self,phase,paths,command=None):
        self.env = dict(os.environ)
        self.held_processes = []
        # Existing bounded pipe/barrier harness; all gates are released before
        # cleanup waits, even when an assertion fails midway through a race.
        self.held = []
        handle = Held(self,phase,command=command or self.argv(paths,True))
        try:
            handle.ready()
            yield handle
        finally:
            if not handle.finished:
                try:
                    os.write(handle.release,b'1'*64)
                except BrokenPipeError:
                    pass
                try:
                    handle.finish()
                except subprocess.TimeoutExpired:
                    pass
            handle.close()
            self.counts['controlled_children'] += 1
            self.counts['unsettled_children'] += handle.process.poll() is None
            # Held expects owner.held as a list; restore this method afterward.
            del self.held

    def finish(self,handle,code=0):
        handle.resume()
        actual,out,err = handle.finish()
        self.assertEqual(actual,code,err)
        return self.decode(subprocess.CompletedProcess(handle.process.args,actual,out,err))

    def inventory(self,expected,root=None):
        rows = body_files(root or self.root)
        counts = collections.Counter(row['sha256'] for row in rows)
        self.assertEqual(counts,collections.Counter({digest(data):number for data,number in expected}))
        self.counts['body_inventories'] += 1
        artifact = self.case/('bodies-%04d.json' % self.counts['body_inventories'])
        artifact.write_text(json.dumps(rows,sort_keys=True)+'\n')
        return rows

    def clean_private(self,retained=()):
        remaining = [p for p in self.root.rglob('.jaro-filter.*') if p.name!='.jaro-filter.foreign' and p not in retained]
        self.assertEqual(remaining,[])
        lock = self.root/'cache/filter-engine/batch.lock'
        self.assertTrue(lock.is_file())
        self.assertEqual(stat.S_IMODE(lock.stat().st_mode),0o600)

    def test_same_filesystem_full_bytes_flags_collisions_and_reruns(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        existing = self.message('cur/same:2,FS',raw=b'Subject: existing\n\nold',folder=self.root/'unsorted')
        first = self.message('cur/same:2,FS',raw=b'Subject: first\n\n'+bytes(range(256))*8192)
        second = self.message('new/same:2,FS',raw=b'Subject: second\n\nsecond')
        dangling = self.root/'unsorted/cur/dangling:2,S'
        dangling.symlink_to(self.case/'absent')
        third = self.message('new/dangling:2,S',raw=b'Subject: third\n\nthird')
        bodies = [p.read_bytes() for p in [existing,first,second,third]]
        self.assertEqual(first.stat().st_dev,self.root.stat().st_dev)
        rows = self.apply([first,second,third])
        self.assertEqual([r['status'] for r in rows],[b'moved']*3)
        self.assertFalse(any(p.exists() for p in [first,second,third]))
        self.assertTrue(dangling.is_symlink())
        final = self.inventory([(data,1) for data in bodies])
        self.assertEqual(sorted(Path(os.fsdecode(bytes.fromhex(r['path_hex']))).name.split(':2,')[-1] for r in final),['FS','FS','FS','S'])
        self.clean_private()
        lock_inode = (self.root/'cache/filter-engine/batch.lock').stat().st_ino
        result = self.command([self.batch,'--root',self.root,'--mode','apply','--maildir',self.incoming])
        self.assertEqual(self.decode(result),[])
        self.assertEqual((self.root/'cache/filter-engine/batch.lock').stat().st_ino,lock_inode)
        self.assertEqual(self.apply([first],7)[0]['status'],b'error')
        self.inventory([(data,1) for data in bodies])

    def test_raw_bytes_and_long_flags_preserved(self):
        self.compile()
        names = [b'new/space name:2,FS',b'cur/new\nline:2,RS',b'new/-leading:2,S',b'cur/nonutf8-\xff:2,F',
                 b'new/x:2,'+b'F'*240]
        paths,bodies = [],[]
        for i,name in enumerate(names):
            path = os.fsencode(self.incoming)+b'/'+name
            data = b'Subject: raw '+str(i).encode()+b'\n\n\xff\x00body'
            with open(path,'wb') as stream:
                stream.write(data)
            paths.append(os.fsdecode(path)); bodies.append(data)
        rows = self.apply(paths)
        self.assertEqual([r['path'] for r in rows],[os.fsencode(p) for p in paths])
        for path,data in zip(paths,bodies):
            target = self.root/'unsorted/cur'/Path(path).name
            self.assertEqual(target.read_bytes(),data)
        self.inventory([(data,1) for data in bodies])
        self.clean_private()

    def test_real_cross_filesystem_transfer(self):
        self.compile()
        if not EVIDENCE.is_dir() or EVIDENCE.stat().st_dev == self.root.stat().st_dev:
            # The production adapter always uses the same bounded copy and
            # publication path on either filesystem. Exercise a controlled
            # second-chunk write failure if no second writable device was
            # explicitly supplied, and record that this is not a real EXDEV run.
            path = self.message('new/controlled-copy',raw=b'Subject: controlled copy\n\n'+bytes(range(256))*4096)
            data = path.read_bytes()
            rows = self.apply([path],7,dict(JF_TEST_FAIL='store-write',JF_TEST_FAIL_AT='2'))
            self.assertEqual(rows[0]['status'],b'error')
            self.assertEqual(path.read_bytes(),data)
            self.inventory([(data,1)])
            self.clean_private()
            self.events.append(dict(cross_filesystem=dict(real=False,controlled_fault='store-write-at-2',
                limitation='No explicit writable second filesystem available',source_retained=True)))
            self.counts['controlled_cross_filesystem'] = self.counts.get('controlled_cross_filesystem',0)+1
            return
        owned = Path(tempfile.mkdtemp(prefix='l52-cross-source-',dir=EVIDENCE))
        path = owned/'cross:2,FS'
        data = b'Subject: cross device\n\n'+bytes(range(256))*4096
        path.write_bytes(data)
        source_device,destination_device = path.stat().st_dev,self.root.stat().st_dev
        self.assertNotEqual(source_device,destination_device,'fixture must be a real cross-device transfer')
        rows = self.apply([path])
        self.assertEqual(rows[0]['status'],b'moved')
        self.assertFalse(path.exists())
        self.assertEqual(list(owned.iterdir()),[])
        self.inventory([(data,1)])
        self.clean_private()
        self.counts['real_cross_filesystem'] += 1
        self.events.append(dict(cross_filesystem=dict(source=str(owned),source_device=source_device,
                                                     destination_device=destination_device,source_removed=True)))

    def test_all_52_authored_apply_targets_and_actual_sieve(self):
        target = json.loads((fixtures.FIXTURES/'target.json').read_text())
        casing = json.loads((fixtures.FIXTURES/'casing.json').read_text())
        groups = [(target['cases'],target['rules'],target), (target['domain_cases'],target['domain_rules'],{}),
                  (casing['cases'],casing['rules'],casing)]
        groups += [([case],case['rules'],{}) for case in target['isolated_domain_cases']]
        all_bodies=[]
        for group,(cases,rules,contacts) in enumerate(groups):
            books = {key:contacts.get(key,[]) for key in ['known','blacklist','own']}
            product = self.compile(rules,**books)
            paths = [self.message(('cur/' if i%2 else 'new/')+'%d-%s:2,FS' % (group,case['id']),case['headers'])
                     for i,case in enumerate(cases)]
            for path,case in zip(paths,cases):
                self.oracle.expect(product/'Filters.sieve',path,case['sieve'],True,extensions='+regex')
                self.counts['oracle_decisions'] += 1
            bodies = [p.read_bytes() for p in paths]
            rows = self.apply(paths)
            for row,path,case,body in zip(rows,paths,cases,bodies):
                self.assertEqual(row['status'],b'moved')
                checked = dict(row,status=b'classified')
                self.assert_target(checked,case,rules,books)
                self.assertFalse(path.exists())
                self.assertEqual((self.root/case['local']/'cur'/path.name).read_bytes(),body)
            self.counts['applied_targets'] += len(rows)
            all_bodies += bodies
        self.assertEqual(self.counts['applied_targets'],52)
        # Some independent fixtures intentionally share message bytes.
        self.inventory(collections.Counter(all_bodies).items())
        self.clean_private()

    def test_noop_does_not_change_source_bytes_flags_or_inode(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        path = self.message('new/already:2,FS',folder=self.root/'unsorted')
        before = path.read_bytes(),path.stat().st_ino
        rows = self.apply([path,path])
        self.assertEqual([r['status'] for r in rows],[b'noop',b'noop'])
        self.assertEqual([r['action'] for r in rows],[b'none',b'none'])
        self.assertEqual((path.read_bytes(),path.stat().st_ino),before)
        self.clean_private()

    def test_faults_never_report_partial_success_and_continue_batch(self):
        faults = {
            'store-sync-hierarchy':(0,1), 'store-read':(0,1), 'store-write':(0,1), 'store-sync-file':(0,1),
            'store-before-verify':(0,1), 'store-before-publish':(0,1), 'store-published':(1,1),
            'store-sync-published':(1,1), 'store-sync-published-file':(1,1), 'store-cleanup-stage':(1,1), 'store-sync-stage':(1,1),
            'store-before-claim':(1,1), 'store-source-claimed':(1,1), 'store-sync-claim':(1,1),
            'store-sync-source-claim':(1,1), 'store-before-unlink':(1,1), 'store-unlink':(1,1),
            'store-sync-source-unlinked':(1,0), 'store-sync-cleanup-directory':(1,1)}
        for phase,(published,source_remains) in faults.items():
            with self.subTest(phase=phase):
                self.fresh_root(phase)
                first = self.message('new/first',raw=b'Subject: failed '+phase.encode()+b'\n\n'+b'\xff\x00'*70000)
                second = self.message('new/second',raw=b'Subject: success '+phase.encode()+b'\n\nnext')
                a,b = first.read_bytes(),second.read_bytes()
                extra = dict(JF_TEST_FAIL=phase)
                if phase in ['store-read','store-write']:
                    extra['JF_TEST_FAIL_AT']='2'
                rows = self.apply([first,second],7,extra)
                self.assertEqual([r['status'] for r in rows],[b'error',b'moved'])
                self.assertEqual(rows[0]['rule_id'],b'builtin:unsorted')
                self.assertEqual((rows[0]['action'],rows[0]['logical'],rows[0]['local'],rows[0]['sieve']),
                                 (b'none',b'',b'',b''))
                self.assertEqual(first.exists(),bool(source_remains))
                self.assertFalse(second.exists())
                self.inventory([(a,published+source_remains),(b,1)])
                self.clean_private()
                self.counts['fault_cases'] += 1

    def test_destination_symlink_components_and_unwritable_directory(self):
        for kind in ['symlink','nested-symlink','unwritable']:
            with self.subTest(kind=kind):
                self.fresh_root(kind)
                outside = self.case/('outside-'+kind)
                outside.mkdir()
                marker = outside/'untouched'
                marker.write_bytes(b'foreign')
                if kind=='symlink':
                    (self.root/'unsorted').symlink_to(outside,target_is_directory=True)
                elif kind=='nested-symlink':
                    self.compile(['from-address x@example.org move redirect/child'])
                    (self.root/'redirect').symlink_to(outside,target_is_directory=True)
                else:
                    self.maildir(self.root/'unsorted')
                    (self.root/'unsorted/cur').chmod(0o500)
                path = self.message('new/keep',{'From':'x@example.org'})
                data = path.read_bytes()
                try:
                    self.assertEqual(self.apply([path],7)[0]['status'],b'error')
                finally:
                    if kind=='unwritable':
                        (self.root/'unsorted/cur').chmod(0o700)
                self.assertEqual(path.read_bytes(),data)
                self.assertEqual(marker.read_bytes(),b'foreign')
                self.assertEqual(list(outside.iterdir()),[marker])

    def test_replacement_at_claim_boundary_is_restored_without_deletion(self):
        self.compile()
        path = self.message('new/source:2,S',raw=b'Subject: original\n\nold')
        original = path.read_bytes()
        replacement = b'Subject: replacement\n\nnew'
        with self.held('store-before-claim',[path]) as handle:
            replacement_path = self.message('new/replacement',raw=replacement)
            os.replace(replacement_path,path)
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['detail'],b'E_SOURCE_CHANGED')
        self.assertEqual(path.read_bytes(),replacement)
        self.inventory([(original,1),(replacement,1)])
        self.clean_private()

    def test_original_name_replacement_before_owned_unlink_survives(self):
        self.compile()
        path = self.message('new/source:2,S',raw=b'Subject: original\n\nold')
        original = path.read_bytes()
        replacement = b'Subject: later replacement\n\nnew'
        with self.held('store-before-unlink',[path,path]) as handle:
            self.assertFalse(path.exists(),'source must already be atomically owned')
            path.write_bytes(replacement)
            rows = self.finish(handle,7)
        self.assertEqual([r['status'] for r in rows],[b'moved',b'error'])
        self.assertEqual(path.read_bytes(),replacement)
        self.inventory([(original,1),(replacement,1)])
        self.clean_private()

    def test_failed_attempt_consumes_duplicate_inode_alias_without_retry(self):
        self.compile()
        path = self.message('new/source:2,FS',raw=b'Subject: one attempt\n\n'+b'\x00\xff'*70000)
        original = path.read_bytes()
        alias = str(path.parent)+'/./'+path.name
        rows = self.apply([path,alias],7,dict(JF_TEST_FAIL='store-write',JF_TEST_FAIL_AT='2'))
        self.assertEqual([row['path'] for row in rows],[os.fsencode(path),os.fsencode(alias)])
        self.assertEqual([row['status'] for row in rows],[b'error',b'error'])
        self.assertEqual([row['detail'] for row in rows],[b'E_COPY_WRITE',b'E_MESSAGE_CHANGED'])
        self.assertEqual(path.read_bytes(),original)
        self.inventory([(original,1)])
        self.clean_private()
        self.counts['fault_cases'] += 1

    def test_flag_rename_before_claim_is_retained_with_error(self):
        self.compile()
        path = self.message('new/source:2,S')
        original = path.read_bytes()
        renamed = self.incoming/'cur/source:2,FS'
        with self.held('store-before-claim',[path]) as handle:
            path.rename(renamed)
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['status'],b'error')
        self.assertEqual(renamed.read_bytes(),original)
        self.inventory([(original,2)])
        self.clean_private()

    def test_rollback_collision_keeps_both_replacements_in_recovery(self):
        self.compile()
        path = self.message('new/source:2,S',raw=b'Subject: original\n\nold')
        original = path.read_bytes()
        a,b = b'Subject: replacement A\n\nA',b'Subject: replacement B\n\nB'
        with self.held('store-before-claim,store-before-restore',[path]) as handle:
            temporary = self.message('new/replacement',raw=a)
            os.replace(temporary,path)
            handle.resume()
            handle.ready()
            self.assertFalse(path.exists())
            path.write_bytes(b)
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['detail'],b'E_SOURCE_RECOVERY')
        self.assertEqual(path.read_bytes(),b)
        claimed = list(self.incoming.glob('new/.jaro-filter.*/source:2,S'))
        self.assertEqual(len(claimed),1)
        self.assertEqual(claimed[0].read_bytes(),a)
        self.inventory([(original,1),(a,1),(b,1)])

    def test_symlink_or_directory_replacement_is_never_deleted(self):
        for kind in ['symlink','directory']:
            self.fresh_root(kind)
            path = self.message('new/source:2,S')
            data = path.read_bytes()
            outside = self.case/('outside-'+kind)
            outside.write_bytes(b'Subject: foreign '+kind.encode()+b'\n\nkept')
            with self.held('store-before-claim',[path]) as handle:
                path.unlink()
                if kind=='symlink':
                    path.symlink_to(outside)
                else:
                    path.mkdir()
                    (path/'nested').write_bytes(outside.read_bytes())
                self.finish(handle,7)
            if kind=='symlink':
                self.assertTrue(path.is_symlink())
            else:
                self.assertTrue(path.is_dir())
                self.assertEqual((path/'nested').read_bytes(),outside.read_bytes())
            self.assertEqual((self.root/'unsorted/cur/source:2,S').read_bytes(),data)
            self.clean_private()

    def test_observed_body_edits_before_copy_and_unlink_are_recoverable(self):
        for phase in ['store-staging-created','store-before-unlink']:
            self.fresh_root(phase)
            path = self.message('new/source',raw=b'Subject: edit\n\nold body')
            original = path.read_bytes()
            writer = os.open(path,os.O_WRONLY)
            try:
                with self.held(phase,[path]) as handle:
                    changed = original+b' appended' if phase=='store-staging-created' else original[:-1]+b'X'
                    self.assertEqual(os.pwrite(writer,changed,0),len(changed))
                    rows = self.finish(handle,7)
            finally:
                os.close(writer)
            self.assertEqual(rows[0]['detail'],b'E_SOURCE_CHANGED')
            self.assertEqual(path.read_bytes(),changed)
            self.inventory([(changed,1)]+([(original,1)] if phase=='store-before-unlink' else []))
            self.clean_private()

    def test_destination_replacement_cannot_redirect_transfer(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        outside = self.case/'outside'
        outside.mkdir()
        with self.held('store-staging-created',[path]) as handle:
            target = self.root/'unsorted/cur'
            target.rename(self.root/'unsorted/previous-cur')
            target.symlink_to(outside,target_is_directory=True)
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['detail'],b'E_DESTINATION_CHANGED')
        self.assertEqual(path.read_bytes(),data)
        self.assertEqual(list(outside.iterdir()),[])
        self.clean_private()

    def test_stage_corruption_is_detected_before_publication(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        with self.held('store-before-verify',[path]) as handle:
            staged = list((self.root/'unsorted/tmp').glob('.jaro-filter.*/message'))
            self.assertEqual(len(staged),1)
            with staged[0].open('r+b') as stream:
                stream.write(b'CORRUPT')
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['detail'],b'E_COPY_VERIFY')
        self.inventory([(data,1)])
        self.clean_private()

    def test_published_copy_corruption_retains_source_and_reports_failure(self):
        self.compile()
        path = self.message('new/source',raw=b'Subject: durable\n\noriginal')
        original = path.read_bytes()
        changed = original[:-1]+b'X'
        with self.held('store-before-unlink',[path]) as handle:
            destination = self.root/'unsorted/cur/source'
            destination.write_bytes(changed)
            rows = self.finish(handle,7)
        self.assertEqual(rows[0]['detail'],b'E_DESTINATION_CHANGED')
        self.assertEqual(path.read_bytes(),original)
        self.inventory([(original,1),(changed,1)])
        self.clean_private()

    def test_headers_changed_after_parsing_cannot_route_replacement_content(self):
        self.compile(['from-address old@example.org move old','from-address new@example.org move new'])
        path = self.message('new/source',raw=b'From: old@example.org\nSubject: edit\n\nbody')
        changed = b'From: new@example.org\nSubject: edit\n\nbody'
        with self.held('headers-read',[path]) as handle:
            path.write_bytes(changed)
            rows = self.finish(handle,7)
        self.assertIn(rows[0]['detail'],[b'E_SOURCE_CHANGED',b'E_MESSAGE_CHANGED'])
        self.assertEqual(path.read_bytes(),changed)
        self.inventory([(changed,1)])
        self.clean_private()

    def test_publication_collision_retries_are_bounded_and_preserve_long_flags(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        flags = ':2,'+'F'*251
        original = b'Subject: old collision\n\nold'
        for prefix in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz':
            (self.root/'unsorted/cur'/(prefix+flags)).write_bytes(original)
        path = self.message('new/s'+flags,raw=b'Subject: new collision\n\nnew')
        incoming = path.read_bytes()
        rows = self.apply([path],7,dict(JF_TEST_TRACE='1'))
        self.assertEqual(rows[0]['detail'],b'E_DESTINATION_COLLISION')
        stderr = Path(self.events[-1]['stderr']['path']).read_bytes()
        self.assertEqual(stderr.splitlines().count(b'TRACE:store-publish-attempt'),64)
        self.assertEqual(path.read_bytes(),incoming)
        self.inventory([(original,62),(incoming,1)])
        self.clean_private()

    def test_owned_directory_collision_never_adopts_or_removes_foreign_claim(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        foreign_body = b'Subject: other owner\n\nretained'
        with self.held('store-before-publish',[path]) as handle:
            staged = list((self.root/'unsorted/tmp').glob('.jaro-filter.*'))
            self.assertEqual(len(staged),1)
            foreign = path.parent/(staged[0].name.rsplit('.',1)[0]+'.2')
            foreign.mkdir()
            (foreign/'message').write_bytes(foreign_body)
            self.finish(handle)
        self.assertEqual((foreign/'message').read_bytes(),foreign_body)
        self.inventory([(data,1),(foreign_body,1)])
        self.clean_private([foreign])

    def test_apply_maildir_snapshot_defers_additions_and_preserves_malformed_files(self):
        self.compile()
        path = self.message('new/selected')
        selected = path.read_bytes()
        command = list(map(str,[self.objects/'jaro-filter-test','--root',self.root,'--mode','apply','--maildir',self.incoming]))
        with self.held('batch-locked',[],command) as handle:
            later = self.message('new/later',raw=b'Subject: added later\n\nnew')
            later_bytes = later.read_bytes()
            rows = self.finish(handle)
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['path'],os.fsencode(path))
        self.assertEqual(later.read_bytes(),later_bytes)
        empty = self.message('new/empty',raw=b'')
        malformed = self.message('new/malformed',raw=b'Subject missing colon\n\nbody')
        broken = malformed.read_bytes()
        rows = self.apply([empty,malformed,later],7)
        self.assertEqual([row['status'] for row in rows],[b'error',b'error',b'moved'])
        self.assertEqual(empty.read_bytes(),b'')
        self.assertEqual(malformed.read_bytes(),broken)
        self.inventory([(selected,1),(later_bytes,1),(b'',1),(broken,1)])
        self.clean_private()

    def test_apply_is_pinned_to_one_generation_across_publication(self):
        self.compile(['from-address x@example.org move old'])
        old = self.generation
        path = self.message('new/source',{'From':'x@example.org'})
        data = path.read_bytes()
        with self.held('batch-locked',[path]) as handle:
            self.compile(['from-address x@example.org move new'])
            self.assertNotEqual(old,self.generation)
            handle.resume()
            code,out,err = handle.finish()
            self.assertEqual(code,0,err)
            rows = self.decode(subprocess.CompletedProcess(handle.process.args,code,out,err),old)
        self.assertEqual(rows[0]['logical'],b'old')
        self.assertEqual((self.root/'old/cur/source').read_bytes(),data)
        self.inventory([(data,1)])
        self.clean_private()

    def test_concurrent_batches_use_persistent_owned_lock(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        with self.held('batch-locked',[path]) as handle:
            lock = self.root/'cache/filter-engine/batch.lock'
            identity = lock.stat().st_ino
            result = self.command(self.argv([path]),6)
            self.assertEqual(result.stdout,b'')
            self.assertEqual(lock.stat().st_ino,identity)
            self.assertEqual(path.read_bytes(),data)
            self.finish(handle)
        self.assertEqual(lock.stat().st_ino,identity)
        self.inventory([(data,1)])
        self.clean_private()

    def test_lock_fsync_failure_starts_no_message_operation(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        args = self.argv([path],True)
        result = subprocess.run(args,capture_output=True,env=dict(os.environ,JF_TEST_FAIL='store-sync-lock-file'))
        self.capture(args,result,extra=dict(environment={'JF_TEST_FAIL':'store-sync-lock-file'}))
        self.assertEqual(result.returncode,3,result.stderr)
        self.assertEqual(result.stdout,b'')
        self.assertEqual(path.read_bytes(),data)
        lock = self.root/'cache/filter-engine/batch.lock'
        original_inode = lock.stat().st_ino
        self.apply([path])
        self.assertEqual(lock.stat().st_ino,original_inode)
        self.inventory([(data,1)])
        self.clean_private()

    def test_explicit_lock_path_cannot_release_the_batch_record_lock(self):
        self.compile()
        path = self.message('new/source')
        another = self.message('new/another',raw=b'Subject: another\n\nwaiting')
        lock = self.root/'cache/filter-engine/batch.lock'
        lock.write_bytes(b''); lock.chmod(0o600)
        with self.held('store-before-unlink',[lock,path]) as handle:
            self.assertEqual(self.command(self.argv([another]),6).stdout,b'')
            rows = self.finish(handle,7)
        self.assertEqual([r['status'] for r in rows],[b'error',b'moved'])
        self.assertEqual(rows[0]['detail'],b'E_MESSAGE_TYPE')
        self.assertEqual(lock.read_bytes(),b'')
        self.assertTrue(another.exists())
        self.clean_private()

    def test_success_durability_and_claim_order(self):
        self.compile()
        path = self.message('new/source')
        self.apply([path],extra=dict(JF_TEST_TRACE='1'))
        lines = Path(self.events[-1]['stderr']['path']).read_bytes().splitlines()
        ordered = ['store-sync-file','store-before-verify','store-publish-attempt','store-published',
                   'store-sync-published-file','store-sync-published','store-cleanup-stage','store-sync-stage',
                   'store-before-claim','store-source-claimed','store-sync-claim','store-sync-source-claim',
                   'store-before-unlink','store-unlink','store-sync-source-unlinked']
        positions = [lines.index(('TRACE:'+phase).encode()) for phase in ordered]
        self.assertEqual(positions,sorted(positions))
        self.clean_private()

    def test_lock_replacement_never_removes_or_unlocks_another_owner_inode(self):
        self.compile()
        path = self.message('new/source')
        data = path.read_bytes()
        lock = self.root/'cache/filter-engine/batch.lock'
        with self.held('store-before-unlink',[path]) as handle:
            old_inode = lock.stat().st_ino
            lock.unlink()
            owner = lock.open('w+b')
            lock.chmod(0o600)
            owner.write(b'another owner'); owner.flush()
            fcntl.lockf(owner,fcntl.LOCK_EX|fcntl.LOCK_NB)
            new_inode = lock.stat().st_ino
            self.assertNotEqual(new_inode,old_inode)
            try:
                rows = self.finish(handle,7)
                self.assertEqual(rows[0]['detail'],b'E_BATCH_LOCK_CHANGED')
                self.assertEqual(lock.stat().st_ino,new_inode)
                # Reading via a second descriptor and closing it would release
                # this test process's own POSIX lock before the busy probe.
                owner.seek(0)
                self.assertEqual(owner.read(),b'another owner')
                self.assertEqual(self.command(self.argv([path]),6).stdout,b'')
            finally:
                owner.close()
        self.assertEqual(path.read_bytes(),data)
        self.inventory([(data,2)])
        self.clean_private()

    def test_foreign_lock_and_temp_artifacts_are_preserved(self):
        self.compile()
        self.maildir(self.root/'unsorted')
        foreign = self.root/'unsorted/tmp/.jaro-filter.foreign'
        foreign.mkdir()
        (foreign/'message').write_bytes(b'Subject: foreign\n\nkept')
        path = self.message('new/source')
        self.apply([path])
        self.assertEqual((foreign/'message').read_bytes(),b'Subject: foreign\n\nkept')
        self.clean_private()
        lock = self.root/'cache/filter-engine/batch.lock'
        lock.unlink()
        outside = self.case/'foreign-lock'
        outside.write_bytes(b'other owner lock')
        lock.symlink_to(outside)
        path = self.message('new/another')
        self.assertEqual(self.command(self.argv([path]),3).stdout,b'')
        self.assertTrue(lock.is_symlink())
        self.assertEqual(outside.read_bytes(),b'other owner lock')

    def test_graceful_interruptions_restore_or_retain_exact_copies(self):
        for phase in ['store-staging-created','store-published','store-source-claimed','store-before-unlink']:
            self.fresh_root(phase)
            path = self.message('new/source')
            data = path.read_bytes()
            with self.held(phase,[path]) as handle:
                handle.process.send_signal(signal.SIGTERM)
                self.finish(handle,128+signal.SIGTERM)
            self.assertTrue(path.exists())
            self.inventory([(data,1 if phase=='store-staging-created' else 2)])
            self.clean_private()

    def test_kill_after_claim_retains_owned_copy_and_releases_lock(self):
        self.compile()
        path = self.message('new/source:2,FS')
        data = path.read_bytes()
        with self.held('store-source-claimed',[path]) as handle:
            handle.process.kill()
            actual,out,err = handle.finish()
            self.assertEqual(actual,-signal.SIGKILL)
        self.assertFalse(path.exists())
        claimed = list(self.incoming.glob('new/.jaro-filter.*/source:2,FS'))
        self.assertEqual(len(claimed),1)
        self.assertEqual(claimed[0].read_bytes(),data)
        self.inventory([(data,2)])
        lock_inode = (self.root/'cache/filter-engine/batch.lock').stat().st_ino
        another = self.message('new/next',raw=b'Subject: next\n\nnext')
        self.apply([another])
        self.assertEqual((self.root/'cache/filter-engine/batch.lock').stat().st_ino,lock_inode)
        self.assertEqual(claimed[0].read_bytes(),data,'next batch must not adopt another process claim')


if __name__=='__main__':
    program = unittest.main(exit=False,verbosity=2)
    print('Storage counts: '+json.dumps(getattr(StorageTests,'counts',{}),sort_keys=True))
    raise SystemExit(not program.result.wasSuccessful())
