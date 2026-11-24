#!/usr/bin/env zsh
set -euo pipefail

if ! command -v neomutt >/dev/null || [[ ! -d /dev/fd ]]; then
    print -- 'SKIP test-abook-consumers.sh: NeoMutt and /dev/fd are required'
    exit 77
fi

source "${0:A:h}/lib/module_helpers.zsh"
# The snapshot driver reads and compiles policy without the Sieve runtime.
if ! SIEVE_DEPS_PREFIX= DESTDIR= PREFIX="$tmp_root/staged" \
    python3 "$repo_root/build/cmake-build.py" --output-dir "$tmp_root/build" \
    --testing --install > "$tmp_root/build.log" 2>&1; then
    cat "$tmp_root/build.log"
    exit 1
fi
relocated="$tmp_root/relocated Anne's 100%s"
mv "$tmp_root/staged" "$relocated"
python3 "$repo_root/extras/test/abook-consumers-test.py" \
    --prefix "$relocated" --snapshot-driver "$tmp_root/build/filter-compiler-driver" \
    --client "$(command -v neomutt)"
