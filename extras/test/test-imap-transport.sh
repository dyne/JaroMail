#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/imap"
export HOME="$tmp_root/home" JAROMAILDIR="$tmp_root/mail"
mkdir -p "$HOME" "$JAROMAILDIR"

for dependency in openssl python3; do
  command -v "$dependency" >/dev/null || { print -u2 -- "SKIP: missing $dependency"; exit 77; }
done
imap=localhost imap_port=993 cert=check certfile='' JARO_IMAP_TIMEOUT=3
openssl() { print -rl -- "$@" > "$tmp_root/args"; cat > "$tmp_root/input"; return ${fixture_status:-0}; }
nc() { print -rl -- "$@" > "$tmp_root/args"; cat > "$tmp_root/input"; }

for transport in TLS1 tls implicit SSL2 SSL23 SSL3 ssl starttls; do
  print -r -- query | run_imap_query
  args="$(cat "$tmp_root/args")"
  assert_contains "$args" '-verify_return_error' 'abort on verification failure'
  assert_contains "$args" '-verify_hostname' 'verify DNS name'
  case "$transport" in
    SSL2|SSL23|SSL3|ssl|starttls) assert_contains "$args" '-starttls' 'STARTTLS mapping' ;;
    *) [[ "$args" != *-starttls* ]] ;;
  esac
  assert_equal "$(cat "$tmp_root/input")" query 'query forwarded'
  print -r -- "PASS: transport mapping $transport"
done
transport=unknown
assert_status 1 run_imap_query < /dev/null
transport=TLS1 fixture_status=71
assert_status 71 run_imap_query < /dev/null
name=fixture host=localhost login=fixture password=secret
assert_status 71 imap_get_size INBOX < /dev/null
fixture_status=0 imap=127.0.0.1
run_imap_query < /dev/null
assert_contains "$(cat "$tmp_root/args")" '-verify_ip' 'verify IP SAN'
imap=localhost certfile="$tmp_root/missing.pem"
assert_status 1 run_imap_query < /dev/null
certfile="$tmp_root/ca.pem"
touch "$certfile"
run_imap_query < /dev/null
assert_contains "$(cat "$tmp_root/args")" "$certfile" 'custom trust'
certfile='' cert=ignore
warning() { print -u2 -r -- "$*"; }
run_imap_query < /dev/null 2> "$tmp_root/warning"
assert_contains "$(cat "$tmp_root/warning")" 'NOT verified' 'visible cert ignore'
[[ "$(cat "$tmp_root/args")" != *-verify_return_error* ]]
cert=invalid
assert_status 1 run_imap_query < /dev/null
cert=check transport=plain
run_imap_query < /dev/null 2> "$tmp_root/warning"
assert_contains "$(cat "$tmp_root/warning")" 'plaintext explicitly requested' 'visible plaintext'
assert_equal "$(cat "$tmp_root/args")" $'localhost\n993' 'plaintext arguments'
unset transport
run_imap_query < /dev/null
assert_contains "$(cat "$tmp_root/args")" s_client 'unset transport is encrypted'
JARO_IMAP_TIMEOUT=0
assert_status 1 run_imap_query < /dev/null
JARO_IMAP_TIMEOUT=3
print -- 'PASS: policy, validation and child failure propagation'

login='a"b\c' password='secret"\marker'
assert_equal "$(_imap_login_command)" 'B00001 LOGIN "a\"b\\c" "secret\"\\marker"' 'LOGIN quoting'
for invalid in $'bad\rLOGIN' $'bad\nLOGIN' $'bad\0LOGIN'; do
  password="$invalid"
  assert_status 1 _imap_login_command
  login="$invalid" password=secret
  assert_status 1 _imap_login_command
  login=fixture
done
password=marker-password login=marker-login
account=fixture option_params=(console)
read_account() { return 0; }
ask_password() { return 0; }
func() { print -u2 -r -- "$*"; }
cmd_imap > "$tmp_root/console-out" 2> "$tmp_root/debug"
[[ "$(cat "$tmp_root/debug")" != *marker-password* && "$(cat "$tmp_root/debug")" != *marker-login* ]]
assert_contains "$(cat "$tmp_root/input")" 'LOGIN "marker-login" "marker-password"' 'console encoded login'
print -- 'PASS: LOGIN quoting, injection rejection and debug redaction'

nc() {
  exec python3 -c '
import os, pathlib, signal, sys, time
root = pathlib.Path(sys.argv[1])
def terminate(*args):
    (root / "zero-child-term").write_text("handled TERM; exiting zero")
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
(root / "zero-child-pid").write_text(str(os.getpid()))
time.sleep(10)
' "$tmp_root"
}
transport=plain JARO_IMAP_TIMEOUT=1
started=$SECONDS
assert_status 124 run_imap_query < /dev/null 2> "$tmp_root/zero-timeout"
[[ -f "$tmp_root/zero-child-term" ]] # Handler was installed and actually ran.
(( SECONDS - started < 5 ))
child_pid="$(cat "$tmp_root/zero-child-pid")"
if kill -0 "$child_pid" 2>/dev/null; then
  print -u2 -- 'ASSERT FAIL: TERM-handled child still exists'
  exit 1
fi
assert_contains "$(cat "$tmp_root/zero-timeout")" deadline 'zero-exit timeout diagnostic'
print -- 'PASS: deadline fails despite child handling TERM and exiting zero; child joined'

openssl() {
  trap '' TERM
  print -r -- "$sysparams[pid]" > "$tmp_root/child-pid"
  zmodload zsh/zselect
  zselect -t 1000 || :
}
zmodload zsh/system
transport=TLS1 JARO_IMAP_TIMEOUT=1
started=$SECONDS
assert_status 124 run_imap_query < /dev/null 2> "$tmp_root/timeout"
(( SECONDS - started < 5 ))
child_pid="$(cat "$tmp_root/child-pid")"
if kill -0 "$child_pid" 2>/dev/null; then
  print -u2 -- 'ASSERT FAIL: timed-out owned child still exists'
  exit 1
fi
assert_contains "$(cat "$tmp_root/timeout")" deadline 'timeout diagnostic'
print -- 'PASS: deadline kills and joins an owned child ignoring TERM'

# The Python fixture is test-only: real OpenSSL clients, owned loopback TLS
# endpoints and generated ephemeral CAs, with bounded joins on every path.
python3 "$repo_root/extras/test/lib/imap_tls_fixture.py" "$repo_root" "$tmp_root"
