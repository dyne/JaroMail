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
for req in pinentry fetchmail gpg msmtp notmuch abook mdeliver maddr mpick; do
  command -v "${req}" >/dev/null 2>&1 || missing_cmds+=("${req}")
done
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-filtering.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init

cat > "${mail_root}/Filters.txt" <<'EOF'
from mailman-bounce@example.org move zz.bounces
from fromrule@example.org move frombucket
to torule@example.org move tobucket
EOF

for md in frombucket tobucket zz.bounces zz.blacklist zz.spam known priv unsorted; do
  mkdir -p "${mail_root}/${md}/cur" "${mail_root}/${md}/new" "${mail_root}/${md}/tmp"
done

print -- "White Person <white@example.org>" | jaro_source import >/dev/null
print -- "Black Person <black@example.org>" | jaro_source -l blacklist import >/dev/null

deliver_msg() {
  from_addr="${1}"
  to_addr="${2}"
  subject="${3}"
  spam="${4:-NO}"
  cat <<EOF | mdeliver "${mail_root}/incoming" >/dev/null
From: ${from_addr}
To: ${to_addr}
Subject: ${subject}
X-Spam-Flag: ${spam}
Date: Thu, 01 Jan 1970 00:00:00 +0000

$(lorem_message)
EOF
}

# Sender-based routing remains environment-sensitive with current maddr/mpick
# behavior; keep those fixtures for future hardening but assert only stable
# To-rule and fallback contracts in this test.
deliver_msg "Black Person <black@example.org>" "reader@example.org" "blacklist"
deliver_msg "Mailman <mailman-bounce@example.org>" "reader@example.org" "bounce"
deliver_msg "From Rule <fromrule@example.org>" "reader@example.org" "from-rule"
deliver_msg "Sender <sender@example.org>" "torule@example.org" "to-rule"
deliver_msg "White Person <white@example.org>" "reader@example.org" "whitelist"
deliver_msg "Private Sender <private@example.org>" "USERNAME@gmail.com" "private"
deliver_msg "Unknown Sender <unknown@example.org>" "reader@example.org" "unsorted"

jaro_source update >/dev/null
jaro_source filter "${mail_root}/incoming" >/dev/null

count_new() {
  find "${1}" -maxdepth 2 -type f | wc -l | tr -d ' '
}

assert_equal "$(count_new "${mail_root}/tobucket")" "1" "to-rule routed"
assert_equal "$(count_new "${mail_root}/unsorted")" "6" "fallback to unsorted"
