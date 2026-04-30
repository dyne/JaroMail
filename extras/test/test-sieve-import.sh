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
  print -- "SKIP test-sieve-import.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 0
fi

jaro_source init >/dev/null

sieve_file="${tmp_root}/import.sieve"
cat > "${sieve_file}" <<'EOF'
# mailbox supports fileinto :create
require ["fileinto","mailbox","variables"];

# zz.blacklist
if header :contains "From" [
"blocked@example.org",
"blocked@example.org"
]
{ fileinto :create "zz.blacklist"; stop; }

# bounces
if header :contains "Sender" "mailman-bounce" {
    fileinto :create "zz.bounces";
    stop;
}

#############
# own filters

if header :contains [ "To","Cc" ]  "team@example.org" { fileinto :create "team"; stop; }
if header :contains [ "From","Sender" ]  "list@example.org" { fileinto :create "lists"; stop; }

# INBOX
if header :contains [ "From","Sender" ] [
"Known Person <known@example.org>",
"known@example.org"
]
{ fileinto :create "INBOX"; stop; }

# spam
if header :is "X-Spam-Flag" "YES" {
    fileinto :create "Spam"; stop;
}

# priv
if header :contains [ "To","Cc" ]  [
"alias@example.org",
"USERNAME@gmail.com"
]
{ fileinto :create "priv"; stop; }

fileinto :create "unsorted";
EOF

jaro_source sieve-import "${sieve_file}" >/dev/null
jaro_source sieve-import "${sieve_file}" >/dev/null

filters="$(grep -v '^#' "${mail_root}/Filters.txt" | grep -v '^$' | sort)"
assert_equal "${filters}" $'from list@example.org move lists\nto team@example.org move team' "import filters"

aliases="$(grep -v '^#' "${mail_root}/Aliases.txt" | grep -v '^$' | sort)"
assert_equal "${aliases}" "alias@example.org" "import aliases"

whitelist="$(jaro_source addr 2>/dev/null | sort)"
assert_equal "${whitelist}" "Known Person <known@example.org>" "import whitelist"

blacklist="$(jaro_source -l blacklist addr 2>/dev/null | sort)"
assert_equal "${blacklist}" "blocked@example.org <blocked@example.org>" "import blacklist"

jaro_source update >/dev/null

generated="$(cat "${mail_root}/Filters.sieve")"
assert_contains "${generated}" '"team@example.org" { fileinto :create "team"; stop; }' "round-trip to filter"
assert_contains "${generated}" '"list@example.org" { fileinto :create "lists"; stop; }' "round-trip from filter"
assert_contains "${generated}" '"alias@example.org"' "round-trip alias"
assert_contains "${generated}" '"known@example.org"' "round-trip whitelist"
assert_contains "${generated}" '"blocked@example.org"' "round-trip blacklist"
