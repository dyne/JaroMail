#!/usr/bin/env zsh
set -euo pipefail
script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
for req in python3 pinentry fetchmail gpg msmtp notmuch abook mdeliver maddr mpick; do
  if ! command -v "$req" >/dev/null 2>&1; then
    print -- "SKIP test-filter-policy.sh: missing $req"
    exit 77
  fi
done
test_setup "$0"
trap test_cleanup EXIT
python3 "$script_dir/lib/filter_policy.py" "$repo_root" "$tmp_root" "$work_root"
