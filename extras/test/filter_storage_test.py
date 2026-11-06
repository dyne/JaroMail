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
import select
import time
import resource
import re
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
        self.capture(args,subprocess.CompletedProcess(args,code,out or b'',err),extra=dict(
            stdout_not_captured=out is None,environment={k:v for k,v in env.items() if k.startswith('JF_TEST_')}))

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

    def signal_schedule(self,mode,signum):
        self.fresh_root('%s-%d'%(mode,signum))
        path=self.message('new/source');data=path.read_bytes();original_mode=stat.S_IMODE(path.stat().st_mode)
        notify,writer=os.pipe();reader,release=os.pipe()
        io_notify,io_writer=os.pipe();io_reader,io_release=os.pipe()
        command=list(map(str,[self.objects/'jaro-filter-signal-test','--root',self.root,'--mode','apply','--',path]))
        env=dict(os.environ,JF_SIGNAL_SCHEDULE=mode,JF_SIGNAL_NOTIFY_FD=str(writer),JF_SIGNAL_RELEASE_FD=str(reader),
                 JF_TEST_HOLD='store-before-unlink,store-before-restore,store-unlink',
                 JF_TEST_NOTIFY_FD=str(io_writer),JF_TEST_RELEASE_FD=str(io_reader),JF_TEST_TRACE='1')
        process=subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                 pass_fds=(writer,reader,io_writer,io_reader))
        for fd in (writer,reader,io_writer,io_reader):os.close(fd)
        controls=[];out=err=b''
        def ready(fd,wanted):
            self.assertTrue(select.select([fd],[],[],10)[0],'signal schedule precondition not reached')
            self.assertEqual(os.read(fd,1),wanted);controls.append(wanted.decode())
        try:
            ready(notify,b'M');ready(io_notify,b'1')
            self.assertGreaterEqual(len(list(Path('/proc/%d/task'%process.pid).iterdir())),18)
            process.send_signal(signum)
            if mode=='consumed':
                os.write(release,b'1');ready(notify,b'C')
                os.write(io_release,b'1');ready(notify,b'W')
                # The real wait returned, but publication is deliberately held.
                # The worker has attempted its stop lock; releasing the consumer
                # must publish stop before that check can return.
                os.write(release,b'1')
                ready(io_notify,b'1')
            else:
                os.write(io_release,b'1');ready(io_notify,b'1')
                # The worker has reached restore while the manager remains held
                # before consumption: its own pending-signal check must suffice.
                os.write(release,b'1')
            os.write(io_release,b'1')
            out,err=process.communicate(timeout=15)
        finally:
            if process.poll() is None:
                for fd in (release,io_release):
                    try:os.write(fd,b'1'*64)
                    except BrokenPipeError:pass
                try:out,err=process.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill();out,err=process.communicate(timeout=5)
            for fd in (notify,release,io_notify,io_release):os.close(fd)
            self.capture(command,subprocess.CompletedProcess(command,process.returncode,out,err),extra=dict(
                environment={k:v for k,v in env.items() if k.startswith(('JF_TEST_','JF_SIGNAL_'))},
                schedule_controls=controls,settled=process.poll() is not None))
            self.counts['controlled_children']+=1
            self.counts['unsettled_children']+=process.poll() is None
        self.assertEqual(process.returncode,128+signum,err)
        rows=self.decode(subprocess.CompletedProcess(command,process.returncode,out,err))
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['status'],b'error')
        self.assertEqual(rows[0]['detail'],b'E_INTERRUPTED')
        self.assertEqual(path.read_bytes(),data);self.assertEqual(stat.S_IMODE(path.stat().st_mode),original_mode)
        inventory=self.inventory([(data,2)])
        self.assertEqual({bytes.fromhex(r['path_hex']) for r in inventory},
                         {os.fsencode(path),os.fsencode(self.root/'unsorted/cur/source')})
        self.assertEqual(stat.S_IMODE((self.root/'unsorted/cur/source').stat().st_mode),0o600)
        self.clean_private()

    def test_pool_pending_signals_stop_at_boundary_before_manager_runs(self):
        for signum in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP,signal.SIGPIPE]:
            with self.subTest(signum=signum):self.signal_schedule('pending',signum)

    def test_pool_consumed_signals_publish_stop_under_same_boundary_lock(self):
        for signum in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP,signal.SIGPIPE]:
            with self.subTest(signum=signum):self.signal_schedule('consumed',signum)

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

    def pool_messages(self,count=40,large=False):
        self.compile()
        paths=[]
        for i in range(count):
            raw=b'From: pool@example.org\r\nSubject: pool\r\n\r\n'+bytes([i%251])* (180000 if large and not i else 128)
            paths.append(self.message('new/pool-%03d:2,S'%i,raw=raw))
        self.pool_modes={p:stat.S_IMODE(p.stat().st_mode) for p in paths}
        bodies=[p.read_bytes() for p in paths]
        self.pool_names=dict(zip(bodies,[p.name for p in paths]))
        return paths,bodies

    @contextmanager
    def pool_held(self,phase,paths,extra=None):
        self.env=dict(os.environ);self.held=[]
        handle=Held(self,phase,command=self.argv(paths,True),extra=dict(JF_TEST_JOB='1',**(extra or {})))
        try:
            handle.ready();yield handle
        finally:
            if not handle.finished:
                try:os.write(handle.release,b'1'*64)
                except BrokenPipeError:pass
            handle.close()
            self.counts['controlled_children']+=1
            self.counts['unsettled_children']+=handle.process.poll() is None
            del self.held

    def wait_removed(self,path,handle):
        deadline=time.monotonic()+10
        while path.exists() and time.monotonic()<deadline:
            self.assertIsNone(handle.process.poll())
            time.sleep(.01)
        self.assertFalse(path.exists(),'independent job did not complete while job 1 was held')

    def pool_rows(self,handle,code=0):
        handle.resume();rc,out,err=handle.finish()
        self.assertEqual(rc,code,err)
        return self.decode(subprocess.CompletedProcess([],rc,out,err)),err

    def pool_sources(self,paths,bodies):
        return {p:(body,self.pool_modes[p]) for p,body in zip(paths,bodies)}

    def assert_pool_bodies(self,bodies,extra=(),*,copies=None,sources=None,foreign=None,destinations=None,fixed=None,alternatives=(),private_directories=()):
        # Every message entry is accounted, including private/foreign copies.
        # Fault callers supply finite bounds justified by their phase/rows;
        # success defaults to exactly one survivor per selected input.
        sources=sources or {};foreign=foreign or {};fixed=fixed or {}
        destinations=destinations or [self.root/'unsorted/cur']
        expected=collections.Counter(bodies)+collections.Counter(extra)
        bounds={body:(count,count) for body,count in expected.items()}
        bounds.update(copies or {})
        actual=collections.Counter();inventory=body_files(self.root);seen=set()
        for row in inventory:
            path=Path(os.fsdecode(bytes.fromhex(row['path_hex'])));data=path.read_bytes();seen.add(path)
            self.assertTrue(stat.S_ISREG(path.lstat().st_mode))
            if path in fixed:
                wanted,mode=fixed[path];self.assertEqual((data,row['mode']),(wanted,mode));continue
            actual[data]+=1
            if path in sources:
                wanted,mode=sources[path];self.assertEqual((data,row['mode']),(wanted,mode))
                if path.parent.name.startswith('.jaro-filter.'):
                    self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode),0o700)
            elif path in foreign:
                wanted,mode=foreign[path];self.assertEqual((data,row['mode']),(wanted,mode))
            else:
                self.assertIn(path.parent,destinations,'unexpected message/stage/private/foreign path')
                self.assertEqual(row['mode'],0o600)
                self.assertTrue(path.name.endswith(':2,S') or path.name.startswith('jaro.'))
                wanted_name=getattr(self,'pool_names',{}).get(data)
                if data not in alternatives:self.assertEqual(path.name,wanted_name,'body/input filename mismatch')
                else:
                    self.assertTrue(path.name==wanted_name or re.fullmatch(r'(?:jaro\.[0-9a-f]{8}\.[0-9a-f]+\.[0-9]+|[0-9A-Za-z]{1,79}):2,S',path.name))
        for body in [*bodies,*extra]:self.assertIn(body,actual)
        self.assertEqual(set(actual),set(bounds),'foreign, partial or missing body')
        for body,(minimum,maximum) in bounds.items():
            self.assertGreaterEqual(actual[body],minimum)
            self.assertLessEqual(actual[body],maximum,'unexpected duplicate')
        self.assertTrue(set(sources)|set(foreign)|set(fixed)<=seen)
        allowed_private=set(private_directories)|{p.parent for p in [*sources,*fixed] if p.parent.name.startswith('.jaro-filter.')}
        for parent,dirs,files in os.walk(self.root,followlinks=False):
            if 'cache' in dirs:dirs.remove('cache')
            relative=Path(parent).relative_to(self.root).parts
            if not any(part in ('cur','new','tmp') for part in relative):continue
            for name in dirs:
                entry=Path(parent)/name
                self.assertFalse(entry.is_symlink());self.assertIn(entry,allowed_private,'unexpected private/foreign directory')
                self.assertEqual(stat.S_IMODE(entry.stat().st_mode),0o700)
            for name in files:
                entry=Path(parent)/name
                self.assertFalse(entry.is_symlink());self.assertIn(entry,seen,'unexpected nonregular/foreign entry')
        (self.case/('pool-inventory-%04d.json'%self.counts['body_inventories'])).write_text(json.dumps(dict(
            files=inventory,bounds=[dict(sha256=digest(body),bytes=len(body),minimum=lo,maximum=hi) for body,(lo,hi) in bounds.items()],
            sources=[str(p) for p in sources],foreign=[str(p) for p in foreign],fixed=[str(p) for p in fixed],
            destinations=list(map(str,destinations))),sort_keys=True,indent=2)+'\n')
        self.counts['body_inventories']+=1

    def pool_interrupted_bounds(self,paths,bodies,rows):
        sources={};bounds={}
        for index,(path,body) in enumerate(zip(paths,bodies)):
            if index>=len(rows):
                self.assertTrue(path.exists());sources.update(self.pool_sources([path],[body]));continue
            row=rows[index]
            if row['status']==b'moved':self.assertFalse(path.exists())
            else:
                self.assertEqual(row['status'],b'error');self.assertTrue(path.exists())
                sources.update(self.pool_sources([path],[body]))
                # A cooperative singleton can stop before or after publication;
                # at most one complete public duplicate may remain.
                bounds[body]=(1,2)
        return sources,bounds

    def test_pool_overlaps_singletons_and_bounds_window_threads_descriptors(self):
        paths,bodies=self.pool_messages(65)
        with self.pool_held('store-before-claim',paths) as h:
            self.wait_removed(paths[1],h)
            self.assertTrue(paths[0].exists())
            self.assertTrue(all(p.exists() for p in paths[32:]),'next window admitted before ordered output')
            tasks=list(Path('/proc/%s/task'%h.process.pid).iterdir())
            self.assertIn(len(tasks),(18,19)) # TSan may own one runtime helper.
            self.assertLessEqual(len(list(Path('/proc/%s/fd'%h.process.pid).iterdir())),240)
            self.events.append(dict(control='pool-resource-observation',tasks=len(tasks),fds=len(list(Path('/proc/%s/fd'%h.process.pid).iterdir())),prepared_bound=32,later_sources_intact=all(p.exists() for p in paths[32:])))
            # Only the flushed header is observable before the window settles.
            self.assertTrue(select.select([h.process.stdout],[],[],1)[0])
            header=os.read(h.process.stdout.fileno(),4096)
            self.assertEqual(header,b'JFD1\0'+self.generation+b'\0')
            h.resume();rc,out,err=h.finish()
            self.assertEqual(rc,0,err)
            rows=self.decode(subprocess.CompletedProcess([],rc,header+out,err))
        self.assertEqual([r['status'] for r in rows],[b'moved']*65)
        self.assert_pool_bodies(bodies)

    def test_pool_fault_order_preserves_first_error_next_moved(self):
        paths,bodies=self.pool_messages(8,True)
        rows=self.apply(paths,7,dict(JF_TEST_FAIL='store-write',JF_TEST_FAIL_AT='2',JF_TEST_TRACE='1'))
        self.assertEqual([r['status'] for r in rows],[b'error']+[b'moved']*7)
        self.assertEqual(rows[0]['detail'],b'E_COPY_WRITE')
        self.assertEqual(paths[0].read_bytes(),bodies[0])
        self.assert_pool_bodies(bodies,sources=self.pool_sources(paths[:1],bodies[:1]))

    def test_pool_ungated_error_does_not_cancel_other_singletons(self):
        paths,bodies=self.pool_messages(16)
        rows=self.apply(paths,7,dict(JF_TEST_FAIL='store-sync-published',JF_TEST_JOB='2'))
        self.assertEqual([r['status'] for r in rows],[b'moved',b'error']+[b'moved']*14)
        self.assertEqual(rows[1]['detail'],b'E_DESTINATION_FSYNC')
        self.assertEqual(paths[1].read_bytes(),bodies[1])
        self.assert_pool_bodies(bodies,copies={bodies[1]:(2,2)},sources=self.pool_sources(paths[1:2],bodies[1:2]))

    def test_pool_ungated_source_mutation_retains_new_body(self):
        paths,bodies=self.pool_messages()
        changed=bodies[0]+b'concurrent body edit'
        with self.pool_held('store-before-claim',paths) as h:
            self.wait_removed(paths[1],h);paths[0].write_bytes(changed)
            rows,_=self.pool_rows(h,7)
        self.assertEqual(rows[0]['status'],b'error')
        self.assertEqual(paths[0].read_bytes(),changed)
        self.assert_pool_bodies(bodies,[changed],sources=self.pool_sources(paths[:1],[changed]))

    def test_pool_ungated_destination_corruption_preserves_source(self):
        paths,bodies=self.pool_messages()
        with self.pool_held('store-before-claim',paths) as h:
            self.wait_removed(paths[1],h)
            target=next(self.root.rglob(paths[0].name))
            targets=[p for p in self.root.rglob(paths[0].name) if p!=paths[0]]
            self.assertEqual(len(targets),1);targets[0].write_bytes(b'corrupted destination')
            rows,_=self.pool_rows(h,7)
        self.assertEqual(rows[0]['status'],b'error')
        self.assertEqual(paths[0].read_bytes(),bodies[0])
        self.assert_pool_bodies(bodies,[b'corrupted destination'],sources=self.pool_sources(paths[:1],bodies[:1]),foreign={targets[0]:(b'corrupted destination',0o600)})

    def test_pool_ungated_destination_rename_preserves_pinned_copies(self):
        paths,bodies=self.pool_messages(16)
        with self.pool_held('store-before-claim',paths) as h:
            self.wait_removed(paths[1],h)
            target=next(p for p in self.root.rglob(paths[0].name) if p!=paths[0])
            leaf=target.parent.parent;renamed=leaf.with_name('renamed-destination')
            leaf.rename(renamed);self.maildir(leaf)
            rows,_=self.pool_rows(h,7)
        self.assertEqual(rows[0]['status'],b'error');self.assertTrue(paths[0].exists())
        sources,bounds=self.pool_interrupted_bounds(paths,bodies,rows);bounds[bodies[0]]=(2,2)
        self.assert_pool_bodies(bodies,copies=bounds,sources=sources,destinations=[leaf/'cur',renamed/'cur'])

    def test_pool_publication_pin_survives_parallel_old_generation(self):
        paths,bodies=self.pool_messages(40)
        generation=self.generation
        with self.pool_held('store-before-claim',paths) as h:
            self.wait_removed(paths[1],h)
            (self.root/'Filters.txt').write_text('from-address pool@example.org move Other\n')
            published=self.command([self.compiler,'--root',self.root,'--report0']).stdout.split(b'\0')
            self.assertNotEqual(published[1],generation)
            rows,_=self.pool_rows(h)
        self.assertEqual(len(rows),40)
        self.assertTrue(all(r['logical']==b'unsorted' for r in rows))
        self.assert_pool_bodies(bodies)

    def test_pool_managed_signals_restore_claim_and_settle_workers(self):
        for signum in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP]:
            self.fresh_root('signal-%d'%signum);paths,bodies=self.pool_messages(40)
            with self.pool_held('store-source-claimed',paths) as h:
                self.wait_removed(paths[1],h)
                h.process.send_signal(signum)
                # Observe cancellation before releasing the held singleton.
                time.sleep(.05)
                rows,err=self.pool_rows(h,128+signum)
            self.assertTrue(paths[0].exists());self.assertTrue(all(p.exists() for p in paths[32:]))
            self.assertEqual(rows[0]['status'],b'error');self.assertEqual(rows[0]['detail'],b'E_INTERRUPTED')
            sources,bounds=self.pool_interrupted_bounds(paths,bodies,rows);bounds[bodies[0]]=(2,2)
            self.assert_pool_bodies(bodies,copies=bounds,sources=sources)

    def test_pool_signal_restore_never_clobbers_new_arrival(self):
        paths,bodies=self.pool_messages(40)
        with self.pool_held('store-source-claimed',paths) as h:
            self.wait_removed(paths[1],h);paths[0].write_bytes(b'new arrival')
            h.process.send_signal(signal.SIGTERM);time.sleep(.05)
            rows,err=self.pool_rows(h,143)
        self.assertEqual(paths[0].read_bytes(),b'new arrival')
        self.assertEqual(rows[0]['status'],b'error');self.assertIn(b'E_RETAINED_PATH',err)
        claims=list(self.incoming.glob('new/.jaro-filter.*/'+paths[0].name));self.assertEqual(len(claims),1)
        sources,bounds=self.pool_interrupted_bounds(paths[1:],bodies[1:],rows[1:])
        sources[claims[0]]=(bodies[0],self.pool_modes[paths[0]]);bounds[bodies[0]]=(2,2)
        self.assert_pool_bodies(bodies,[b'new arrival'],copies=bounds,sources=sources,foreign={paths[0]:(b'new arrival',self.pool_modes[paths[0]])})

    def pool_killed_inventory(self,paths,bodies):
        # SIGKILL may leave one original/claim, one published copy and one
        # incomplete stage per active job. Each stage is assigned injectively
        # to a still-original admitted input whose bytes it prefixes.
        inventory=body_files(self.root);orig=[0]*len(paths);claimed=[0]*len(paths);published=[0]*len(paths)
        indexes={body:i for i,body in enumerate(bodies)};names={p.name:i for i,p in enumerate(paths)}
        stages=[];fixed={};private_dirs=list(self.root.rglob('.jaro-filter.*'))
        self.assertLessEqual(len(private_dirs),32)
        for directory in private_dirs:
            self.assertTrue(directory.is_dir());self.assertFalse(directory.is_symlink())
            self.assertIn(directory.parent,[self.incoming/'new',self.root/'unsorted/tmp'])
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode),0o700)
            self.assertLessEqual(len(list(directory.iterdir())),1)
        for row in inventory:
            path=Path(os.fsdecode(bytes.fromhex(row['path_hex'])));data=path.read_bytes()
            if path in paths:
                i=paths.index(path);orig[i]+=1;self.assertEqual(data,bodies[i]);self.assertEqual(row['mode'],self.pool_modes[path])
            elif path.parent.parent==self.incoming/'new' and path.parent.name.startswith('.jaro-filter.'):
                self.assertIn(path.name,names);i=names[path.name];claimed[i]+=1
                self.assertEqual(data,bodies[i]);self.assertEqual(row['mode'],self.pool_modes[paths[i]])
                fixed[path]=(data,row['mode'],path.stat().st_dev,path.stat().st_ino)
            elif path.parent==self.root/'unsorted/cur':
                self.assertIn(data,indexes);i=indexes[data];published[i]+=1
                self.assertEqual(path.name,paths[i].name);self.assertEqual(row['mode'],0o600)
            else:
                self.assertEqual(path.parent.parent,self.root/'unsorted/tmp');self.assertTrue(path.parent.name.startswith('.jaro-filter.'))
                self.assertEqual(path.name,'message');self.assertEqual(row['mode'],0o600)
                stages.append((path,data));fixed[path]=(data,row['mode'],path.stat().st_dev,path.stat().st_ino)
        for i in range(len(paths)):
            self.assertLessEqual(orig[i]+claimed[i],1);self.assertLessEqual(published[i],1)
            self.assertIn(orig[i]+claimed[i]+published[i],(1,2))
            if claimed[i]:self.assertEqual(published[i],1,'claim requires a complete durable publication')
            if i>=32:self.assertEqual((orig[i],claimed[i],published[i]),(1,0,0))
        self.assertEqual((orig[0],claimed[0],published[0]),(0,1,1))
        self.assertLessEqual(len(stages),15) # Held claim consumes one of16workers.
        assigned={}
        def assign(stage,seen):
            data=stages[stage][1]
            for i,body in enumerate(bodies[:32]):
                if i in seen or not orig[i] or not body.startswith(data) or (published[i] and data!=body):continue
                seen.add(i)
                if i not in assigned or assign(assigned[i],seen):assigned[i]=stage;return True
            return False
        for stage in range(len(stages)):self.assertTrue(assign(stage,set()),'partial/foreign/duplicate stage has no distinct eligible input')
        (self.case/'pool-killed-inventory.json').write_text(json.dumps(dict(files=inventory,
            originals=orig,claims=claimed,published=published,stage_assignments=assigned,
            private_directories=list(map(str,private_dirs))),sort_keys=True,indent=2)+'\n')
        self.counts['body_inventories']+=1
        return orig,published,fixed,set(private_dirs)

    def test_pool_kill_retains_private_claim_without_adopting_it_on_retry(self):
        paths,bodies=self.pool_messages(40)
        with self.pool_held('store-source-claimed',paths) as h:
            self.wait_removed(paths[1],h);h.process.kill()
            rc,out,err=h.finish();self.assertEqual(rc,-signal.SIGKILL)
            self.assertEqual(out,b'JFD1\0'+self.generation+b'\0')
        self.assertFalse(paths[0].exists())
        original,published,fixed,private_dirs=self.pool_killed_inventory(paths,bodies)
        claims=list(self.incoming.glob('new/.jaro-filter.*/'+paths[0].name));self.assertEqual(len(claims),1)
        rows=self.apply([p for p in paths if p.exists()]);self.assertTrue(all(r['status']==b'moved' for r in rows))
        self.assertEqual(claims[0].read_bytes(),bodies[0])
        self.assertEqual(set(self.root.rglob('.jaro-filter.*')),private_dirs)
        for path,(data,mode,dev,ino) in fixed.items():
            self.assertEqual((path.read_bytes(),stat.S_IMODE(path.stat().st_mode),path.stat().st_dev,path.stat().st_ino),(data,mode,dev,ino))
        copies={body:(published[i]+original[i],published[i]+original[i]) for i,body in enumerate(bodies)}
        self.assert_pool_bodies(bodies,copies=copies,fixed={p:(v[0],v[1]) for p,v in fixed.items()},alternatives={body for i,body in enumerate(bodies) if original[i] and published[i]},private_directories=private_dirs)

    def test_pool_header_output_failure_precedes_message_effects(self):
        paths,bodies=self.pool_messages()
        with open('/dev/full','wb') as output:
            r=subprocess.run(self.argv(paths),stdout=output,stderr=subprocess.PIPE,timeout=20)
        self.assertEqual(r.returncode,3,r.stderr)
        self.assertTrue(all(p.exists() for p in paths));self.assert_pool_bodies(bodies,sources=self.pool_sources(paths,bodies))

    def test_pool_real_sigpipe_header_failure_precedes_message_effects(self):
        paths,bodies=self.pool_messages();read,write=os.pipe();os.close(read)
        try:r=subprocess.run(self.argv(paths),stdout=write,stderr=subprocess.PIPE,timeout=20)
        finally:os.close(write)
        self.assertEqual(r.returncode,141,r.stderr)
        self.assertTrue(all(p.exists() for p in paths));self.assert_pool_bodies(bodies,sources=self.pool_sources(paths,bodies))

    def test_pool_midrecord_pipe_failure_closes_admission_and_reports_unreceived_moves(self):
        paths,bodies=self.pool_messages(100)
        # A closed reader after a complete header forces a record/flush error.
        # One window may finish durably; no later window may start.
        with self.pool_held('store-before-claim',paths) as h:
            header=os.read(h.process.stdout.fileno(),len(self.generation)+6)
            self.assertEqual(header,b'JFD1\0'+self.generation+b'\0')
            h.process.stdout.close();h.process.stdout=None
            h.resume();rc,out,err=h.finish()
        self.assertEqual(rc,141,err)
        self.assertTrue(all(p.exists() for p in paths[32:]))
        self.assertFalse(any(p.exists() for p in paths[:32]))
        self.assertGreaterEqual(err.count(b'E_RETAINED_PATH'),32)
        self.assert_pool_bodies(bodies,sources=self.pool_sources(paths[32:],bodies[32:]))

    def test_pool_resource_limit_falls_back_before_effects(self):
        paths,bodies=self.pool_messages(3)
        # Start with the production generic FD fallback, then hold the first
        # real singleton operation to observe that later jobs remain untouched.
        read,writer=os.pipe();reader,release=os.pipe()
        env=dict(os.environ,JF_TEST_HOLD='store-before-claim',JF_TEST_JOB='1',JF_TEST_NOTIFY_FD=str(writer),JF_TEST_RELEASE_FD=str(reader))
        def limit():resource.setrlimit(resource.RLIMIT_NOFILE,(64,64))
        child=subprocess.Popen(self.argv(paths,True),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,pass_fds=(writer,reader),preexec_fn=limit)
        os.close(writer);os.close(reader)
        try:
            self.assertTrue(select.select([read],[],[],20)[0]);self.assertEqual(os.read(read,1),b'1')
            self.assertLessEqual(len(list(Path('/proc/%s/task'%child.pid).iterdir())),2) # Includes a possible TSan helper.
            self.assertTrue(all(p.exists() for p in paths))
            os.write(release,b'1');out,err=child.communicate(timeout=20)
            self.assertEqual(child.returncode,0,err);self.assertEqual(len(self.decode(subprocess.CompletedProcess([],0,out,err))),3)
        finally:
            if child.poll() is None:child.kill();child.communicate()
            os.close(read);os.close(release)
            self.counts['controlled_children']+=1;self.counts['unsettled_children']+=child.poll() is None
        self.assert_pool_bodies(bodies)

    def test_pool_same_basename_dependency_falls_back_preserving_collision_order(self):
        self.compile();other=self.root/'other';other.mkdir()
        a=self.message('same:2,S',raw=b'From: one@example.org\n\nfirst')
        b=self.message('same:2,S',raw=b'From: two@example.org\n\nsecond',folder=other)
        with self.pool_held('store-before-claim',[a,b]) as h:
            self.assertLessEqual(len(list(Path('/proc/%s/task'%h.process.pid).iterdir())),2)
            self.assertTrue(b.exists());rows,_=self.pool_rows(h)
        self.assertEqual([r['status'] for r in rows],[b'moved',b'moved'])
        self.assertFalse(a.exists() or b.exists())
        dest=next(p for p in self.root.rglob('same:2,S'))
        self.assertEqual(dest.read_bytes(),b'From: one@example.org\n\nfirst')
        first=b'From: one@example.org\n\nfirst';second=b'From: two@example.org\n\nsecond'
        self.pool_names={first:a.name,second:b.name}
        self.assert_pool_bodies([first,second],alternatives={second})


    def test_pool_real_cross_filesystem_keeps_complete_singleton_protocol(self):
        paths,bodies=self.pool_messages(8)
        cross=EVIDENCE/('pool-cross-'+self.case.name);cross.mkdir(parents=True)
        self.assertNotEqual(cross.stat().st_dev,self.root.stat().st_dev,'real cross-filesystem prerequisite')
        external=[]
        for p in paths:
            target=cross/p.name;target.write_bytes(p.read_bytes());p.unlink();external.append(target)
        rows=self.apply(external)
        self.assertEqual([r['status'] for r in rows],[b'moved']*8)
        self.assertTrue(all(not p.exists() for p in external));self.assertEqual(list(cross.iterdir()),[]);self.assert_pool_bodies(bodies)
        self.counts['real_cross_filesystem']+=1

    def test_pool_inode_alias_and_noop_dependencies_keep_singleton_attempt_semantics(self):
        self.compile();self.maildir(self.root/'unsorted')
        noop=self.message('cur/noop:2,S',folder=self.root/'unsorted')
        alias=self.root/'unsorted/cur/alias:2,S';os.link(noop,alias)
        data=noop.read_bytes();mode=stat.S_IMODE(noop.stat().st_mode);identity=(noop.stat().st_dev,noop.stat().st_ino)
        rows=self.apply([noop,alias])
        self.assertEqual([r['status'] for r in rows],[b'noop',b'noop'])
        self.assertTrue(noop.exists() and alias.exists())
        self.assertEqual((noop.stat().st_dev,noop.stat().st_ino),identity)
        self.assertEqual((alias.stat().st_dev,alias.stat().st_ino),identity)
        self.assert_pool_bodies([data,data],sources={noop:(data,mode),alias:(data,mode)})


    def test_pool_post_open_lock_alias_is_quarantined_until_join(self):
        paths,bodies=self.pool_messages(8)
        lock=self.root/'cache/filter-engine/batch.lock'
        with self.held('message-before-open,message-lock-alias',paths) as h:
            backup=self.incoming/'tmp/original';paths[0].rename(backup);os.link(lock,paths[0])
            h.resume();h.ready()
            probe=['python3','-c',"import fcntl,sys; f=open(sys.argv[1],'r+'); fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)",str(lock)]
            r=subprocess.run(probe,capture_output=True)
            self.assertNotEqual(r.returncode,0,'post-open alias released the coordinator batch lock')
            h.resume();rc,out,err=h.finish()
        self.assertEqual(rc,3,err);self.assertEqual(backup.read_bytes(),bodies[0])
        self.assertTrue(all(p.exists() for p in paths[1:]));self.assertIn(b'E_BATCH_LOCK_CHANGED',err)
        r=subprocess.run(probe,capture_output=True);self.assertEqual(r.returncode,0,r.stderr)
        sources=self.pool_sources(paths[1:],bodies[1:]);sources[backup]=(bodies[0],self.pool_modes[paths[0]])
        self.assertEqual(paths[0].stat().st_ino,lock.stat().st_ino)
        self.assert_pool_bodies(bodies,[b''],sources=sources,foreign={paths[0]:(b'',0o600)})


    def test_pool_signal_during_preflight_prevents_message_effects(self):
        paths,bodies=self.pool_messages(40)
        with self.held('batch-preflight',paths) as h:
            h.process.send_signal(signal.SIGTERM);h.resume();rc,out,err=h.finish()
        self.assertEqual(rc,143,err);self.assertTrue(all(p.exists() for p in paths))
        self.assert_pool_bodies(bodies,sources=self.pool_sources(paths,bodies))


    def pool_file_output_error(self,count,limit):
        paths,bodies=self.pool_messages(count);output=self.case/'limited-output'
        def bounds():
            signal.signal(signal.SIGXFSZ,signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE,(limit,limit))
        with output.open('wb') as stream:
            r=subprocess.run(self.argv(paths),stdout=stream,stderr=subprocess.PIPE,preexec_fn=bounds,restore_signals=False,timeout=30)
        self.assertEqual(r.returncode,3,r.stderr)
        raw=output.read_bytes();self.assertTrue(raw.startswith(b'JFD1\0'+self.generation+b'\0'))
        values=raw.split(b'\0')[2:-1]
        for i in range(len(values)//11):
            row=dict(zip(fixtures.FIELDS,values[i*11:(i+1)*11]))
            self.assertEqual(row['status'],b'moved')
            self.assertFalse(paths[int(row['index'])-1].exists())
        completed=min(count,32)
        self.assertFalse(any(p.exists() for p in paths[:completed]))
        self.assertTrue(all(p.exists() for p in paths[completed:]))
        self.assertGreaterEqual(r.stderr.count(b'E_RETAINED_PATH'),completed)
        self.assert_pool_bodies(bodies,sources=self.pool_sources(paths[completed:],bodies[completed:]))
        self.capture(self.argv(paths),subprocess.CompletedProcess([],r.returncode,raw,r.stderr),extra=dict(RLIMIT_FSIZE=limit,SIGXFSZ='ignored',output_path=str(output)))

    def test_pool_final_flush_failure_keeps_truthful_durable_outcome_and_nonzero_exit(self):
        self.pool_file_output_error(1,256)

    def test_pool_midrecord_regular_output_failure_never_admits_next_window(self):
        self.pool_file_output_error(65,5000)


    def test_pool_global_resource_error_cancels_queued_and_settles_active_jobs(self):
        paths,bodies=self.pool_messages(65)
        r=subprocess.run(self.argv(paths,True),env=dict(os.environ,JF_TEST_FAIL='store-job-start'),capture_output=True,timeout=30)
        self.capture(self.argv(paths,True),r,extra=dict(JF_TEST_FAIL='store-job-start'))
        self.assertEqual(r.returncode,3,r.stderr);self.assertIn(b'E_MEMORY',r.stderr)
        self.assertTrue(all(p.exists() for p in paths),'phase-gated first resource error must cancel all later jobs')
        self.assertEqual(r.stdout,b'JFD1\0'+self.generation+b'\0');self.assert_pool_bodies(bodies,sources=self.pool_sources(paths,bodies))


    def pool_name_limit(self,response):
        paths,bodies=self.pool_messages(2);self.maildir(self.root/'unsorted')
        existing=self.root/'unsorted/cur'/paths[0].name;existing.write_bytes(b'prior delivery');existing.chmod(0o640)
        source=self.case/'name-limit.c';library=self.case/'name-limit.so'
        source.write_text('#define _POSIX_C_SOURCE 200809L\n#include <unistd.h>\n#include <errno.h>\nlong fpathconf(int fd,int name) { (void)fd; if (name==_PC_NAME_MAX) { '+response+' } errno=EINVAL; return -1; }\n')
        self.command(['gcc','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror','-o',library,source])
        preload=str(library)
        for marker,runtime in [('ASAN_OPTIONS','libasan.so'),('TSAN_OPTIONS','libtsan.so')]:
            if marker in os.environ:
                runtime_path=self.command(['gcc','-print-file-name='+runtime]).stdout.decode().strip()
                preload=runtime_path+':'+preload
        with self.pool_held('store-before-claim',paths,dict(LD_PRELOAD=preload)) as h:
            self.assertLessEqual(len(list(Path('/proc/%s/task'%h.process.pid).iterdir())),2)
            self.assertTrue(paths[1].exists());rows,_=self.pool_rows(h)
        self.assertEqual([r['status'] for r in rows],[b'moved',b'moved'])
        self.assertEqual(existing.read_bytes(),b'prior delivery')
        self.assert_pool_bodies(bodies,[b'prior delivery'],foreign={existing:(b'prior delivery',0o640)},alternatives={bodies[0]})

    def test_pool_short_destination_name_limit_uses_serial_collision_semantics(self):
        self.pool_name_limit('return 14;')

    def test_pool_indeterminate_destination_name_limit_uses_serial_collision_semantics(self):
        self.pool_name_limit('errno=0; return -1;')

    def test_pool_inventory_rejects_duplicate_partial_foreign_and_wrong_identity(self):
        paths,bodies=self.pool_messages(2);self.apply(paths)
        first=self.root/'unsorted/cur'/paths[0].name;second=self.root/'unsorted/cur'/paths[1].name
        self.assert_pool_bodies(bodies)
        duplicate=first.with_name('duplicate:2,S');duplicate.write_bytes(bodies[0]);duplicate.chmod(0o600)
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:duplicate.unlink()
        stage=self.root/'unsorted/tmp/.jaro-filter.foreign';stage.mkdir(mode=0o700);partial=stage/'message';partial.write_bytes(bodies[0][:20]);partial.chmod(0o600)
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:partial.unlink();stage.rmdir()
        foreign=first.with_name('foreign:2,S');foreign.write_bytes(b'foreign message');foreign.chmod(0o600)
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:foreign.unlink()
        first.write_bytes(bodies[1]);second.write_bytes(bodies[0])
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:first.write_bytes(bodies[0]);second.write_bytes(bodies[1])
        first.chmod(0o640)
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:first.chmod(0o600)
        link=self.root/'unsorted/new/foreign-link';link.symlink_to(first)
        try:
            with self.assertRaises(AssertionError):self.assert_pool_bodies(bodies)
        finally:link.unlink()
        self.assert_pool_bodies(bodies)



if __name__=='__main__':
    program = unittest.main(exit=False,verbosity=2)
    print('Storage counts: '+json.dumps(getattr(StorageTests,'counts',{}),sort_keys=True))
    raise SystemExit(not program.result.wasSuccessful())
