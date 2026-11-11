#!/usr/bin/env zsh
# Account parsing is a local data operation: no transport or installed runtime.
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/accounts"
ACCOUNTS="$tmp_root/accounts"
mkdir -p "$ACCOUNTS"
account=literal

cat > "$ACCOUNTS/literal" <<'EOF'
  # leading whitespace comments are supported
name "Alice O'Connor  "; $(touch NEVER-CREATED) `touch ALSO-NEVER` $HOME * ? [abc]
email alice@example.org
login 'literal$(command);`command`$HOME*'
imap imap.example.org
smtp smtp.example.org
auth plain # shipped template has this comment
password "a\"b\\c # $(touch NEVER-CREATED)"
folders INBOX "Work projects" 'Single quoted' "a\"b" "a\\b" '$HOME*' '$(touch NEVER-CREATED)'
exclude "a b" literal#hash
my_hdr From: Alice <alice@example.org>
my_hdr X-Test: preserved
my_hdr X-Organization: O'Reilly
EOF
raw_header=$' \tmy_hdr X-Raw: O\'Reilly "quoted" \\literal # $HOME $(touch NEVER-CREATED)  '
printf '%s\n' "$raw_header" >> "$ACCOUNTS/literal"
read_account
assert_equal "$name" 'Alice O'"'"'Connor  ; $(touch NEVER-CREATED) `touch ALSO-NEVER` $HOME * ? [abc]' 'literal scalar and quoted name'
assert_equal "$login" 'literal$(command);`command`$HOME*' 'literal login metacharacters'
assert_equal "$password" 'a"b\c # $(touch NEVER-CREATED)' 'password quotes and backslash'
assert_equal "$auth" plain 'inline comment'
assert_equal "${#folders}" 7 'list boundaries'
assert_equal "${folders[2]}" 'Work projects' 'double quoted folder'
assert_equal "${folders[3]}" 'Single quoted' 'single quoted folder'
assert_equal "${folders[4]}" 'a"b' 'escaped quote'
assert_equal "${folders[5]}" 'a\b' 'escaped backslash'
assert_equal "${folders[6]}" '$HOME*' 'no variable or glob expansion'
assert_equal "${folders[7]}" '$(touch NEVER-CREATED)' 'no command substitution'
assert_equal "${exclude[2]}" literal#hash 'hash within word'
assert_equal "$my_hdr" $'my_hdr From: Alice <alice@example.org>\nmy_hdr X-Test: preserved\nmy_hdr X-Organization: O\'Reilly\n'"$raw_header" 'headers retain raw Mutt syntax and whitespace'
[[ ! -e NEVER-CREATED && ! -e ALSO-NEVER ]]

cat > "$ACCOUNTS/first" <<'EOF'
name first
login first@example.org
host legacy.example.org
port 00995
proto pop
password first-secret
options keep fetchall
folders INBOX
exclude ignored
certfile /tmp/first-ca
my_hdr X-First: yes
EOF
account=first
read_account
assert_equal "$imap" legacy.example.org 'legacy host alias'
assert_equal "$smtp" legacy.example.org 'legacy SMTP host fallback'
assert_equal "$imap_port:$smtp_port:$port" 00995:00995:00995 'legacy ports'
assert_equal "$proto" pop 'POP remains supported'

# No terminating newline is required; .txt fallback remains supported.
printf '%s' 'imap second.example.org' > "$ACCOUNTS/second.txt"
account=second
read_account
assert_equal "$password:$port:$certfile:$my_hdr" ::: 'credentials and optional scalars reset'
assert_equal "$accountopt" keep 'options reset to default'
assert_equal "$imap_port:$smtp_port" 993:587 'ports reset to defaults'
assert_equal "$proto:$name:$login:$email:$transport:$auth:$cert" imap:second:::TLS1:plain:ignore 'compatible defaults'
assert_equal "${#folders}:${#exclude}" 0:0 'arrays reset'

cat > "$ACCOUNTS/duplicate" <<'EOF'
imap first.example.org
imap last.example.org
folders one two
folders "last folder"
name first
name last
imap_port 1
smtp_port 65535
EOF
account=duplicate
read_account
assert_equal "$imap:$name:${#folders}:${folders[1]}" 'last.example.org:last:1:last folder' 'duplicates replace previous value'
assert_equal "$imap_port:$smtp_port" 1:65535 'inclusive port bounds'

