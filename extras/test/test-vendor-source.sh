#!/usr/bin/env zsh
set -euo pipefail
repo_root="${0:A:h:h:h}"
python3 "$repo_root/vendor/verify.py"
python3 - "$repo_root/vendor" <<'PY'
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1])
manifest = json.loads((source / 'sources.json').read_text())
expected = {
    'AUTHORS', 'COPYING', 'COPYING.LGPL',
    'src/sieve-tools/sievec.c', 'src/sieve-tools/sieve-test.c',
    'src/lib-sieve-tool/sieve-tool.c', 'src/lib-sieve-tool/sieve-tool.h',
    'src/lib-sieve/util/mail-raw.h',
}
assert {entry['path'] for entry in manifest['files']} == expected
abook = json.loads((source / 'abook/sources.json').read_text())
assert abook['upstream']['version'] == '0.6.2'
assert len(abook['files']) == 126
assert {'abook.c', 'database.c', 'filter.c', 'COPYING', 'AUTHORS', 'configure',
        'Makefile.in', 'po/fr.po'} <= {entry['path'] for entry in abook['files']}
with tempfile.TemporaryDirectory(prefix='jaromail-vendor-test-') as temp:
    fixture = Path(temp) / 'vendor'
    shutil.copytree(source, fixture)
    def rejects(reason):
        result = subprocess.run([sys.executable, str(fixture / 'verify.py')],
                                capture_output=True, text=True)
        assert result.returncode != 0 and reason in result.stderr, result

    target = fixture / 'pigeonhole/upstream/src/sieve-tools/sievec.c'
    original = target.read_bytes()
    target.write_bytes(original + b'\n/* changed fixture */\n')
    rejects('upstream source differs')
    target.unlink()
    rejects('extra or missing')
    target.write_bytes(original)
    extra = fixture / 'full-release.tar.gz'
    extra.write_bytes(b'archive fixture')
    rejects('extra or missing')
    extra.unlink()
    abook_target = fixture / 'abook/upstream/database.c'
    abook_original = abook_target.read_bytes()
    abook_target.write_bytes(abook_original + b'\n/* changed fixture */\n')
    rejects('upstream source differs')
    abook_target.unlink()
    rejects('extra or missing')
    abook_target.write_bytes(abook_original)
    abook_extra = fixture / 'abook/upstream/config.h'
    abook_extra.write_text('/* build output must stay outside vendor */\n')
    rejects('extra or missing')
    abook_extra.unlink()
    extra = fixture / 'pigeonhole/upstream/extra.c'
    extra.write_text('/* unneeded source */\n')
    rejects('extra or missing')
    extra.unlink()
    target.unlink()
    target.symlink_to(source / 'pigeonhole/upstream/src/sieve-tools/sievec.c')
    rejects('unexpected symlink')
print('PASS: minimal Sieve scope, complete abook scope and modified/missing/extra/symlink rejection')
PY
