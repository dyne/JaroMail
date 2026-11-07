"""Exercise the release installer without network access or a real user prefix."""
import hashlib
import io
import os
from pathlib import Path
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


if __name__ == '__main__':
    unittest.main()
