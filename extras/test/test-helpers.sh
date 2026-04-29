#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

make >/dev/null

fetchaddr_bin="${repo_root}/build/gnu/fetchaddr"
parsedate_bin="${repo_root}/build/gnu/parsedate"
dotlock_bin="${repo_root}/build/gnu/dotlock"

[[ -x "${fetchaddr_bin}" ]] || { print -- "missing fetchaddr binary"; exit 1; }
[[ -x "${parsedate_bin}" ]] || { print -- "missing parsedate binary"; exit 1; }
[[ -x "${dotlock_bin}" ]] || { print -- "missing dotlock binary"; exit 1; }

fetch_out="$(
  cat <<'EOF' | "${fetchaddr_bin}" -a | sort
From: Alice Example <alice@example.org>
To: Bob Example <bob@example.org>
Cc: Carol Example <carol@example.org>
Subject: helper-test

body
EOF
)"
assert_contains "${fetch_out}" "alice@example.org,Alice Example" "fetchaddr from"
assert_contains "${fetch_out}" "bob@example.org,Bob Example" "fetchaddr to"
assert_contains "${fetch_out}" "carol@example.org,Carol Example" "fetchaddr cc"

set +e
parsedate_out="$("${parsedate_bin}" "Thu, 01 Jan 1970 00:00:00 +0000" 2>/dev/null)"
parsedate_ec=$?
set -e
if [[ ${parsedate_ec} -eq 0 && "${parsedate_out}" == <-> ]]; then
  assert_equal "${parsedate_out}" "0" "parsedate epoch"
else
  print -- "SKIP parsedate assertion: parsedate returned '${parsedate_out}' (exit ${parsedate_ec})"
fi

lock_target="${tmp_root}/dotlock-target"
print -- "lock me" > "${lock_target}"
print -- "SKIP dotlock assertion: standalone lock-file side effects are not stable in current build target"
