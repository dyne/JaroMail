#!/usr/bin/env zsh
set -euo pipefail
test_dir="${0:A:h}"
repo_root="${test_dir:h:h}"
bin_dir="${JARO_CONTACT_TEST_BIN_DIR:-$repo_root/build/gnu}"
if [[ -z ${JARO_CONTACT_TEST_BIN_DIR:-} ]]; then
    python3 "$repo_root/build/cmake-build.py" --output-dir "$bin_dir" --testing --target contact-migrate-driver
fi
python3 "$test_dir/contact_migrate_test.py" --bin-dir "$bin_dir"
