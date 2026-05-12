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
for req in pinentry fetchmail gpg msmtp notmuch abook mdeliver; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-group-modes.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init >/dev/null

mkdir -p "${mail_root}/Groups"

queue_msg() {
  recipient="${1}"
  subject="${2}"
  cat <<EOF | jaro_source queue "${recipient}" >/dev/null
From: Sender Person <sender@example.org>
To: placeholder@example.org
Subject: ${subject}
Date: Thu, 01 Jan 1970 00:00:00 +0000

$(lorem_message)
EOF
}

count_outbox() {
  find "${mail_root}/outbox/new" -type f | wc -l | tr -d ' '
}

latest_outbox() {
  ls -1t "${mail_root}/outbox/new" | head -n1
}

# 1) individual mode + deduplicate (case-insensitive)
cat > "${mail_root}/Groups/team-individual" <<'EOF'
#mode individual
Alice <alice@myown.foundation>
alice@myown.foundation
Bob <bob@web3privacy.info>
EOF
queue_msg "team-individual@jaromail.group" "group-individual"
assert_equal "$(count_outbox)" "2" "individual sends one envelope per unique recipient"

# 2) no mode defaults to individual, comments are ignored, and mixed entries work
cat > "${mail_root}/Groups/team-default" <<'EOF'
# this group intentionally has no #mode
Alice <alice@myown.foundation>
# following comments should be ignored
bob@web3privacy.info
# another ignored comment
EOF
queue_msg "team-default@jaromail.group" "group-default"
assert_equal "$(count_outbox)" "4" "missing mode defaults to individual"

# 3) cc mode + Reply-To sender
cat > "${mail_root}/Groups/team-cc" <<'EOF'
#mode cc
Alice <alice@myown.foundation>
Bob <bob@web3privacy.info>
EOF
queue_msg "team-cc@jaromail.group" "group-cc"
cc_mail="${mail_root}/outbox/new/$(latest_outbox)"
cc_to_line="$(awk '/^To:/ { print; exit }' "${cc_mail}")"
assert_contains "${cc_to_line}" "alice@myown.foundation" "cc to contains recipient one"
assert_contains "${cc_to_line}" "bob@web3privacy.info" "cc to contains recipient two"
assert_equal "$(awk '/^Reply-To:/ { print; exit }' "${cc_mail}")" "Reply-To: sender@example.org" "cc reply-to sender"

# 4) bcc mode + hidden To + Reply-To sender
cat > "${mail_root}/Groups/team-bcc" <<'EOF'
#mode bcc
Alice <alice@myown.foundation>
Bob <bob@web3privacy.info>
EOF
queue_msg "team-bcc@jaromail.group" "group-bcc"
bcc_mail="${mail_root}/outbox/new/$(latest_outbox)"
assert_equal "$(awk '/^To:/ { print; exit }' "${bcc_mail}")" "To: undisclosed-recipients:;" "bcc hidden to"
assert_contains "$(awk '/^Bcc:/ { print; exit }' "${bcc_mail}")" "alice@myown.foundation" "bcc contains recipient one"
assert_contains "$(awk '/^Bcc:/ { print; exit }' "${bcc_mail}")" "bob@web3privacy.info" "bcc contains recipient two"
assert_equal "$(awk '/^Reply-To:/ { print; exit }' "${bcc_mail}")" "Reply-To: sender@example.org" "bcc reply-to sender"

# 5) validation: invalid entry rejects queue and postpones mail
cat > "${mail_root}/Groups/team-invalid" <<'EOF'
#mode individual
this-is-not-an-email
Alice <alice@myown.foundation>
EOF
if queue_msg "team-invalid@jaromail.group" "group-invalid"; then
  print -- "ASSERT FAIL (invalid group should fail)"
  exit 1
fi
assert_equal "$(find "${mail_root}/postponed/new" -type f | wc -l | tr -d ' ')" "1" "invalid group moves mail to postponed"
