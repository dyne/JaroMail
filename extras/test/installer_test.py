"""Exercise the release installer without network access or a real user prefix."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='jaro-installer-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.tools = self.root / 'tools'
        self.tools.mkdir()
        self.prefix = self.root / 'prefix with spaces'
        self.env = dict(os.environ, PATH=str(self.tools)+os.pathsep+os.environ['PATH'],
                        INSTALL_FIXTURE=str(self.root), JARO_INSTALL_PREFIX=str(self.prefix), TMPDIR=str(self.root))
        self.stub('uname', '#!/bin/bash\nif [[ $1 == -s ]]; then echo Linux; else echo "${INSTALL_ARCH:-x86_64}"; fi\n')
        self.stub('curl', '''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args=sys.argv[1:]; root=pathlib.Path(os.environ['INSTALL_FIXTURE'])
if os.environ.get('INSTALL_DOWNLOAD_FAIL'): sys.exit(22)
url=args[-1]
with (root/'downloads').open('a') as calls:
    calls.write(url+'\\n')
if url.endswith('/latest'):
    print('https://github.com/dyne/JaroMail/releases/tag/'+os.environ.get('INSTALL_TAG','v6.1.0'),end='')
else:
    if '/download/v6.1.0/jaromail-6.1.0-linux-' not in url: sys.exit(23)
    source=root/('checksum' if url.endswith('.sha256') else 'package.tar.gz')
    shutil.copyfile(source,args[args.index('--output')+1])
''')
        self.archive()

    def stub(self, name, text):
        path = self.tools / name
        path.write_text(text)
        path.chmod(0o755)

    def archive(self, arch='x86_64'):
        archive = self.root / 'package.tar.gz'
        with tarfile.open(archive, 'w:gz') as bundle:
            for name, text, mode in [('bin/jaro', '#!/bin/bash\necho 6.1.0\n', 0o755),
                                     ('share/jaromail/VERSION', '6.1.0\n', 0o644),
                                     ('share/jaromail/Accounts/default.txt', '# account\n', 0o644),
                                     ('share/jaromail/bin/abook', 'bundled\n', 0o755),
                                     ('share/jaromail/zlibs/setup', '# new shell module\n', 0o644),
                                     ('share/jaromail/sieve/lib/libfixture.so', 'new runtime library\n', 0o644),
                                     ('share/man/man1/jaro.1', 'manual\n', 0o644)]:
                entry = tarfile.TarInfo('jaromail-6.1.0-linux-'+arch+'/'+name)
                entry.size = len(text.encode()); entry.mode = mode
                bundle.addfile(entry, io.BytesIO(text.encode()))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.root/'checksum').write_text(digest+'  package.tar.gz\n')

    def run_installer(self, *args, succeeds=True):
        result = subprocess.run(['bash', str(REPO/'build/install-release.sh'), *args],
                                env=self.env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout+result.stderr)
        return result

    def test_complete_package_and_repeat_install(self):
        for _ in range(2):
            self.run_installer()
            self.assertTrue((self.prefix/'bin/jaro').stat().st_mode & 0o111)
            self.assertEqual((self.prefix/'share/jaromail/VERSION').read_text(), '6.1.0\n')
            self.assertTrue((self.prefix/'share/jaromail/bin/abook').exists())
            self.assertTrue((self.prefix/'share/man/man1/jaro.1').exists())
        self.assertEqual(list(self.root.glob('jaromail-install.*')), [])

    def test_arm64(self):
        self.env['INSTALL_ARCH'] = 'aarch64'
        self.archive('arm64')
        self.run_installer()
        self.assertTrue((self.prefix/'bin/jaro').exists())

    def test_checksum_failure_preserves_install(self):
        self.prefix.mkdir()
        marker = self.prefix/'existing'
        marker.write_text('keep')
        (self.root/'checksum').write_text('0'*64+'  package.tar.gz\n')
        self.assertIn('Checksum mismatch', self.run_installer(succeeds=False).stderr)
        self.assertEqual(list(self.prefix.iterdir()), [marker])

    def test_download_failure(self):
        self.env['INSTALL_DOWNLOAD_FAIL'] = '1'
        self.run_installer(succeeds=False)
        self.assertFalse(self.prefix.exists())

    def test_unsupported_architecture(self):
        self.env['INSTALL_ARCH'] = 'riscv64'
        self.assertIn('Unsupported architecture', self.run_installer(succeeds=False).stderr)
        self.assertFalse(self.prefix.exists())

    def test_invalid_release(self):
        self.env['INSTALL_TAG'] = 'unexpected'
        self.run_installer(succeeds=False)
        self.assertFalse(self.prefix.exists())

    def test_options(self):
        self.run_installer('--help')
        for args in [('--prefix',), ('--prefix','/'), ('--prefix','relative'), ('--unknown',)]:
            self.run_installer(*args, succeeds=False)
        custom = self.root/'custom'
        self.run_installer('--prefix', str(custom))
        self.assertTrue((custom/'bin/jaro').exists())

    def existing_installation(self, version='6.0.0'):
        runtime = self.prefix / 'share/jaromail'
        (runtime / 'bin').mkdir(parents=True)
        (runtime / 'zlibs').mkdir()
        (self.prefix / 'bin').mkdir()
        jaro = self.prefix / 'bin/jaro'
        jaro.write_text('#!/usr/bin/env zsh\nexport JAROWORKDIR="${0:A:h:h}/share/jaromail"\n'
                        'exec "$JAROWORKDIR/bin/jaro" "$@"\n')
        jaro.chmod(0o755)
        shutil.copy2(REPO / 'src/jaro', runtime / 'bin/jaro')
        shutil.copy2(REPO / 'src/zlibs/bootstrap', runtime / 'zlibs/bootstrap')
        shutil.copy2(REPO / 'src/zlibs/version', runtime / 'zlibs/version')
        shutil.copy2(REPO / 'build/install-release.sh', runtime / 'bin/install-release.sh')
        (runtime / 'VERSION').write_text(version + '\n')
        return jaro, runtime

    def run_cli(self, jaro, *args):
        env = dict(self.env, JARO_INSTALL_PREFIX=str(self.root / 'ignored prefix'),
                   JAROMAILDIR=str(self.root / 'untouched mail'))
        return subprocess.run([jaro, *args], env=env, capture_output=True, text=True, timeout=15)

    def test_installed_jaro_update_and_preview(self):
        jaro, runtime = self.existing_installation()
        (runtime / 'obsolete').write_text('old runtime')
        unrelated = self.prefix / 'bin/other'
        unrelated.write_text('unrelated executable')
        for args, status in [(('-n', 'update'), 0), (('update', '/legacy/profile'), 2)]:
            result = self.run_cli(jaro, *args)
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertFalse((self.root / 'downloads').exists())
        result = self.run_cli(jaro, 'update')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((runtime / 'VERSION').read_text(), '6.1.0\n')
        self.assertEqual((runtime / 'zlibs/setup').read_text(), '# new shell module\n')
        self.assertEqual((runtime / 'sieve/lib/libfixture.so').read_text(), 'new runtime library\n')
        self.assertEqual(unrelated.read_text(), 'unrelated executable')
        self.assertFalse((runtime / 'obsolete').exists())
        self.assertFalse((self.root / 'ignored prefix').exists())
        self.assertFalse((self.root / 'untouched mail').exists())

    def test_installed_jaro_skips_equal_or_older_releases(self):
        jaro, runtime = self.existing_installation()
        for version in ('6.1.0', '6.1.0+local', '6.10.0'):
            with self.subTest(version=version):
                (self.root / 'downloads').write_text('')
                (runtime / 'VERSION').write_text(version + '\n')
                result = self.run_cli(jaro, 'update')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('up to date', result.stdout)
                calls = (self.root / 'downloads').read_text().splitlines()
                self.assertEqual(calls, ['https://github.com/dyne/JaroMail/releases/latest'])
                self.assertEqual((runtime / 'VERSION').read_text(), version + '\n')

    def test_failed_activation_restores_previous_runtime_and_launcher(self):
        jaro, runtime = self.existing_installation()
        original_launcher = jaro.read_bytes()
        real_mv = shutil.which('mv')
        self.stub('mv', f'''#!/bin/bash
case "$*" in
  *'/new/jaro '*) exit 1 ;;
  *) exec "{real_mv}" "$@" ;;
esac
''')
        result = self.run_cli(jaro, 'update')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual((runtime / 'VERSION').read_text(), '6.0.0\n')
        self.assertEqual(jaro.read_bytes(), original_launcher)
        self.assertEqual(list(self.prefix.glob('*/.jaromail-install.*')), [])


if __name__ == '__main__':
    unittest.main()
