#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

bad_root="${tmp_root}/missing-workdir"
mkdir -p "${bad_root}"

output="$(
  JAROMAILDIR="${mail_root}" \
  JAROWORKDIR="${bad_root}" \
  "${repo_root}/src/jaro" source addr 2>&1
)"

assert_contains "${output}" "System in ${repo_root}/src" "bootstrap recovers workdir"
assert_contains "${output}" "Mail in ${mail_root}" "bootstrap keeps maildir"
