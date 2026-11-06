#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: SIEVE_DEPS_PREFIX required for independent parity'; exit 77; }
command -v abook >/dev/null || { print -u2 'SKIP: abook required for parity'; exit 77; }
python3 "${0:A:h}/filter_parity_test.py"
