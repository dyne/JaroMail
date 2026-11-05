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

parsedate_out="$(TZ=UTC0 "${parsedate_bin}" "Thu, 01 Jan 1970 00:00:00 +0000")"
assert_equal "${parsedate_out}" "0" "parsedate epoch in UTC"
if "${parsedate_bin}" 'invalid date' >/dev/null 2>&1; then
  print -u2 -- 'ASSERT FAIL: parsedate accepted an invalid date'; exit 1
fi

lock_target="${tmp_root}/dotlock-target"
print -- "lock me" > "${lock_target}"
"${dotlock_bin}" -r 0 "${lock_target}"
[[ -f "${lock_target}.lock" ]]
if "${dotlock_bin}" -r 0 "${lock_target}"; then
  print -u2 -- 'ASSERT FAIL: contending dotlock succeeded'; exit 1
else
  assert_equal "$?" 3 "dotlock contention status"
fi
"${dotlock_bin}" -u "${lock_target}"
[[ ! -e "${lock_target}.lock" ]]
print -- 'PASS: native address, date and locking helpers'
