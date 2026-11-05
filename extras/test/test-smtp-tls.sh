#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/email"
account=fixture smtp_keep_password=0
read_account() {
  email=sender@example.org login=sender@example.org smtp=fixture.invalid
  smtp_port=587 auth=plain cert=ignore password=fixture-secret
}
ztmp() { ztmpfile="$(mktemp "$tmp_root/smtp.XXXXXXXX")"; }
openssl() { print -u2 -- 'unexpected separate TLS probe'; return 99; }
msmtp() {
  [[ "$1" == -C && "$3" == -t ]]
  cp "$2" "$tmp_root/captured.conf"
  cat > "$tmp_root/message"
  return $smtp_fixture_status
}

smtp_fixture_status=0
print message | smtp_send
config="$(cat "$tmp_root/captured.conf")"
assert_contains "$config" 'tls_certcheck on' "certificate checks enabled even with cert ignore"
assert_contains "$config" 'tls_trust_file "system"' "system trust"
assert_equal "$(cat "$tmp_root/message")" message "message forwarded"
[[ ! -e "$ztmpfile" && -z "${password-}" ]]

# Certificate rejection is a send failure, with no unverified retry.
smtp_fixture_status=69
assert_status 69 smtp_send < "$tmp_root/message"
[[ ! -e "$ztmpfile" && -z "${password-}" ]]

certfile="$tmp_root/private-ca.pem"
touch "$certfile"
smtp_fixture_status=0
smtp_send < "$tmp_root/message"
assert_contains "$(cat "$tmp_root/captured.conf")" "tls_trust_file \"$certfile\"" "private CA trust"
certfile="$tmp_root/missing-ca.pem"
assert_status 1 smtp_send < "$tmp_root/message"
print -- 'PASS: verified SMTP configuration, private CAs and TLS failure propagation'
