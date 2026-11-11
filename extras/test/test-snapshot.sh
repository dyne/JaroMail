#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
# Compile-time controls are absent from the installed production helper.
if ! python3 "$repo_root/build/cmake-build.py" --testing > "$tmp_root/build.log" 2>&1; then
    cat "$tmp_root/build.log"
    exit 1
fi
python3 "$repo_root/extras/test/snapshot_test.py"
