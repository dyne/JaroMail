"""Real CMake builds/packages and isolated Git/GitHub release contracts."""
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]
WORK = Path(os.environ.get('JARO_FILTER_BUILD_WORK') or tempfile.mkdtemp(prefix='jaro-cmake-', dir=os.environ.get('TMPDIR')))
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('jaro_build_version', REPO / 'build/version.py')
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)
spec = importlib.util.spec_from_file_location('jaro_abook_deps', REPO / 'build/abook-deps.py')
abook_deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abook_deps)


def run(args, *, env=None, cwd=None, succeeds=True):
    result = subprocess.run(list(map(str, args)), env=env, cwd=cwd, capture_output=True, text=True, timeout=120)
    if succeeds and result.returncode:
        raise AssertionError(f'{args!r} failed ({result.returncode}):\n{result.stdout[-1800:]}\n{result.stderr[-1800:]}')
    return result


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.root = WORK / self._testMethodName
        self.root.mkdir()
        (self.root / 'VERSION').write_text('6.0.0\n')
        shutil.copy2(REPO / '.gitattributes', self.root)
        shutil.copy2(REPO / '.git_archival.txt', self.root)
        run(['git', 'init', '-q', self.root])
        run(['git', '-C', self.root, 'add', '.'])
        run(['git', '-C', self.root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.org',
             'commit', '-qm', 'fixture'])

    def tag(self, name):
        run(['git', '-C', self.root, 'tag', name])

    def test_version_baseline_and_latest_reachable_tag(self):
        self.tag('v5.2.1')
        self.assertEqual(version.resolve(self.root), '6.0.0')
        self.tag('v6.1.2')
        self.assertEqual(version.resolve(self.root), '6.1.2')
        self.tag('v6.2.0')
        self.tag('v6.02.1')
        self.tag('v7.0.0-rc.1')
        self.assertEqual(version.resolve(self.root), '6.2.0')
        run(['git', '-C', self.root, 'checkout', '-qb', 'unreleased'])
        run(['git', '-C', self.root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.org',
             'commit', '--allow-empty', '-qm', 'unrelated'])
        self.tag('v99.0.0')
        run(['git', '-C', self.root, 'checkout', '-q', 'HEAD~1'])
        self.assertEqual(version.resolve(self.root), '6.2.0')

    def test_version_override_and_invalid_input(self):
        self.assertEqual(version.resolve(self.root, '6.3.4'), '6.3.4')
        self.assertEqual(version.resolve(self.root, '6.3.4-rc.1+build.9'), '6.3.4-rc.1+build.9')
        for invalid in ('6.0', 'v6.0.0', '06.0.0', '6.0.0-01', '6.0.0\necho unsafe', '../6.0.0'):
            with self.assertRaises(ValueError):
                version.resolve(self.root, invalid)

    def test_version_archive_metadata_and_copied_source(self):
        self.tag('v6.4.3')
        archive = self.root.parent / (self.root.name+'.tar')
        run(['git', '-C', self.root, 'archive', '--format=tar', '--output', archive, 'HEAD'])
        extracted = self.root / 'extracted'
        extracted.mkdir()
        with tarfile.open(archive) as bundle:
            bundle.extractall(extracted, filter='data')
        self.assertIn('v6.4.3', (extracted / '.git_archival.txt').read_text())
        self.assertEqual(version.resolve(extracted), '6.4.3')
        copied = self.root / 'copied'
        copied.mkdir()
        (copied / 'VERSION').write_text('6.0.0\n')
        self.assertEqual(version.resolve(copied), '6.0.0')

    def test_version_release_bootstrap_major(self):
        self.tag('v5.2.1')
        output = self.root / 'output'
        run([sys.executable, REPO / 'build/release.py', 'prepare', '--source', self.root, '--output', output])
        self.assertIn('tag=v5.2.1\nadditional=breaking:', output.read_text())
        output.unlink()
        self.tag('v6.0.0')
        run([sys.executable, REPO / 'build/release.py', 'prepare', '--source', self.root, '--output', output])
        self.assertEqual(output.read_text(), 'tag=v6.0.0\nadditional=\n')

    def test_version_release_major_validation(self):
        for current, next_version, success in [('v5.2.1','6.0.0',True),('v5.2.1','5.3.0',False),
                                               ('v5.2.1','7.0.0',False),('v6.1.0','6.1.0',True),
                                               ('v6.1.0','6.0.1',False),('v6.1.0','6.2.0',True)]:
            result = run([sys.executable,REPO/'build/release.py','validate-version',
                          '--current',current,'--next',next_version],succeeds=success)
            self.assertEqual(result.returncode==0,success)


class CMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = WORK / 'cmake'
        cls.work.mkdir()
        cls.arch = 'arm64' if platform.machine() in ('aarch64', 'arm64') else 'x86_64'
        cls.source, cls.binary = cls.work / 'source with spaces', cls.work / 'binary with spaces'
        cls.source.mkdir()
        for name in ('src', 'build', 'doc', 'cmake', 'vendor', 'CMakeLists.txt', 'Makefile', 'VERSION', 'COPYING.txt'):
            original = REPO / name
            if original.is_dir():
                def ignore(directory, names):
                    return [n for n in names if n.endswith(('.o','.d','.zwc')) or n == '__pycache__' or
                            (Path(directory) == REPO/'build' and n in ('gnu','win','sieve-tools'))]
                shutil.copytree(original, cls.source/name, ignore=ignore)
            else:
                shutil.copy2(original, cls.source/name)
        cls.bin = cls.work / 'tools'
        cls.bin.mkdir()
        pandoc = cls.bin / 'pandoc'
        pandoc.write_text('#!/bin/sh\necho unexpected-Pandoc >&2\nexit 91\n')
        pandoc.chmod(0o700)
        compiler = cls.bin / 'compiler'
        compiler.write_text('''#!/bin/sh
case "${JARO_CMAKE_FAIL:-}" in
  compile) exit 77 ;;
  link) case " $* " in *' -c '*) ;; *) exit 78 ;; esac ;;
esac
exec "$JARO_REAL_CC" "$@"
''')
        compiler.chmod(0o700)
        cls.env = dict(os.environ, PATH=str(cls.bin)+os.pathsep+os.environ['PATH'], JARO_REAL_CC=shutil.which('cc'))
        for name in ('PREFIX', 'DESTDIR', 'CMAKE_INSTALL_PREFIX'):
            cls.env.pop(name, None)
        cls.manuals = {name: (cls.source/'doc'/name).read_bytes() for name in ('jaro.1','jaromail.1')}
        configured = run(['cmake', '-S', cls.source, '-B', cls.binary, '-G', 'Ninja',
                         '-DCMAKE_C_COMPILER='+str(compiler), '-DCMAKE_BUILD_TYPE=',
                         '-DCMAKE_C_FLAGS=-O0 -DJARO_CMAKE_REGRESSION_TEST=1',
                         '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--as-needed', '-DCMAKE_C_STANDARD_LIBRARIES=-lm',
                         '-DJARO_VERSION=6.7.8', '-DJARO_PACKAGE_ARCH='+cls.arch, '-DBUILD_TESTING=OFF'], env=cls.env)
        (cls.work/'configure.log').write_text(configured.stdout+configured.stderr)
        built = run(['cmake', '--build', cls.binary, '--parallel', '2', '--verbose'], env=cls.env)
        cls.buildlog = built.stdout+built.stderr
        (cls.work/'build.log').write_text(cls.buildlog)

    def test_cmake_flags_cache_and_native_helpers(self):
        commands = json.loads((self.binary/'compile_commands.json').read_text())
        self.assertTrue(commands)
        self.assertTrue(all('-DJARO_CMAKE_REGRESSION_TEST=1' in row['command'] for row in commands))
        self.assertIn('-Wl,--as-needed', self.buildlog)
        self.assertIn('-lm', self.buildlog)
        if shutil.which('ccache') and 'CCACHE' not in os.environ:
            self.assertIn('ccache', self.buildlog)
        for name in ('fetchaddr','parsedate','dotlock','gpgewrap','jaro-filter','jaro-filter-compile','jaro-filter-legacy','abook'):
            self.assertTrue(os.access(self.binary/'bin'/name, os.X_OK))
        result = run([sys.executable, self.source/'build/check-static.py', '--bin-dir', self.binary/'bin', '--arch',self.arch], succeeds=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('dynamic', result.stderr)

    def test_cmake_install_destdir_and_relocated_version(self):
        stage = self.work / 'stage'
        prefix = '/opt/jaro release'
        run(['cmake','--install',self.binary,'--prefix',prefix],env=dict(self.env,DESTDIR=str(stage)))
        installed = stage / prefix.lstrip('/')
        self.assertEqual(run([installed/'bin/jaro','--version'],env=self.env).stdout, 'Jaro Mail v6.7.8\n')
        moved = self.work / 'relocated install'
        shutil.copytree(installed,moved)
        self.assertEqual(run([moved/'bin/jaro','-v'],env=self.env,cwd=self.work).stdout,'Jaro Mail v6.7.8\n')
        runtime = moved/'share/jaromail'
        self.assertEqual((runtime/'VERSION').read_text(),'6.7.8\n')
        module = runtime/'zlibs/filters.zwc'
        # Installation removes stale precompiled Zsh modules from the real prefix.
        stale = installed/'share/jaromail/zlibs/filters.zwc'
        stale.write_bytes(b'stale')
        run(['cmake','--install',self.binary,'--prefix',prefix],env=dict(self.env,DESTDIR=str(stage)))
        self.assertFalse(stale.exists())
        for name,data in self.manuals.items():
            self.assertEqual((installed/'share/man/man1'/name).read_bytes(),data)

    def test_cmake_abook_import_export_and_private_install(self):
        abook = self.binary/'bin/abook'
        self.assertIn('abook v0.6.2',run([abook,'--help'],env=self.env).stdout)
        work = self.work/'abook roundtrip'
        work.mkdir()
        address = 'person.with.a.long.untruncated.address@addressbook.example.org'
        aliases, database = work/'aliases', work/'contacts.abook'
        aliases.write_text(f'alias fixture Full Address <{address}>\n')
        run([abook,'--config','/dev/null','--convert','--informat','mutt','--infile',aliases,
             '--outformat','abook','--outfile',database],env=self.env)
        self.assertIn('email='+address,database.read_text())
        query = run([abook,'--config','/dev/null','--datafile',database,'--mutt-query',''],env=self.env).stdout
        self.assertIn(address+'\tFull Address\t',query)
        exported = run([abook,'--config','/dev/null','--convert','--informat','abook','--infile',database,
                        '--outformat','vcard'],env=self.env).stdout
        self.assertIn(address,exported)
        self.assertIn('BEGIN:VCARD',exported)
        original = {p.relative_to(self.source/'vendor/abook/upstream'):p.read_bytes()
                    for p in (self.source/'vendor/abook/upstream').rglob('*') if p.is_file()}
        installed = work/'installed'
        run(['cmake','--install',self.binary,'--prefix',installed],env=self.env)
        runtime = installed/'share/jaromail'
        self.assertTrue((runtime/'licenses/abook/COPYING').is_file())
        self.assertTrue((installed/'share/man/man5/abookrc.5').is_file())
        self.assertFalse((installed/'bin/abook').exists())
        # Match the bootstrap's private-bin precedence even with a system decoy.
        decoy = work/'decoy'
        decoy.mkdir()
        (decoy/'abook').write_text('#!/bin/sh\nexit 91\n')
        (decoy/'abook').chmod(0o700)
        probe = work/'runtime probe.zsh'
        probe.write_text('source "$JAROWORKDIR/zlibs/bootstrap"\nwhence -p abook\nabook --help\n')
        env = dict(self.env,JAROWORKDIR=str(runtime),PATH=str(decoy)+os.pathsep+self.env['PATH'])
        result = run(['zsh',probe],env=env,cwd=work)
        self.assertIn(str(runtime/'bin/abook'),result.stdout)
        self.assertIn('abook v0.6.2',result.stdout)
        run([sys.executable,REPO/'extras/test/abook-tui-smoke.py','--binary',runtime/'bin/abook'],env=self.env)
        self.assertEqual(original,{p.relative_to(self.source/'vendor/abook/upstream'):p.read_bytes()
                                  for p in (self.source/'vendor/abook/upstream').rglob('*') if p.is_file()})

    def test_cmake_default_user_and_system_install_with_destdir(self):
        user_prefix = Path.home()/'.local'
        cache = (self.binary/'CMakeCache.txt').read_text()
        self.assertIn(f'CMAKE_INSTALL_PREFIX:PATH={user_prefix}\n', cache)
        stage = self.work/'user stage'
        run(['cmake','--install',self.binary],env=dict(self.env,DESTDIR=str(stage)))
        installed = stage/str(user_prefix).lstrip('/')
        self.assertEqual(run([installed/'bin/jaro','--version'],env=self.env).stdout,'Jaro Mail v6.7.8\n')
        for prefix in ('/usr/local', '/usr', '/opt/system with spaces'):
            stage = self.work/('system stage '+prefix.rsplit('/',1)[-1])
            env = dict(self.env,DESTDIR=str(stage))
            if prefix != '/usr/local':
                env['PREFIX'] = prefix
            run(['cmake','--build',self.binary,'--target','install-system'],env=env)
            installed = stage/prefix.lstrip('/')
            self.assertEqual(run([installed/'bin/jaro','--version'],env=self.env).stdout,'Jaro Mail v6.7.8\n')
            self.assertTrue((installed/'share/man/man1/jaro.1').is_file())
        self.assertEqual((self.binary/'CMakeCache.txt').read_text(),cache)
        blocked = self.work/'invalid staging directory'
        blocked.write_text('not a directory\n')
        result = run(['cmake','--build',self.binary,'--target','install-system'],
                     env=dict(self.env,DESTDIR=str(blocked)),succeeds=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('System installation failed',result.stdout+result.stderr)

    def test_cmake_explicit_prefix_is_preserved(self):
        for name, extra, env in (
            ('cache', ['-DCMAKE_INSTALL_PREFIX=/opt/explicit prefix'], dict(self.env,PREFIX='/opt/ignored')),
            ('environment', [], dict(self.env,PREFIX='/opt/explicit prefix')),
        ):
            binary = self.work/('prefix '+name)
            run(['cmake','-S',self.source,'-B',binary,'-G','Ninja','-DBUILD_TESTING=OFF',*extra],env=env)
            self.assertIn('CMAKE_INSTALL_PREFIX:PATH=/opt/explicit prefix\n',(binary/'CMakeCache.txt').read_text())
            run(['cmake','-S',self.source,'-B',binary],env=self.env)
            self.assertIn('CMAKE_INSTALL_PREFIX:PATH=/opt/explicit prefix\n',(binary/'CMakeCache.txt').read_text())

    def test_make_user_system_and_prefix_overrides_with_destdir(self):
        binary = self.work/'make output'
        cases = (
            ('install', Path.home()/'.local', 'default'),
            ('install-system', Path('/usr/local'), 'default'),
            ('install-system', Path('/usr'), 'command'),
            ('install-system', Path('/opt/env with spaces'), 'environment'),
            ('install', Path('/opt/user override'), 'command'),
        )
        expected_version = 'Jaro Mail v'+version.resolve(self.source)+'\n'
        for index, (target,prefix,override) in enumerate(cases):
            stage = self.work/f'make stage {index}'
            args = ['make','-C',self.source,target,'BUILD_DIR='+str(binary),'DESTDIR='+str(stage)]
            env = dict(self.env)
            if override == 'command':
                args.append('PREFIX='+str(prefix))
            elif override == 'environment':
                env['PREFIX'] = str(prefix)
            run(args,env=env)
            installed = stage/str(prefix).lstrip('/')
            self.assertEqual(run([installed/'bin/jaro','--version'],env=self.env).stdout,expected_version)
            self.assertTrue((installed/'share/man/man1/jaro.1').is_file())
        stage = self.work/'direct helper stage'
        run([sys.executable,self.source/'build/cmake-build.py','--output-dir',binary,'--install'],
            env=dict(self.env,DESTDIR=str(stage)))
        installed = stage/str(Path.home()/'.local').lstrip('/')
        self.assertEqual(run([installed/'bin/jaro','--version'],env=self.env).stdout,expected_version)

    def test_cmake_incremental_build_refreshes_tag_version(self):
        run(['git','init','-q',self.source])
        run(['git','-C',self.source,'add','.'])
        run(['git','-C',self.source,'-c','user.name=Fixture','-c','user.email=fixture@example.org',
             'commit','-qm','fixture'])
        binary=self.work/'tag build'
        run(['cmake','-S',self.source,'-B',binary,'-G','Ninja','-DBUILD_TESTING=OFF'],env=self.env)
        self.assertEqual((binary/'generated/VERSION').read_text(),'6.0.0\n')
        run(['git','-C',self.source,'tag','v6.9.1'])
        run(['cmake','--build',binary,'--target','jaro-filter-compile','--parallel','2'],env=self.env)
        self.assertEqual((binary/'generated/VERSION').read_text(),'6.9.1\n')
        self.assertIn('jaromail-6.9.1-linux-',(binary/'CPackConfig.cmake').read_text())
        branch=run(['git','-C',self.source,'symbolic-ref','--short','HEAD']).stdout.strip()
        run(['git','-C',self.source,'checkout','-qb','future-version'])
        run(['git','-C',self.source,'-c','user.name=Fixture','-c','user.email=fixture@example.org',
             'commit','--allow-empty','-qm','future'])
        run(['git','-C',self.source,'tag','v6.9.2'])
        run(['git','-C',self.source,'checkout','-q',branch])
        run(['cmake','--build',binary,'--target','jaro-filter-compile'],env=self.env)
        self.assertEqual((binary/'generated/VERSION').read_text(),'6.9.1\n')
        run(['git','-C',self.source,'merge','--ff-only','future-version'])
        run(['cmake','--build',binary,'--target','jaro-filter-compile'],env=self.env)
        self.assertEqual((binary/'generated/VERSION').read_text(),'6.9.2\n')

    def test_cmake_manpages_are_never_implicit(self):
        run(['cmake','--build',self.binary,'--parallel','2'],env=self.env)
        for name,data in self.manuals.items():self.assertEqual((self.source/'doc'/name).read_bytes(),data)
        result = run(['cmake','--build',self.binary,'--target','man'],env=self.env,succeeds=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('unexpected-Pandoc',result.stdout+result.stderr)
        for name,data in self.manuals.items():self.assertEqual((self.source/'doc'/name).read_bytes(),data)

    def test_cmake_in_source_build_cannot_overwrite_make_wrapper(self):
        source=self.work/'forbidden in-source build'
        source.mkdir()
        shutil.copy2(REPO/'CMakeLists.txt',source)
        makefile=source/'Makefile'
        makefile.write_text('preserve this wrapper\n')
        result=run(['cmake','-S',source,'-B',source],succeeds=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('out-of-source',result.stderr)
        self.assertEqual(makefile.read_text(),'preserve this wrapper\n')

    def test_cmake_compile_and_link_failures_propagate(self):
        for mode in ('compile','link'):
            try:
                result = run(['cmake','--build',self.binary,'--clean-first','--target','fetchaddr'],
                             env=dict(self.env,JARO_CMAKE_FAIL=mode,CCACHE_DISABLE='1'),succeeds=False)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse((self.binary/'bin/fetchaddr').exists())
            finally:
                run(['cmake','--build',self.binary,'--parallel','2'],env=self.env)

    def test_cmake_package_is_complete_and_relocatable(self):
        run(['cmake','--build',self.binary,'--target','package'],env=self.env)
        archive=self.binary/('jaromail-6.7.8-linux-'+self.arch+'.tar.gz')
        self.assertTrue(archive.is_file())
        extracted=self.work/'archive'
        extracted.mkdir()
        with tarfile.open(archive) as bundle:bundle.extractall(extracted,filter='data')
        root=extracted/('jaromail-6.7.8-linux-'+self.arch)
        self.assertEqual(run([root/'bin/jaro','--version'],env=self.env).stdout,'Jaro Mail v6.7.8\n')
        self.assertTrue((root/'share/jaromail/bin/jaro-filter').is_file())
        self.assertIn('abook v0.6.2',run([root/'share/jaromail/bin/abook','--help'],env=self.env).stdout)
        self.assertTrue((root/'share/jaromail/COPYING.txt').is_file())
        self.assertFalse((root/'share/jaromail/bin/filter-policy-driver').exists())


class AbookDependencyTests(unittest.TestCase):
    def setUp(self):
        self.root = WORK/self._testMethodName
        self.root.mkdir()

    def test_abook_dependency_archive_checksum_and_offline_failures(self):
        source = dict(name='fixture',version='1',sha256=hashlib.sha256(b'pinned bytes').hexdigest())
        with self.assertRaisesRegex(ValueError,'missing'):
            abook_deps.archive(source,self.root,False)
        path = self.root/'fixture-1.tar.gz'
        path.write_bytes(b'pinned bytes')
        self.assertEqual(abook_deps.archive(source,self.root,False),path)
        path.write_bytes(b'changed archive')
        with self.assertRaisesRegex(ValueError,'checksum differs'):
            abook_deps.archive(source,self.root,False)

    def test_abook_reuses_ncurses_from_dyne_target_prefix(self):
        compiler = self.root/'dyne/gcc-musl/bin/x86_64-linux-musl-gcc'
        compiler.parent.mkdir(parents=True)
        self.assertIsNone(abook_deps.bundled_ncurses(compiler,'x86_64-linux-musl'))
        target = self.root/'dyne/x86_64-linux-musl'
        include = target/'include/ncursesw'
        include.mkdir(parents=True)
        notice = '/* Permission is hereby granted, free of charge. */\n'
        (include/'curses.h').write_text(notice+'#define NCURSES_VERSION "6.6"\n')
        (include/'ncurses.h').write_text(notice+'#define NCURSES_VERSION "6.6"\n')
        (include/'ncurses_dll.h').write_text('/* fixture declarations */\n')
        (target/'lib').mkdir()
        (target/'lib/libncursesw.a').write_bytes(b'fixture static library')
        bundle = abook_deps.bundled_ncurses(compiler,'x86_64-linux-musl')
        self.assertEqual(bundle['identity']['version'],'6.6')
        installed = self.root/'staged'
        abook_deps.stage_ncurses(bundle,installed)
        self.assertEqual((installed/'include/ncurses.h').read_bytes(),(include/'ncurses.h').read_bytes())
        self.assertTrue((installed/'include/ncursesw/ncurses_dll.h').is_file())
        self.assertEqual((installed/'lib/libncursesw.a').read_bytes(),b'fixture static library')
        self.assertIn('Permission is hereby granted',(installed/'share/licenses/ncurses/COPYING').read_text())
        first = bundle['identity']
        (target/'lib/libncursesw.a').write_bytes(b'new bundled library')
        self.assertNotEqual(first,abook_deps.bundled_ncurses(compiler,'x86_64-linux-musl')['identity'])

    def test_abook_partial_bundle_is_rejected(self):
        compiler = self.root/'dyne/gcc-musl/bin/x86_64-linux-musl-gcc'
        (self.root/'dyne/x86_64-linux-musl/lib').mkdir(parents=True)
        (self.root/'dyne/x86_64-linux-musl/lib/libncursesw.a').write_bytes(b'incomplete')
        with self.assertRaisesRegex(ValueError,'incomplete bundled ncurses'):
            abook_deps.bundled_ncurses(compiler,'x86_64-linux-musl')

    def test_abook_reuses_bundled_readline_and_history(self):
        compiler = self.root/'dyne/gcc-musl/bin/x86_64-linux-musl-gcc'
        self.assertIsNone(abook_deps.bundled_readline(compiler,'x86_64-linux-musl'))
        target = self.root/'dyne/x86_64-linux-musl'
        include = target/'include/readline'
        include.mkdir(parents=True)
        (include/'readline.h').write_text('#define RL_VERSION_MAJOR\t8\n#define RL_VERSION_MINOR\t3\n')
        (include/'history.h').write_text('/* history fixture */\n')
        (target/'lib').mkdir()
        (target/'lib/libreadline.a').write_bytes(b'static readline fixture')
        history = target/'lib/libhistory.a'
        history.write_bytes(b'static history fixture')
        bundle = abook_deps.bundled_readline(compiler,'x86_64-linux-musl')
        self.assertEqual(bundle['identity']['version'],'8.3')
        installed = self.root/'staged'
        abook_deps.stage_readline(bundle,installed)
        for name in ('libreadline.a','libhistory.a'):
            self.assertEqual((installed/'lib'/name).read_bytes(),(target/'lib'/name).read_bytes())
        self.assertEqual((installed/'include/readline/history.h').read_bytes(),(include/'history.h').read_bytes())
        original = bundle['identity']
        history.write_bytes(b'updated history library')
        self.assertNotEqual(original,abook_deps.bundled_readline(compiler,'x86_64-linux-musl')['identity'])
        history.unlink()
        with self.assertRaisesRegex(ValueError,'incomplete bundled readline'):
            abook_deps.bundled_readline(compiler,'x86_64-linux-musl')

    def test_abook_required_bundle_fails_before_building_or_creating_cache(self):
        compiler = self.root/'dyne/gcc-musl/bin/x86_64-linux-musl-gcc'
        compiler.parent.mkdir(parents=True)
        compiler.write_text('#!/bin/sh\ncase "$1" in\n-dumpmachine) echo x86_64-linux-musl ;;\n--version) echo fixture-compiler ;;\n*) exit 91 ;;\nesac\n')
        compiler.chmod(0o700)
        cache, sources = self.root/'cache', self.root/'sources'
        result = run([sys.executable,REPO/'build/abook-deps.py','--cc',compiler,'--arch','x86_64',
                      '--cache-dir',cache,'--source-cache',sources,'--require-bundled'],succeeds=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('--require-bundled needs complete target',result.stderr)
        self.assertFalse(cache.exists())
        self.assertFalse(sources.exists())

    def test_abook_dependency_extract_rejects_traversal_and_links(self):
        for index,name in enumerate(('../escape','fixture-1/../../escape','fixture-1/link')):
            path = self.root/f'archive{index}.tar.gz'
            with tarfile.open(path,'w:gz') as bundle:
                member = tarfile.TarInfo(name)
                if index == 2:
                    member.type,member.linkname = tarfile.SYMTYPE,'/outside'
                    bundle.addfile(member)
                else:
                    member.size = 1
                    bundle.addfile(member,io.BytesIO(b'x'))
            with self.assertRaises(ValueError):
                abook_deps.extract(path,self.root,dict(name='fixture',version='1'))

    def test_abook_dependency_cache_rejects_modified_files_and_identity(self):
        prefix = self.root/'prefix'
        prefix.mkdir()
        library = prefix/'target.a'
        library.write_bytes(b'archive bytes')
        identity = dict(machine='fixture-musl')
        (self.root/'build.json').write_text(json.dumps(dict(identity=identity,files={'target.a':abook_deps.digest(library)})))
        self.assertEqual(abook_deps.verify_cache(self.root,identity),prefix)
        with self.assertRaisesRegex(ValueError,'identity differs'):
            abook_deps.verify_cache(self.root,dict(machine='wrong-arch'))
        library.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'contents differ'):
            abook_deps.verify_cache(self.root,identity)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.work=WORK/self._testMethodName
        self.work.mkdir()
        self.log=self.work/'gh.jsonl'
        self.state=self.work/'state.json'
        self.assets=self.work/'assets'
        self.assets.mkdir()
        for arch in ('x86_64','arm64'):
            path=self.assets/f'jaromail-6.0.0-linux-{arch}.tar.gz'
            path.write_bytes(('fixture-'+arch).encode())
            path.with_name(path.name+'.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name+'\n')
        fake=self.work/'gh'
        fake.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
args=sys.argv[1:];state=pathlib.Path(os.environ['RELEASE_FAKE_STATE'])
with open(os.environ['RELEASE_FAKE_LOG'],'a')as f:f.write(json.dumps(args)+'\\n')
current=json.loads(state.read_text())if state.exists()else None
if args[:2]==['release','view']:
    if current is None:sys.exit(1)
    print(json.dumps(current))
elif args[0]=='api':
    if current is None:sys.exit(1)
    print(os.environ.get('RELEASE_FAKE_COMMIT','a'*40))
elif args[:2]==['release','create']:
    state.write_text(json.dumps({'isDraft':True,'assets':[{'name':pathlib.Path(v).name}for v in args if '.tar.gz'in v]}))
elif args[:2]==['release','edit']:
    current['isDraft']=False;state.write_text(json.dumps(current))
''')
        fake.chmod(0o700)
        self.env=dict(os.environ,PATH=str(self.work)+os.pathsep+os.environ['PATH'],
                      RELEASE_FAKE_STATE=str(self.state),RELEASE_FAKE_LOG=str(self.log))

    def publish(self,succeeds=True,extra=None):
        return run([sys.executable,REPO/'build/release.py','publish','--version','6.0.0',
                    '--artifacts',self.assets,'--repository','fixture/repo','--commit','a'*40],
                   env=dict(self.env,**(extra or {})),succeeds=succeeds)

    def test_release_both_architectures_before_publish_and_retry(self):
        self.publish()
        self.assertFalse(json.loads(self.state.read_text())['isDraft'])
        calls=[json.loads(line)for line in self.log.read_text().splitlines()]
        create=next(c for c in calls if c[:2]==['release','create'])
        self.assertEqual(sum('.tar.gz'in arg for arg in create),4)
        self.assertIn('--draft',create)
        self.assertEqual(calls[-1][:2],['release','edit'])
        self.log.unlink()
        self.publish()
        calls=[json.loads(line)for line in self.log.read_text().splitlines()]
        self.assertFalse(any(c[:2]in (['release','create'],['release','edit'],['release','upload'])for c in calls))

    def test_release_missing_or_corrupt_artifact_never_contacts_github(self):
        arm=self.assets/'jaromail-6.0.0-linux-arm64.tar.gz'
        arm.unlink()
        self.assertNotEqual(self.publish(False).returncode,0)
        self.assertFalse(self.log.exists())
        arm.write_bytes(b'corrupted')
        self.assertNotEqual(self.publish(False).returncode,0)
        self.assertFalse(self.log.exists())

    def test_release_refuses_conflicting_tag_or_incomplete_published_release(self):
        self.state.write_text(json.dumps({'isDraft':True,'assets':[]}))
        self.assertNotEqual(self.publish(False,{'RELEASE_FAKE_COMMIT':'b'*40}).returncode,0)
        self.state.write_text(json.dumps({'isDraft':False,'assets':[]}))
        self.assertNotEqual(self.publish(False).returncode,0)
        calls=[json.loads(line)for line in self.log.read_text().splitlines()]
        self.assertFalse(any(c[:2]in (['release','create'],['release','edit'],['release','upload'])for c in calls))


if __name__=='__main__':
    result=unittest.main(verbosity=2,exit=False).result
    summary=dict(executed=result.testsRun,failed=len(result.errors)+len(result.failures),skipped=len(result.skipped),
                 observedAt=datetime.now(timezone.utc).isoformat())
    (WORK/'summary.json').write_text(json.dumps(summary,sort_keys=True)+'\n')
    print('CMake/release tests: '+json.dumps(summary,sort_keys=True))
    sys.exit(0 if result.wasSuccessful()and result.testsRun and not result.skipped else 1)
