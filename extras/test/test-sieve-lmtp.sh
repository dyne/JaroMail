#!/usr/bin/env zsh
set -euo pipefail
[[ -n ${SIEVE_DEPS_PREFIX:-} ]] || { print -u2 'SKIP: pinned SIEVE_DEPS_PREFIX required'; exit 77; }
exec python3 "${0:A:h}/sieve_lmtp_test.py"
