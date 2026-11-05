#!/usr/bin/env zsh
set -euo pipefail
repo_root="${0:A:h:h:h}"
if [[ -z ${SIEVE_DEPS_PREFIX:-} ]]; then
  print -- 'SKIP sieve-oracles: set SIEVE_DEPS_PREFIX to the matched 2.4.5 development prefix'
  exit 77
fi
python3 "$repo_root/extras/test/sieve-oracles.py"
