#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: SIEVE_DEPS_PREFIX required for native batch/Sieve decisions'; exit 77; }
python3 "${0:A:h}/filter_batch_test.py"
