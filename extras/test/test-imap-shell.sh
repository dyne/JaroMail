#!/usr/bin/env zsh
source "${0:A:h}/lib/test_helpers.zsh"
for dependency in python3 openssl; do
  command -v "$dependency" >/dev/null || { print -u2 -- "SKIP: missing $dependency"; exit 77; }
done
test_setup "$0"
trap test_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export HOME="$tmp_root/home" JARO_KEYRING=fixture
mkdir -p "$HOME"
jaro_source -q init > "$tmp_root/init.log" 2>&1
python3 "$repo_root/extras/test/lib/imap_shell_fixture.py" "$repo_root" "$tmp_root" "$prefix_root"
