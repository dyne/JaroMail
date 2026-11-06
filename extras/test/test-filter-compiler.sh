#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: SIEVE_DEPS_PREFIX required for generated compiler semantic validation'; exit 77; }
python3 "$script_dir/filter_compiler_test.py"
