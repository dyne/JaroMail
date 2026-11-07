#!/usr/bin/env zsh

set -euo pipefail
export JARO_FILTER_ENGINE=legacy # independent retained legacy observations

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
  exit 77
fi

jaro_source init

cat > "${mail_root}/Filters.txt" <<'EOF'
from mailman-bounce@example.org move zz.bounces
from fromrule@example.org move frombucket
to torule@example.org move tobucket
EOF

for md in frombucket tobucket zz.bounces zz.blacklist zz.spam known priv unsorted; do
  maildir_fixture_ensure "${mail_root}/${md}"
done

print -- "White Person <white@example.org>" | jaro_source import >/dev/null
print -- "Black Person <black@example.org>" | jaro_source -l blacklist import >/dev/null

deliver_msg() {
  from_addr="${1}"
  to_addr="${2}"
  subject="${3}"
  spam="${4:-NO}"
  msg_name="${subject//[^A-Za-z0-9._-]/_}.eml"
  make_maildir_message "${mail_root}/incoming" "new" "${msg_name}" \
    "From: ${from_addr}" \
    "To: ${to_addr}" \
    "Subject: ${subject}" \
    "X-Spam-Flag: ${spam}" \
    "Date: Thu, 01 Jan 1970 00:00:00 +0000" >/dev/null
}

# Retain independently observed legacy behavior. The native probes below must
# assert the intended destinations for every stage instead of this fallthrough.
deliver_msg "Black Person <black@example.org>" "reader@example.org" "blacklist"
deliver_msg "Mailman <mailman-bounce@example.org>" "reader@example.org" "bounce"
deliver_msg "From Rule <fromrule@example.org>" "reader@example.org" "from-rule"
deliver_msg "Sender <sender@example.org>" "torule@example.org" "to-rule"
deliver_msg "White Person <white@example.org>" "reader@example.org" "whitelist"
deliver_msg "Private Sender <private@example.org>" "USERNAME@gmail.com" "private"
deliver_msg "Unknown Sender <unknown@example.org>" "reader@example.org" "unsorted"

jaro_source update >/dev/null
filter_log="$(jaro_source filter "${mail_root}/incoming" 2>&1)"

count_new() {
  find "${1}" -maxdepth 2 -type f | wc -l | tr -d ' '
}

assert_equal "$(count_new "${mail_root}/tobucket")" "1" "to-rule routed"
assert_equal "$(count_new "${mail_root}/unsorted")" "6" "fallback to unsorted"
assert_contains "${filter_log}" "Filtering maildir: ${mail_root}/incoming (7 mails)" "filter summary line"
assert_contains "${filter_log}" "-> tobucket" "to-rule progress line"

# Run actual native apply on fresh named messages with the same real books.
# A qualified profile preserves these legacy ERE rule bytes without weakening
# them into globs. Source and destination bodies must be identical.
qualification="$(python3 -c 'import hashlib,pathlib,sys; print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())' \
    "$repo_root/docs/filter-regex-qualification.json")"
cat > "$mail_root/FilterProfile.txt" <<EOF
version 1
profile pigeonhole-2.4.5-posix-ere-v1
capability fileinto
capability mailbox
capability regex
regex-qualification $qualification
EOF
print -r -- 'native-own@example.org' > "$mail_root/Aliases.txt"
typeset -a native_ids=()
typeset -A native_destinations=()
native_probe() {
  local id="$1" destination="$2"
  native_ids+=("$id")
  native_destinations[$id]="$destination"
  shift 2
  local message_path
  message_path="$(make_maildir_message "$mail_root/incoming" new "native-$id.eml" \
    "$@" "Subject: Native $id" "Date: Thu, 01 Jan 1970 00:00:00 +0000")"
  cp -- "$message_path" "$tmp_root/expected-native-$id.eml"
}
native_probe blacklist zz.blacklist 'From: Black Person <black@example.org>'
native_probe bounce zz.bounces 'From: unrelated@example.org' 'Sender: list-mailman-bounce@example.org'
native_probe from-rule frombucket 'From: From Rule <fromrule@example.org>'
native_probe sender-rule frombucket 'From: unrelated@example.org' 'Sender: fromrule@example.org'
native_probe to-rule tobucket 'To: torule@example.org'
native_probe whitelist known 'From: White Person <white@example.org>'
native_probe private priv 'Cc: NATIVE-OWN@EXAMPLE.ORG'
native_probe spam zz.spam 'X-Spam-Flag: yes'
native_probe fallback unsorted 'From: unknown@example.org'
JARO_FILTER_ENGINE=native jaro_source filter "$mail_root/incoming" >/dev/null
for id in "${native_ids[@]}"; do
  destination="${native_destinations[$id]}"
  target="$mail_root/$destination/cur/native-$id.eml"
  [[ -f "$target" ]] || { print -u2 -- "FAIL native $id: expected $destination"; exit 1; }
  cmp -- "$tmp_root/expected-native-$id.eml" "$target" || { print -u2 -- "FAIL native $id: body changed"; exit 1; }
  [[ ! -e "$mail_root/incoming/new/native-$id.eml" ]] || { print -u2 -- "FAIL native $id: source retained after success"; exit 1; }
done
assert_equal "$(count_new "$mail_root/incoming")" "0" 'native input accounting'
print -- 'Native filtering identities: 9/9 destinations, exact bodies and source accounting'
