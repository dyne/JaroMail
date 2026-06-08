#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

export JAROMAILDIR="${mail_root}"
export JAROWORKDIR="${work_root}"
source "${repo_root}/src/zlibs/bootstrap"

print -- 'application/pdf xdg-open' > "${mail_root}/Applications.txt"
: > "${mail_root}/Filters.txt"

subcommand=open
mutt_exec=true
name='Test User'
email='test@example.org'
muttflags=''

x_mutt

mailcap_file="${mail_root}/.mutt/mailcap"
assert_contains "$(cat "${mailcap_file}")" "a=\"\${JAROMAILDIR:-${mail_root}}/tmp\" && mkdir -p \"\$a\"" "runtime attachment tmp path"
