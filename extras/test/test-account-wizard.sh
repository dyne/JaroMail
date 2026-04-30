#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

missing_cmds=()
for req in pinentry fetchmail gpg msmtp notmuch abook; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-account-wizard.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init >/dev/null

wizard_output="$(
  cat <<'EOF' | jaro_source wizard 2>&1
person@example.org






n
n
y
EOF
)"

account_file="${mail_root}/Accounts/person@example.org"
[[ -r "${account_file}" ]] || {
  print -- "ASSERT FAIL (wizard account file): missing ${account_file}"
  exit 1
}

assert_contains "${wizard_output}" "Account setup wizard" "wizard starts"
assert_contains "${wizard_output}" "Collecting account identity" "wizard identity step"

expected_lines="$(
  cat <<'EOF'
name person
email person@example.org
login person@example.org
proto imap
imap mail.example.org
imap_port 993
smtp mail.example.org
smtp_port 587
auth plain
cert check
transport TLS1
options keep
exclude zz.spam zz.bounces zz.blacklist zz.social
my_hdr From: person <person@example.org>
EOF
)"

assert_equal "$(awk '/^(name|email|login|proto|imap|imap_port|smtp|smtp_port|auth|cert|transport|options|exclude|my_hdr)/ { print }' "${account_file}")" "${expected_lines}" "wizard account shape"
