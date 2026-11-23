#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
if ! python3 "$repo_root/build/cmake-build.py" --output-dir "$repo_root/build/gnu" --testing --target jaro-dedup-test > "$tmp_root/build.log" 2>&1; then
    cat "$tmp_root/build.log"
    exit 1
fi
if ! python3 "$repo_root/build/cmake-build.py" --output-dir "$repo_root/build/gnu" --testing --target jaro-dedup > "$tmp_root/build-production.log" 2>&1; then
    cat "$tmp_root/build-production.log"
    exit 1
fi
python3 "$repo_root/extras/test/dedup_scratch_test.py"
