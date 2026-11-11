#!/usr/bin/env zsh
set -euo pipefail
source "${0:A:h}/lib/test_helpers.zsh"
test_setup "${0}"
trap test_cleanup EXIT
python3 "$repo_root/extras/test/outbox_controls_test.py" "$repo_root" "$prefix_root" "$tmp_root/controls"
