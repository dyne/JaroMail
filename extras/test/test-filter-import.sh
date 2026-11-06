#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
command -v abook >/dev/null || { print -u2 'SKIP: abook required'; exit 77; }
python3 "$script_dir/filter_import_test.py"
