#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: SIEVE_DEPS_PREFIX required for batch/apply Sieve comparisons'; exit 77; }
python3 "${0:A:h}/filter_delivery_oracle_test.py"
