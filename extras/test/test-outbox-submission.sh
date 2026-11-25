#!/usr/bin/env zsh
set -euo pipefail
source "${0:A:h}/lib/test_helpers.zsh"
test_setup "$0"
trap test_cleanup EXIT
python3 "$repo_root/extras/test/outbox_submission_test.py" "$prefix_root"
