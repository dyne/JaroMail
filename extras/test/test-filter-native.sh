#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
python3 "$script_dir/filter_native_test.py"
