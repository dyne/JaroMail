#!/usr/bin/env zsh
source "${0:A:h}/lib/test_helpers.zsh"
for dependency in zsh python3 fetchmail; do
  command -v "$dependency" >/dev/null || { print -u2 -- "SKIP: missing $dependency"; exit 77; }
done
real_fetchmail="$(command -v fetchmail)"
test_setup "$0"
trap test_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export HOME="$tmp_root/home" JARO_KEYRING=fixture
export IMAP_TEST_STATE="$tmp_root/state" IMAP_TEST_REAL_FETCHMAIL="$real_fetchmail"
mkdir -p "$HOME" "$IMAP_TEST_STATE"

cat > "$test_bin/openssl" <<'EOF'
#!/bin/sh
printf 'openssl\n' >> "$IMAP_TEST_STATE/transport-calls"
cat > "$IMAP_TEST_STATE/query"
cat "$IMAP_TEST_STATE/response"
exit "$(cat "$IMAP_TEST_STATE/status")"
EOF
cat > "$test_bin/nc" <<'EOF'
#!/bin/sh
# Reachability fixture only; no socket is opened.
exit 0
EOF
cat > "$test_bin/notmuch" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$test_bin/pinentry" <<'EOF'
#!/bin/sh
printf 'unexpected pinentry\n' >&2
exit 97
EOF
cat > "$test_bin/fetchmail" <<'EOF'
#!/usr/bin/env python3
# No remote fetch. Use the real parser's configdump mode to inspect the exact
# downstream mailbox tokens, rather than approximating fetchmail with shlex.
import ast, json, os, pathlib, subprocess, sys
state = pathlib.Path(os.environ['IMAP_TEST_STATE'])
config = sys.stdin.read()
with (state / 'fetch-calls').open('a') as log:
    log.write(json.dumps(sys.argv[1:]) + '\n')
result = subprocess.run([os.environ['IMAP_TEST_REAL_FETCHMAIL'], '--configdump', '-f', '-'],
                        input=config, text=True, capture_output=True, timeout=5)
if result.returncode:
    sys.stderr.write(result.stderr)
    sys.exit(96)
tree = ast.parse(result.stdout)
value = next(node.value for node in tree.body if isinstance(node, ast.Assign)
             and isinstance(node.targets[0], ast.Name) and node.targets[0].id == 'fetchmailrc')
class Booleans(ast.NodeTransformer):
    def visit_Name(self, node):
        if node.id not in ('TRUE', 'FALSE'):
            raise ValueError('unexpected name in configdump')
        return ast.Constant(node.id == 'TRUE')
parsed = ast.literal_eval(Booleans().visit(value))
names = parsed['servers'][0]['users'][0]['mailboxes']
(state / 'folders.json').write_text(json.dumps(names))
sys.exit(1 if '-c' in sys.argv else 0) # Successful empty poll, no download.
EOF
chmod +x "$test_bin/"{openssl,nc,notmuch,pinentry,fetchmail}
jaro_source -q init > "$IMAP_TEST_STATE/init.log" 2>&1

source "$repo_root/src/zlibs/imap"
source "$repo_root/src/zlibs/accounts"
fn() { :; }; func() { :; }; act() { :; }; notice() { :; }; warning() { :; }
error() { print -u2 -r -- "$*"; }

