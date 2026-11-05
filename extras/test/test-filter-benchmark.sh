#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
source "$script_dir/lib/test_helpers.zsh"
for req in python3 pinentry fetchmail gpg msmtp notmuch abook mdeliver maddr mpick; do
  if ! command -v "$req" >/dev/null 2>&1; then
    print -- "SKIP test-filter-benchmark.sh: missing $req"
    exit 77
  fi
done
test_setup "$0"
trap test_cleanup EXIT
# Harness regression only. These single samples are not performance baselines.
python3 "$repo_root/extras/bench/filter_baseline.py" --repo "$repo_root" \
  --work "$work_root" --scratch "$tmp_root/bench-smoke" --output "$tmp_root/report" \
  --sizes 10 --messages 8 --runs 1
