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
mutt_command="$(command -v neomutt || command -v mutt)" || missing_cmds+=(neomutt)
if (( ${#missing_cmds[@]} > 0 )); then
  print -- "SKIP test-account-wizard.sh: missing runtime commands: ${missing_cmds[*]}"
  exit 77
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

wizard_input() {
  printf '%s\n' "$1" "${2:-}" "${3:-}" "${4:-}" "${5:-}" "${6:-}" "${7:-}" n n y
}

assert_no_staged_accounts() {
  local -a stages=("$mail_root/Accounts"/.wizard.*(N))
  assert_equal "${#stages}" 0 'private staging files cleaned'
}

# Run the actual wizard with literal values, then read its published file using
# the production parser. No reachability checks or credentials are requested.
quoted_name='  Alice O'"'"'Connor "A" \ # $HOME $(touch NEVER-CREATED) `touch '"$tmp_root"'/header-command` *  '
quoted_login='literal\login "quoted" $HOME $(touch NEVER-CREATED)'
wizard_output="$(wizard_input quoted@example.org "$quoted_name" "$quoted_login" '' 1 '' 65535 | jaro_source wizard 2>&1)"
assert_contains "$wizard_output" 'Account configuration saved' 'quoted account published'
assert_no_staged_accounts
zmodload zsh/stat
zstat -A account_modes +mode "$mail_root/Accounts/quoted@example.org"
(( (account_modes[1] & 8#777) == 8#600 )) || {
  print -u2 -- 'ASSERT FAIL: wizard account is not private (0600)'
  exit 1
}

# Reading the generated data needs neither bootstrap nor network tools.
source "$repo_root/src/zlibs/accounts"
fn() { :; }; func() { :; }; act() { :; }; notice() { :; }; warning() { :; }
error() { print -u2 -r -- "$*"; }
ACCOUNTS="$mail_root/Accounts"
account=quoted@example.org
read_account
assert_equal "$name" "$quoted_name" 'wizard display name round trip'
assert_equal "$login" "$quoted_login" 'wizard login round trip'
assert_equal "$imap_port:$smtp_port" 1:65535 'wizard port bounds'
assert_equal "${#exclude}:${exclude[1]}:${exclude[4]}" 4:zz.spam:zz.social 'wizard list round trip'
[[ ! -e NEVER-CREATED ]]
grep '^my_hdr ' "$ACCOUNTS/quoted@example.org" > "$tmp_root/header.muttrc"
mkdir "$tmp_root/mutt-home"
header_query_status=0
HOME="$tmp_root/mutt-home" MAIL="$tmp_root/empty-mail" "$mutt_command" -n -F "$tmp_root/header.muttrc" -Q realname > "$tmp_root/header-query.log" 2> "$tmp_root/header-query.err" || header_query_status=$?
# Match the existing Mutt query fixture's NeoMutt status convention; any config
# error must still produce no passing result (stderr is checked separately).
if [[ -s "$tmp_root/header-query.err" ]] || (( header_query_status > 1 )); then
  print -u2 -- 'ASSERT FAIL: Mutt rejected wizard header'
  cat "$tmp_root/header-query.err" >&2
  exit 1
fi
assert_contains "$(<"$tmp_root/header-query.log")" real 'Mutt processed header configuration'
[[ ! -e "$tmp_root/header-command" ]] || {
  print -u2 -- 'ASSERT FAIL: Mutt evaluated wizard display name'
  exit 1
}

# Existing files, .txt accounts and symlinks (including dangling links) remain
# untouched. EOF also terminates a scripted wizard without publishing anything.
original_account="$(<"$account_file")"
if printf '%s\n' person@example.org | jaro_source wizard > "$tmp_root/existing.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: existing account accepted'
  exit 1
fi
assert_equal "$(<"$account_file")" "$original_account" 'existing account preserved'
cp "$account_file" "$ACCOUNTS/legacy@example.org.txt"
ln -s "$tmp_root/absent-target" "$ACCOUNTS/symlink@example.org"
for wizard_email in legacy@example.org symlink@example.org; do
  if printf '%s\n' "$wizard_email" | jaro_source wizard > "$tmp_root/existing.log" 2>&1; then
    print -u2 -- 'ASSERT FAIL: existing logical account accepted'
    exit 1
  fi
done
[[ ! -e "$ACCOUNTS/legacy@example.org" && -L "$ACCOUNTS/symlink@example.org" ]]
assert_equal "$(<"$ACCOUNTS/legacy@example.org.txt")" "$original_account" '.txt account preserved'
if printf '%s\n' eof@example.org | jaro_source wizard > "$tmp_root/eof.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: incomplete input accepted'
  exit 1
fi
[[ ! -e "$ACCOUNTS/eof@example.org" ]]
if printf '%s\n' ../escape@example.org | jaro_source wizard > "$tmp_root/path.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: email pathname accepted'
  exit 1
fi
[[ ! -e "$mail_root/escape@example.org" ]]

for bad_port in 0 65536 not-a-port; do
  if wizard_input "bad-$bad_port@example.org" '' '' '' "$bad_port" | jaro_source wizard > "$tmp_root/invalid.log" 2>&1; then
    print -u2 -- 'ASSERT FAIL: invalid wizard port accepted'
    exit 1
  fi
  assert_contains "$(<"$tmp_root/invalid.log")" 'from 1 to 65535' 'shared wizard port validation'
  [[ ! -e "$ACCOUNTS/bad-$bad_port@example.org" ]]
done
if wizard_input bad-smtp@example.org '' '' '' '' '' 65536 | jaro_source wizard > "$tmp_root/invalid.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: invalid SMTP port accepted'
  exit 1
fi
assert_contains "$(<"$tmp_root/invalid.log")" 'smtp_port' 'SMTP field diagnostic'
[[ ! -e "$ACCOUNTS/bad-smtp@example.org" ]]
if wizard_input bad-host@example.org '' '' 'bad host' | jaro_source wizard > "$tmp_root/invalid.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: invalid wizard host accepted'
  exit 1
fi
assert_contains "$(<"$tmp_root/invalid.log")" 'invalid or missing imap host' 'shared wizard host validation'
[[ ! -e "$ACCOUNTS/bad-host@example.org" ]]
if wizard_input control@example.org $'invalid\rname' | jaro_source wizard > "$tmp_root/invalid.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: multiline directive value accepted'
  exit 1
fi
[[ ! -e "$ACCOUNTS/control@example.org" ]]
assert_no_staged_accounts

# A bad serializer must fail the parser check before the file is published.
if (
  _wizard_write_account() { print -r -- 'imap "malformed'; }
  wizard_input rejected@example.org | account_wizard
) > "$tmp_root/staged-validation.log" 2>&1; then
  print -u2 -- 'ASSERT FAIL: invalid staging file was published'
  exit 1
fi
assert_contains "$(<"$tmp_root/staged-validation.log")" 'malformed quoting' 'staged parser validation'
[[ ! -e "$ACCOUNTS/rejected@example.org" ]]
assert_no_staged_accounts

# Simulate another process creating the destination after validation. link(2)
# must refuse files, symlinks and directories at the exact destination path.
real_link="$(command -v link)"
cat > "$test_bin/link" <<'EOF'
#!/bin/sh
case "$WIZARD_RACE_KIND" in
  file) printf '%s\n' concurrent-account > "$2" ;;
  symlink) ln -s "$WIZARD_RACE_TARGET" "$2" ;;
  directory) mkdir "$2" ;;
esac
exec "$WIZARD_REAL_LINK" "$@"
EOF
chmod +x "$test_bin/link"
print -r -- untouched > "$tmp_root/race-target"
for race_kind in file symlink directory; do
  race_email="race-$race_kind@example.org"
  if wizard_input "$race_email" | WIZARD_RACE_KIND="$race_kind" WIZARD_RACE_TARGET="$tmp_root/race-target" WIZARD_REAL_LINK="$real_link" jaro_source wizard > "$tmp_root/race.log" 2>&1; then
    print -u2 -- 'ASSERT FAIL: concurrent account replaced'
    exit 1
  fi
  assert_contains "$(<"$tmp_root/race.log")" 'Cannot publish account without overwriting' 'publication race diagnostic'
  assert_no_staged_accounts
done
assert_equal "$(<"$ACCOUNTS/race-file@example.org")" concurrent-account 'concurrent file preserved'
[[ -L "$ACCOUNTS/race-symlink@example.org" && -d "$ACCOUNTS/race-directory@example.org" ]]
assert_equal "$(<"$tmp_root/race-target")" untouched 'concurrent symlink target preserved'
assert_equal "$(ls -A "$ACCOUNTS/race-directory@example.org")" '' 'no publication inside existing directory'
print -- 'PASS: wizard round trips, shared validation, private staging and no-overwrite publication'