account=bad
bad_account() {
    local directive="$1" diagnostic="$2"
    printf 'imap valid.example.org\n%s\n' "$directive" > "$ACCOUNTS/bad"
    if read_account > "$tmp_root/output" 2> "$tmp_root/error"; then
        print -u2 -r -- "ASSERT FAIL: accepted invalid directive: $directive"
        return 1
    fi
    assert_contains "$(cat "$tmp_root/error")" "$ACCOUNTS/bad:2:" 'file and line diagnostic'
    assert_contains "$(cat "$tmp_root/error")" "$diagnostic" 'actionable diagnostic'
    assert_equal "$password:$imap:${#folders}" ::0 'failed parse clears account state'
}
bad_account 'folders "unfinished' 'malformed quoting'
bad_account "name 'unfinished" 'malformed quoting'
bad_account 'password secret\' 'malformed quoting'
[[ "$(cat "$tmp_root/error")" != *secret* ]]
bad_account 'proto smtp' 'unsupported proto'
bad_account 'proto' 'missing value'
bad_account 'imap ""' 'missing value'
bad_account 'smtp "bad host"' 'invalid or missing smtp host'
bad_account 'host -option' 'invalid or missing smtp host'
bad_account 'smtp_port 0' 'from 1 to 65535'
bad_account 'imap_port 65536' 'from 1 to 65535'
bad_account 'port 1+2' 'from 1 to 65535'
bad_account 'port 999999999999999999999999' 'from 1 to 65535'
bad_account 'imap_port' 'missing value'
bad_account 'unknown private-value' 'unknown account directive'

account=first
read_account
account=missing
assert_status 1 read_account > "$tmp_root/missing.log" 2>&1
assert_equal "$password:$port:$accountopt:${#folders}" :::0 'missing account also clears state'

account=shipped
cp "$repo_root/docs/Accounts/default.txt" "$ACCOUNTS/shipped"
read_account
assert_equal "$auth" plain 'shipped template parses'
assert_equal "$smtp_port" 587 'shipped SMTP port'

# Existing SMTP batching explicitly requests credential reuse. Scope that
# request to the same account and SMTP identity after the parser reset.
smtp_keep_password=1
type=smtp host=$smtp port=$smtp_port password=batch-secret
read_account
assert_equal "$password" batch-secret 'explicit same-account SMTP batch reuse'
type=smtp host=$smtp port=$smtp_port password=batch-secret
account=second
read_account
assert_equal "$password" '' 'batch request does not carry password between accounts'
type=smtp host=$smtp port=$smtp_port password=batch-secret
printf '\nsmtp changed.example.org\n' >> "$ACCOUNTS/second.txt"
read_account
assert_equal "$password" '' 'changed SMTP identity invalidates cached password'
smtp_keep_password=0
password=old-secret
read_account
assert_equal "$password" '' 'ordinary reread resets password'

# The writer used by the wizard must preserve every scalar/list boundary,
# including empty entries and values ending in whitespace or a backslash.
roundtrip_name=$'  Alice "A" O\'Connor\\ # $HOME $(command)\t '
roundtrip_folders=(INBOX 'Work projects' "O'Connor" 'a"b' 'a\b' 'trailing\' '$HOME*' '$(touch NEVER-CREATED)' '' $'tab\tfolder')
{
  _account_write_field name "$roundtrip_name"
  _account_write_field imap localhost
  _account_write_field folders "${roundtrip_folders[@]}"
  _account_write_field exclude "${roundtrip_folders[@]}"
  _account_write_field password ''
} > "$ACCOUNTS/serialized"
account=serialized
read_account
assert_equal "$name" "$roundtrip_name" 'serialized scalar round trip'
assert_equal "${#folders}:${#exclude}" 10:10 'serialized list boundary count'
for (( folder_index=1; folder_index <= ${#roundtrip_folders}; folder_index++ )); do
  assert_equal "${folders[folder_index]}" "${roundtrip_folders[folder_index]}" 'serialized folder round trip'
  assert_equal "${exclude[folder_index]}" "${roundtrip_folders[folder_index]}" 'serialized exclusion round trip'
done
assert_equal "$password" '' 'serialized empty password'
[[ ! -e NEVER-CREATED ]]
print -- 'PASS: account data, quotes, validation, aliases, defaults and account isolation'
