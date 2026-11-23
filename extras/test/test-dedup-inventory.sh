#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
if ! python3 "$repo_root/build/cmake-build.py" --output-dir "$repo_root/build/gnu" --testing --target dedup-inventory-driver > "$tmp_root/build.log" 2>&1; then
    cat "$tmp_root/build.log"
    exit 1
fi
python3 "$repo_root/extras/test/dedup_inventory_test.py"
