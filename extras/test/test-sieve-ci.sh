#!/usr/bin/env zsh
set -euo pipefail
repo_root="${0:A:h:h:h}"
python3 "$repo_root/extras/test/sieve-ci.py"
