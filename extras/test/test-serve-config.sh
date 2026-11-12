#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

# Rendering/CLI fixtures deliberately stub the external tools. The separate
# live suite validates this output with real Dovecot and authenticates over IMAP.
cat > "$test_bin/dovecot" <<'EOF'
#!/bin/sh
if [ "$1" = --version ]; then echo "${TEST_DOVECOT_VERSION:-2.4.0}"; exit 0; fi
echo launched >> "$TEST_SERVE_LAUNCHES"
exit 37
EOF
cat > "$test_bin/doveconf" <<'EOF'
#!/bin/sh
exit "${TEST_DOVECONF_EXIT:-0}"
EOF
cat > "$test_bin/lsof" <<'EOF'
#!/bin/sh
if [ "${TEST_SERVE_BUSY:-0}" = 1 ]; then
  echo 'dovecot 123 user 1u IPv4 1 0t0 TCP 127.0.0.1:61444 (LISTEN)'
fi
EOF
chmod +x "$test_bin"/{dovecot,doveconf,lsof}
export TEST_SERVE_LAUNCHES="$tmp_root/launches"

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
assert_contains "${serve_out}" "Credentials: ${passwd_path}" "serve credential file response"
assert_contains "${serve_out}" "Config: ${conf_path}" "serve config response"

conf_body="$(cat "${conf_path}")"
assert_contains "${conf_body}" "dovecot_config_version = 2.4.0" "serve config version"
assert_contains "${conf_body}" "imap = yes" "serve protocols"
assert_contains "${conf_body}" "listen = 127.0.0.1" "serve listen"
assert_contains "${conf_body}" "ssl = no" "serve ssl"
assert_contains "${conf_body}" "auth_allow_cleartext = yes" "serve plaintext auth"
assert_contains "${conf_body}" "auth_mechanisms = plain" "serve auth mechanism"
assert_contains "${conf_body}" "mail_path = \"${mail_root}\"" "serve mail location"
assert_contains "${conf_body}" "mailbox_list_layout = fs" "serve layout"
assert_contains "${conf_body}" "default_internal_user = $(id -un)" "rootless internal user"
assert_contains "${conf_body}" "default_login_user = $(id -un)" "rootless login user"
assert_contains "${conf_body}" "port = 61444" "serve selected port"

if print -- "${conf_body}" | grep -Eiq 'pop3|lmtp|submission|managesieve|sieve|/etc/dovecot'; then
  print -- "ASSERT FAIL: found forbidden service/include in config"
  exit 1
fi

passwd_body="$(cat "${passwd_path}")"
assert_equal "${passwd_body}" "jaro:{PLAIN}testpass" "serve passwd contents"

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
assert_contains "${space_conf_body}" "mail_path = \"${space_root}\"" "serve quoted mail location"
assert_contains "${space_out}" "Host: 127.0.0.1" "serve response with spaced root"

set +e
JARO_SERVE_CONFIG_ONLY=1 JARO_SERVE_USER='bad:user' jaro_source serve >/dev/null 2>&1
bad_user_ec=$?
set -e
[[ "${bad_user_ec}" -ne 0 ]] || { print -- "ASSERT FAIL: invalid user with colon accepted"; exit 1; }

# A busy listener must not rotate credentials or replace an existing config.
cp "$conf_path" "$tmp_root/conf.before"
cp "$passwd_path" "$tmp_root/passwd.before"
if TEST_SERVE_BUSY=1 JARO_SERVE_PORT=61444 JARO_SERVE_PASSWORD=replacement jaro_source serve >/dev/null 2>&1; then
  print -u2 'busy port accepted'; exit 1
fi
cmp "$conf_path" "$tmp_root/conf.before"
cmp "$passwd_path" "$tmp_root/passwd.before"
[[ ! -e "$TEST_SERVE_LAUNCHES" ]]

# Wrong versions fail before creating a runtime; invalid config is a failure,
# never a successful skip or a daemon launch.
if TEST_DOVECOT_VERSION=2.3.21 jaro_source serve >/dev/null 2>&1; then
  print -u2 'unsupported Dovecot accepted'; exit 1
fi
cmp "$passwd_path" "$tmp_root/passwd.before"
if TEST_DOVECONF_EXIT=89 JARO_SERVE_CONFIG_ONLY=1 jaro_source serve >/dev/null 2>&1; then
  print -u2 'invalid config accepted'; exit 1
fi
[[ ! -e "$TEST_SERVE_LAUNCHES" ]]

# Dry-run uses the actual CLI/bootstrap on an absent root. It must not even
# probe the unsupported daemon, and must work for source and installed wrappers.
for cli in "$repo_root/src/jaro" "$prefix_root/bin/jaro"; do
  dry_root="$tmp_root/dry-root"
  TEST_DOVECOT_VERSION=2.3.21 JAROMAILDIR="$dry_root" JAROWORKDIR="$work_root" \
    "$cli" -n serve > "$tmp_root/dry.log" 2>&1
  [[ ! -e "$dry_root" && ! -e "$TEST_SERVE_LAUNCHES" ]]
  assert_contains "$(cat "$tmp_root/dry.log")" 'Would serve' 'dry-run preview'
done

# Failed launch preserves its exit status and releases the runtime lock.
set +e
jaro_source serve > "$tmp_root/launch.log" 2>&1
launch_status=$?
set -e
assert_equal "$launch_status" 37 'daemon exit propagated'
JARO_SERVE_CONFIG_ONLY=1 jaro_source serve >/dev/null 2>&1
print -- 'PASS: serve config, rootless settings, busy port, validation and dry-run'
