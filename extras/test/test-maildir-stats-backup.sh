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
  print -- "SKIP test-maildir-stats-backup.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init >/dev/null
maildir_fixture_ensure "${mail_root}/archive"

domain_input="Alice <alice@example.org>
Bob <bob@example.org>
Carol <carol@dyne.org>"
domain_stat="$(
  print -- "${domain_input}" \
    | jaro_source stat domain 2>/dev/null \
    | sed -E 's/#[#]*//g' \
    | tr -s ' ' \
    | sed 's/[[:space:]]*$//' \
    | awk '{print $1" "$NF}' \
    | sort
)"
domain_expected="1 dyne.org
2 example.org"
assert_equal "${domain_stat}" "${domain_expected}" "stat domain counts"

folder_input="${mail_root}/known/new/a
${mail_root}/known/cur/b
${mail_root}/archive/new/c"
folder_stat="$(
  print -- "${folder_input}" \
    | jaro_source stat folder 2>/dev/null \
    | sed -E 's/#[#]*//g' \
    | tr -s ' ' \
    | sed 's/[[:space:]]*$//' \
    | awk '{print $1" "$NF}' \
    | sort
)"
folder_expected="1 archive
2 known"
assert_equal "${folder_stat}" "${folder_expected}" "stat folder counts"

make_maildir_message "${mail_root}/known" "new" "backup-a.eml" \
  "From: Source <source@example.org>" \
  "To: Target <target@example.org>" \
  "Subject: backup-token-a" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

make_maildir_message "${mail_root}/known" "new" "backup-b.eml" \
  "From: Source <source@example.org>" \
  "To: Target <target@example.org>" \
  "Subject: backup-token-b" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

jaro_source update >/dev/null
jaro_source index >/dev/null

backup_stderr="${tmp_root}/backup.err"
set +e
jaro_source backup "${mail_root}/archive" "subject:backup-token" >/dev/null 2>"${backup_stderr}"
backup_status=$?
set -e

assert_equal "${backup_status}" "0" "backup current exit code"
assert_equal "$(find "${mail_root}/archive" -maxdepth 2 -type f | wc -l | tr -d ' ')" "0" "backup destination unchanged on failure"
