#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
for target in jaro-dedup-test jaro-dedup; do
    if ! python3 "$repo_root/build/cmake-build.py" --output-dir "$repo_root/build/gnu" --testing --target "$target" > "$tmp_root/build.log" 2>&1; then
        cat "$tmp_root/build.log"
        exit 1
    fi
done
python3 "$repo_root/extras/test/dedup_classify_test.py"
