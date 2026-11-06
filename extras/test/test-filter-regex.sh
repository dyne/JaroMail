#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: regex-enabled SIEVE_DEPS_PREFIX required'; exit 77; }
python3 "${0:A:h}/filter_regex_test.py"
