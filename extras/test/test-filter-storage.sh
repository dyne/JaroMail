#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: SIEVE_DEPS_PREFIX required for native apply/Sieve decisions'; exit 77; }
python3 "${0:A:h}/filter_storage_test.py"
