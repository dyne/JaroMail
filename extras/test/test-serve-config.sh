#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

zmodload zsh/stat

msg_path="$(make_maildir_message \
  "${mail_root}/known" "new" "serve-msg-1" \
  "From: Serve Test <serve@example.org>" \
  "To: Local User <local@example.org>" \
  "Subject: jaro-serve-fixture")"

serve_out="$(
  JARO_SERVE_CONFIG_ONLY=1 \
  JARO_SERVE_PASSWORD=testpass \
  JARO_SERVE_PORT=61444 \
  jaro_source serve 2>&1
)"

runtime_root="${mail_root}/.imap"
conf_path="${runtime_root}/dovecot.conf"
passwd_path="${runtime_root}/passwd"

[[ -f "${conf_path}" ]] || { print -- "ASSERT FAIL: missing ${conf_path}"; exit 1; }
[[ -f "${passwd_path}" ]] || { print -- "ASSERT FAIL: missing ${passwd_path}"; exit 1; }
[[ -f "${msg_path}" ]] || { print -- "ASSERT FAIL: fixture message moved ${msg_path}"; exit 1; }

assert_contains "${serve_out}" "Host: 127.0.0.1" "serve host response"
assert_contains "${serve_out}" "Port: 61444" "serve port response"
assert_contains "${serve_out}" "TLS: none" "serve tls response"
assert_contains "${serve_out}" "User: jaro" "serve user response"
assert_contains "${serve_out}" "Password: testpass" "serve password response"
assert_contains "${serve_out}" "Config: ${conf_path}" "serve config response"

conf_body="$(cat "${conf_path}")"
assert_contains "${conf_body}" "protocols = imap" "serve protocols"
assert_contains "${conf_body}" "listen = 127.0.0.1" "serve listen"
assert_contains "${conf_body}" "ssl = no" "serve ssl"
assert_contains "${conf_body}" "disable_plaintext_auth = no" "serve plaintext auth"
assert_contains "${conf_body}" "auth_mechanisms = plain" "serve auth mechanism"
assert_contains "${conf_body}" "mail_location = maildir:${mail_root}:LAYOUT=fs" "serve mail location"
assert_contains "${conf_body}" "port = 61444" "serve selected port"

if print -- "${conf_body}" | grep -Eiq 'pop3|lmtp|submission|managesieve|sieve|/etc/dovecot'; then
  print -- "ASSERT FAIL: found forbidden service/include in config"
  exit 1
fi

passwd_body="$(cat "${passwd_path}")"
assert_contains "${passwd_body}" "jaro:{PLAIN}testpass:" "serve passwd contents"

zstat -H conf_stat "${conf_path}"
zstat -H pass_stat "${passwd_path}"
if (( (conf_stat[mode] & 8#777) != 8#600 )); then
  print -- "ASSERT FAIL: ${conf_path} mode ${conf_stat[mode]} expected 0600"
  exit 1
fi
if (( (pass_stat[mode] & 8#777) != 8#600 )); then
  print -- "ASSERT FAIL: ${passwd_path} mode ${pass_stat[mode]} expected 0600"
  exit 1
fi

default_port_out="$(
  unset JARO_SERVE_PORT
  JARO_SERVE_CONFIG_ONLY=1 \
  JARO_SERVE_PASSWORD=testpass \
  jaro_source serve 2>&1
)"
assert_contains "${default_port_out}" "Port: 61443" "serve default port"

set +e
JARO_SERVE_CONFIG_ONLY=1 JARO_SERVE_HOST=0.0.0.0 jaro_source serve >/dev/null 2>&1
bad_host_ec=$?
set -e
[[ "${bad_host_ec}" -ne 0 ]] || { print -- "ASSERT FAIL: invalid host accepted"; exit 1; }

set +e
JARO_SERVE_CONFIG_ONLY=1 JARO_SERVE_PORT=0 jaro_source serve >/dev/null 2>&1
bad_port_ec=$?
set -e
[[ "${bad_port_ec}" -ne 0 ]] || { print -- "ASSERT FAIL: invalid port accepted"; exit 1; }

set +e
JARO_SERVE_CONFIG_ONLY=1 JARO_SERVE_MAX_CONNECTIONS=100 jaro_source serve >/dev/null 2>&1
bad_max_ec=$?
set -e
[[ "${bad_max_ec}" -ne 0 ]] || { print -- "ASSERT FAIL: invalid max connections accepted"; exit 1; }

space_root="${tmp_root}/mail root with spaces"
space_out="$(
  JAROMAILDIR="${space_root}" \
  JAROWORKDIR="${work_root}" \
  JARO_SERVE_CONFIG_ONLY=1 \
  JARO_SERVE_PASSWORD=testpass \
  "${repo_root}/src/jaro" serve 2>&1
)"
space_conf="${space_root}/.imap/dovecot.conf"
[[ -f "${space_conf}" ]] || { print -- "ASSERT FAIL: missing ${space_conf}"; exit 1; }
space_conf_body="$(cat "${space_conf}")"
escaped_space_root="${space_root// /\\ }"
assert_contains "${space_conf_body}" "mail_location = maildir:${escaped_space_root}:LAYOUT=fs" "serve escaped mail location"
assert_contains "${space_out}" "Host: 127.0.0.1" "serve response with spaced root"

set +e
JARO_SERVE_CONFIG_ONLY=1 JARO_SERVE_USER='bad:user' jaro_source serve >/dev/null 2>&1
bad_user_ec=$?
set -e
[[ "${bad_user_ec}" -ne 0 ]] || { print -- "ASSERT FAIL: invalid user with colon accepted"; exit 1; }
