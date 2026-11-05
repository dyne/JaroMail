#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
if ! command -v python3 >/dev/null 2>&1; then
  print -- "SKIP test-filter-contract.sh: missing python3"
  exit 77
fi
python3 "$script_dir/lib/filter_contract.py" "$script_dir/fixtures/filter-contract/interfaces.json"
