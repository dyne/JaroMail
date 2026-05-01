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
for req in pinentry fetchmail gpg msmtp notmuch abook mdeliver maddr; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-maildir-commands.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init >/dev/null

for md in incoming known unsorted archive; do
  maildir_fixture_ensure "${mail_root}/${md}"
done
maildir_fixture_ensure "${mail_root}/incoming/nested"

jaro_source ismd "${mail_root}/incoming" >/dev/null

cat <<'EOF' | jaro_source deliver archive >/dev/null
From: Deliver Person <deliver@example.org>
To: Target <target@example.org>
Subject: deliver-check
Date: Thu, 01 Jan 1970 00:00:00 +0000

fixture
EOF

archive_count="$(find "${mail_root}/archive" -maxdepth 2 -type f | wc -l | tr -d ' ')"
assert_equal "${archive_count}" "1" "deliver to archive maildir"

make_maildir_message "${mail_root}/incoming" "new" "a.eml" \
  "From: Alice <alice@example.org>" \
  "To: Bob <bob@example.org>" \
  "Cc: Carol <carol@example.org>" \
  "Subject: one" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

make_maildir_message "${mail_root}/incoming" "cur" "b.eml:2,S" \
  "From: Dan <dan@example.org>" \
  "To: Eve <eve@example.org>" \
  "Subject: two" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

make_maildir_message "${mail_root}/incoming" "tmp" "tmp.eml" \
  "From: Tmp <tmp@example.org>" \
  "To: Tmp <tmp@example.org>" \
  "Subject: tmp" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

listed_messages="$(
  zsh -lc '
    JAROMAILDIR="'"${mail_root}"'" \
    JAROWORKDIR="'"${work_root}"'" \
    PROGRAM=test VERSION=6.0 \
    source "'"${repo_root}"'/src/zlibs/bootstrap" source
    maildir_list_messages "'"${mail_root}"'/incoming"
  ' 2>/dev/null | sort
)"
assert_contains "${listed_messages}" "/incoming/new/a.eml" "maildir_list_messages includes new"
assert_contains "${listed_messages}" "/incoming/cur/b.eml:2,S" "maildir_list_messages includes cur"
if [[ "${listed_messages}" == *"/incoming/tmp/tmp.eml"* ]]; then
  print -- "ASSERT FAIL (maildir_list_messages tmp): tmp file should be ignored"
  exit 1
fi

listed_folders="$(
  zsh -lc '
    JAROMAILDIR="'"${mail_root}"'" \
    JAROWORKDIR="'"${work_root}"'" \
    PROGRAM=test VERSION=6.0 \
    source "'"${repo_root}"'/src/zlibs/bootstrap" source
    maildir_list_folders "'"${mail_root}"'"
  ' 2>/dev/null | sort
)"
assert_contains "${listed_folders}" "/incoming" "maildir_list_folders includes direct child"
assert_contains "${listed_folders}" "/archive" "maildir_list_folders includes archive"
if [[ "${listed_folders}" == *"/incoming/nested"* ]]; then
  print -- "ASSERT FAIL (maildir_list_folders nested): nested folder should be excluded"
  exit 1
fi

make_maildir_message "${mail_root}/known" "new" "move-src.eml" \
  "From: Move <move@example.org>" \
  "To: Dest <dest@example.org>" \
  "Subject: move" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

make_maildir_message "${mail_root}/known" "new" "copy-src.eml" \
  "From: Copy <copy@example.org>" \
  "To: Dest <dest@example.org>" \
  "Subject: copy" \
  "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null

zsh -lc '
  JAROMAILDIR="'"${mail_root}"'" \
  JAROWORKDIR="'"${work_root}"'" \
  PROGRAM=test VERSION=6.0 \
  source "'"${repo_root}"'/src/zlibs/bootstrap" source
  maildir_refile "'"${mail_root}"'/known/new/move-src.eml" "'"${mail_root}"'/archive"
  maildir_refile "'"${mail_root}"'/known/new/copy-src.eml" "'"${mail_root}"'/archive" copy
' >/dev/null 2>&1

if [[ -f "${mail_root}/known/new/move-src.eml" ]]; then
  print -- "ASSERT FAIL (maildir_refile move): source should be moved"
  exit 1
fi
if [[ ! -f "${mail_root}/known/new/copy-src.eml" ]]; then
  print -- "ASSERT FAIL (maildir_refile copy): source should remain"
  exit 1
fi
archive_refiled_count="$(find "${mail_root}/archive" -maxdepth 2 -type f | wc -l | tr -d ' ')"
assert_equal "${archive_refiled_count}" "3" "maildir_refile delivered messages"

sender_stdout="$(jaro_source extract "${mail_root}/incoming" sender 2>/dev/null)"
recipient_stdout="$(jaro_source extract "${mail_root}/incoming" recipient 2>/dev/null)"
all_stdout="$(jaro_source extract "${mail_root}/incoming" all 2>/dev/null)"

assert_equal "${sender_stdout}" "" "extract sender mode stdout"
assert_equal "${recipient_stdout}" "" "extract recipient mode stdout"
assert_equal "${all_stdout}" "" "extract all mode stdout"

help_output="$(jaro_source -h 2>/dev/null)"
if [[ "${help_output}" == *" publish "* ]]; then
  print -- "ASSERT FAIL (publish removed): help still lists publish command"
  exit 1
fi