account_fixture() {
  cat > "$mail_root/Accounts/default" <<'EOF'
name fixture
email fixture@example.org
login fixture
password secret
proto imap
imap localhost
imap_port 993
transport TLS1
cert check
options keep
EOF
}
header() { printf '%s\r\n' '* OK ready' 'B00000 OK capability' 'B00001 OK authenticated'; }
footer() { printf '%s\r\n' 'B00002 OK listed' '* BYE logout' 'B00003 OK logout'; }
response_fixture() {
  { header; (( $# == 0 )) || printf '%s\r\n' "$@"; footer; } > "$IMAP_TEST_STATE/response"
  print 0 > "$IMAP_TEST_STATE/status"
}
reset_calls() { rm -f "$IMAP_TEST_STATE/"{fetch-calls,transport-calls,folders.json}; }
assert_status() {
  local expected="$1" actual=0
  shift
  "$@" || actual=$?
  assert_equal "$actual" "$expected" "command status: ${case_name:-module} $1"
}
cli_list() { jaro_source -q imap listfolders > "$IMAP_TEST_STATE/list.out" 2> "$IMAP_TEST_STATE/list.err"; }
cli_fetch() { jaro_source -q fetch > "$IMAP_TEST_STATE/fetch.out" 2> "$IMAP_TEST_STATE/fetch.err"; }
reject_both() {
  local case_name="$1" expected="${2:-1}"
  reset_calls
  assert_status "$expected" cli_list
  [[ ! -s "$IMAP_TEST_STATE/list.out" ]]
  assert_status "$expected" cli_fetch
  [[ ! -e "$IMAP_TEST_STATE/fetch-calls" ]]
  print -r -- "PASS: CLI list/fetch reject $case_name atomically"
}
assert_fetch_folders() {
  python3 - "$IMAP_TEST_STATE/folders.json" "$@" <<'PY'
import json, pathlib, sys
actual = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert actual == sys.argv[2:], (actual, sys.argv[2:])
PY
}

name='' imap='' proto=imap
assert_status 1 check_imap
name=fixture
assert_status 1 check_imap
imap=localhost proto=pop
assert_status 1 check_imap
proto=imap
assert_status 0 check_imap
print -- 'PASS: explicit check_imap success/failure'

account_fixture
response_fixture '* LIST () "/" INBOX' '* list (\HasNoChildren) NIL "Work projects"' \
  '* LIST () "/" "a\"b"' '* LIST () "/" "a\\b"' '* LIST () "/" " edge "' \
  $'* LIST () "/" "tab\tname"' '* LIST () "/" ""' '* LIST () "/" &AMk-' \
  '* LIST () "/" "$(touch NEVER)"' '* LIST () "/" "a\\x22"' '* LIST () "/" -dash' \
  '* LIST () "/" a}b' '* LIST () "/" "a*%]"'
expected_folders=(INBOX 'Work projects' 'a"b' 'a\b' ' edge ' $'tab\tname' '' '&AMk-' '$(touch NEVER)' 'a\x22' -dash 'a}b' 'a*%]')
reset_calls
cli_list
print -rl -- "${expected_folders[@]}" > "$IMAP_TEST_STATE/expected.out"
cmp "$IMAP_TEST_STATE/list.out" "$IMAP_TEST_STATE/expected.out"
cli_fetch
assert_fetch_folders "${expected_folders[@]}"
[[ "$(wc -l < "$IMAP_TEST_STATE/fetch-calls" | tr -d ' ')" == 1 ]]
[[ ! -e NEVER ]]
print -- 'PASS: CLI LIST and real fetchmail parser preserve 13 exact folder boundaries'

reset_calls
assert_status 1 jaro_source -q -a missing imap listfolders > "$IMAP_TEST_STATE/missing.out" 2> "$IMAP_TEST_STATE/missing.err"
[[ ! -s "$IMAP_TEST_STATE/missing.out" ]]
assert_status 1 jaro_source -q -a missing fetch >> "$IMAP_TEST_STATE/missing.out" 2>> "$IMAP_TEST_STATE/missing.err"
[[ ! -e "$IMAP_TEST_STATE/transport-calls" && ! -e "$IMAP_TEST_STATE/fetch-calls" ]]
print -- 'PASS: missing account stops both CLI paths before transport'

response_fixture
reset_calls
cli_list
[[ ! -s "$IMAP_TEST_STATE/list.out" ]]
assert_status 1 cli_fetch
[[ ! -e "$IMAP_TEST_STATE/fetch-calls" ]]
print -- 'PASS: successful empty LIST is distinct from fetch with no selected folders'

response_fixture '* LIST () "/" INBOX'
print 69 > "$IMAP_TEST_STATE/status"
reject_both 'transport status' 69
print 0 > "$IMAP_TEST_STATE/status"
{ printf '%s\r\n' '* OK ready' 'B00000 OK capability' 'B00001 NO authentication rejected'; footer; } > "$IMAP_TEST_STATE/response"
reject_both 'tagged NO'
{ header; printf '%s\r\n' '* LIST () "/" INBOX' 'B00002 BAD list rejected' 'B00003 OK logout'; } > "$IMAP_TEST_STATE/response"
reject_both 'tagged BAD after mailbox'
{ header; printf '%s\r\n' '* LIST () "/" INBOX' 'B00002 OK listed'; } > "$IMAP_TEST_STATE/response"
reject_both 'missing logout completion'
{ header; printf '%s\r\n' 'B00002 OK listed'; printf '%s' 'B00003 OK logout'; } > "$IMAP_TEST_STATE/response"
reject_both 'unterminated response line'
response_fixture '* LIST () "/" "unterminated'
reject_both 'truncated quoted mailbox'
response_fixture '* LIST () "/" {12}' 'literal name'
reject_both 'unsupported literal mailbox'
assert_contains "$(cat "$IMAP_TEST_STATE/list.err")" 'literal mailbox names are unsupported' 'literal diagnostic'
response_fixture '* LIST () "/" "INBOX" (CHILDINFO (SUBSCRIBED))'
reject_both 'unsupported LIST extension'
response_fixture '* LIST () "/" "bad\qescape"'
reject_both 'invalid quoted escape'
response_fixture '* LIST () "/" invalid*atom'
reject_both 'wildcard outside quoted mailbox'
response_fixture '* LIST () "/" "Föld"'
reject_both 'unsupported UTF-8'
response_fixture '* BYE disconnected'
reject_both 'early BYE'
response_fixture 'B00002 OK premature' '* LIST () "/" INBOX'
for attempt in 1 2 3 4 5; do
  reject_both "duplicate or out-of-order completion (attempt $attempt)"
done
{ header; printf '* LIST () "/" "%017000d"\r\n' 0; footer; } > "$IMAP_TEST_STATE/response"
reject_both 'oversized response line'
{ header; repeat 70 printf '* CAPABILITY %016000d\r\n' 0; footer; } > "$IMAP_TEST_STATE/response"
reject_both 'oversized response total'
{ header; repeat 10001 printf '%s\r\n' '* LIST () "/" INBOX'; footer; } > "$IMAP_TEST_STATE/response"
reject_both 'excessive mailbox count'

account_fixture
print 'proto invalid' >> "$mail_root/Accounts/default"
response_fixture '* LIST () "/" INBOX'
reject_both 'malformed account'
[[ ! -e "$IMAP_TEST_STATE/transport-calls" ]]

# Configured folders bypass discovery, retaining the same exact downstream
# boundaries even if inspection transport would fail.
account_fixture
_account_write_field folders "${expected_folders[@]}" >> "$mail_root/Accounts/default"
print 69 > "$IMAP_TEST_STATE/status"
reset_calls
cli_fetch
assert_fetch_folders "${expected_folders[@]}"
[[ ! -e "$IMAP_TEST_STATE/transport-calls" ]]
print -- 'PASS: configured folder boundaries preserved without discovery'

account_fixture
reset_calls
jaro_source -q fetch 'Work projects' 'a"b' 'a\b' > "$IMAP_TEST_STATE/explicit.out" 2> "$IMAP_TEST_STATE/explicit.err"
assert_fetch_folders 'Work projects' 'a"b' 'a\b'
[[ ! -e "$IMAP_TEST_STATE/transport-calls" ]]
print -- 'PASS: explicit CLI folder arguments retain boundaries and bypass discovery'

account_fixture
print 'exclude ".*"' >> "$mail_root/Accounts/default"
response_fixture '* LIST () "/" INBOX'
reset_calls
assert_status 1 cli_fetch
[[ ! -e "$IMAP_TEST_STATE/fetch-calls" ]]
print -- 'PASS: all excluded folders stop before fetchmail default selection'

account_fixture
_account_write_field exclude '\[1\]' >> "$mail_root/Accounts/default"
response_fixture '* LIST () "/" "a[1]"' '* LIST () "/" a1' '* LIST () "/" keep'
reset_calls
cli_fetch
assert_fetch_folders a1 keep
print -- 'PASS: exclusions remove exact names containing glob characters'

# Retain the real transport/watchdog while stubbing only its executable. Nested
# Zsh pipeline status collection previously masked this parser error sometimes.
name=fixture proto=imap imap=localhost host=localhost imap_port=993 login=fixture password=secret
transport=TLS1 cert=check certfile=''
openssl() { cat > /dev/null; cat "$IMAP_TEST_STATE/response"; }
mktemp() {
  local allocated
  allocated="$(command mktemp "$@")" || return 1
  print -r -- "$allocated" >> "$IMAP_TEST_STATE/response-files"
  print -r -- "$allocated"
}
{ header; printf '%s\r\n' '* LIST () "/" INBOX' 'B00002 OK listed'; } > "$IMAP_TEST_STATE/response"
for attempt in {1..100}; do
  assert_status 1 _imap_discover_folders 2>> "$IMAP_TEST_STATE/repeated-errors"
  assert_equal "${#reply}" 0 'failed discovery never retains names'
done
while IFS= read -r owned_file; do
  [[ ! -e "$owned_file" ]]
done < "$IMAP_TEST_STATE/response-files"
print -- 'PASS: 100 transport/watchdog parser rejections preserve status and remove private response files'

# The other discovery caller also reports protocol/transport failures and uses
# the shared quoted-string encoder when selecting a folder.
run_imap_query() {
  cat > "$IMAP_TEST_STATE/size-query"
  cat "$IMAP_TEST_STATE/response"
  return "$(cat "$IMAP_TEST_STATE/status")"
}
name=fixture proto=imap imap=localhost host=localhost imap_port=993 login=fixture password=secret
{ header; printf '%s\r\n' 'B00002 OK selected' '* 1 FETCH (RFC822.SIZE 42)' 'B00003 OK fetched' 'B00004 OK logout'; } > "$IMAP_TEST_STATE/response"
print 0 > "$IMAP_TEST_STATE/status"
assert_equal "$(imap_get_size 'a"b\c')" 42 'size query success'
assert_contains "$(cat "$IMAP_TEST_STATE/size-query")" 'SELECT "a\"b\\c"' 'size folder encoding'
{ header; printf '%s\r\n' 'B00002 NO selection rejected' 'B00003 OK fetched' 'B00004 OK logout'; } > "$IMAP_TEST_STATE/response"
assert_status 1 imap_get_size INBOX
{ header; printf '%s\r\n' 'B00002 OK selected'; } > "$IMAP_TEST_STATE/response"
assert_status 1 imap_get_size INBOX
print 71 > "$IMAP_TEST_STATE/status"
assert_status 71 imap_get_size INBOX
print -- 'PASS: size caller preserves encoded names and protocol/transport failures'
