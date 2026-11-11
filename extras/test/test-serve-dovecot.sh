#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
# CI already builds a matched Dovecot 2.4 development installation for Sieve.
if [[ -n ${SIEVE_DEPS_PREFIX:-} ]]; then
  export PATH="$SIEVE_DEPS_PREFIX/sbin:$SIEVE_DEPS_PREFIX/bin:$PATH"
fi

if ! command -v dovecot >/dev/null 2>&1 || ! command -v doveconf >/dev/null 2>&1; then
  print -- "SKIP test-serve-dovecot.sh: missing dovecot or doveconf"
  exit 77
fi
if [[ "$(dovecot --version)" != 2.4.<->* ]]; then
  print -- 'SKIP test-serve-dovecot.sh: Dovecot 2.4.x required'
  exit 77
fi
python3 "$script_dir/serve_dovecot_test.py"
