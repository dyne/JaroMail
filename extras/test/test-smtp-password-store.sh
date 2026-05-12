#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

state_dir="${tmp_root}/smtp-state"
pass_store="${tmp_root}/pass-store"
mkdir -p "${state_dir}" "${pass_store}"
print -- "test-gpg-id" > "${pass_store}/.gpg-id"

cat > "${test_bin}/pass" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
store="${PASSWORD_STORE_DIR:?}"
cmd="${1:-ls}"
shift || true
case "${cmd}" in
  show)
    key="${1:?}"
    file="${store}/${key}.gpg"
    [[ -r "${file}" ]] || exit 1
    cat "${file}"
    ;;
  insert)
    key="${@: -1}"
    file="${store}/${key}.gpg"
    mkdir -p "${file:h}"
    cat > "${file}"
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "${test_bin}/pass"

cat > "${test_bin}/pinentry" <<'EOF'
#!/usr/bin/env sh
cat >/dev/null
printf 'D smtp-secret\nOK\n'
EOF
chmod +x "${test_bin}/pinentry"

cat > "${test_bin}/gpg" <<'EOF'
#!/usr/bin/env sh
exit 0
EOF
chmod +x "${test_bin}/gpg"

cat > "${test_bin}/openssl" <<'EOF'
#!/usr/bin/env sh
if [ "$1" = "s_client" ]; then
  cat >/dev/null
  printf '%s\n' '-----BEGIN CERTIFICATE-----' 'test' '-----END CERTIFICATE-----'
elif [ "$1" = "x509" ]; then
  cat >/dev/null
  printf '%s\n' 'MD5 Fingerprint=AA:BB:CC'
else
  exit 1
fi
EOF
chmod +x "${test_bin}/openssl"

cat > "${test_bin}/msmtp" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
cfg=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -C)
      cfg="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done
cp "${cfg}" "${SMTP_CAPTURE_DIR}/msmtp.conf"
cat > "${SMTP_CAPTURE_DIR}/message.eml"
EOF
chmod +x "${test_bin}/msmtp"

jaro_source init >/dev/null
cat > "${mail_root}/Accounts/default" <<'EOF'
name person
email person@example.org
login person@example.org
proto imap
imap mail.example.org
imap_port 993
smtp smtp.example.org
smtp_port 587
auth plain
cert check
transport TLS1
options keep
EOF

cat > "${test_bin}/pass" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
store="${PASSWORD_STORE_DIR:?}"
cmd="${1:-ls}"
shift || true
case "${cmd}" in
  show)
    [[ -n "${GPG_TTY:-}" ]] || exit 1
    print -- "${GPG_TTY}" > "${PASS_GPG_TTY_FILE}"
    print -- "smtp-secret"
    ;;
  insert)
    key="${@: -1}"
    file="${store}/${key}.gpg"
    mkdir -p "${file:h}"
    cat > "${file}"
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "${test_bin}/pass"

fake_tty="${state_dir}/fake-tty"
gpg_tty_seen="${state_dir}/gpg-tty-seen"
touch "${fake_tty}"
cat > "${state_dir}/stdin-message" <<'EOF'
stdin is not a tty
EOF

GPG_TTY="${fake_tty}" \
PASS_GPG_TTY_FILE="${gpg_tty_seen}" \
JARO_KEYRING=pass \
PASSWORD_STORE_DIR="${pass_store}" \
jaro_source askpass < "${state_dir}/stdin-message" >/dev/null

assert_equal "$(<"${gpg_tty_seen}")" "${fake_tty}" "pass gets gpg tty when stdin is redirected"

cat > "${test_bin}/pass" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
store="${PASSWORD_STORE_DIR:?}"
cmd="${1:-ls}"
shift || true
case "${cmd}" in
  show)
    key="${1:?}"
    file="${store}/${key}.gpg"
    [[ -r "${file}" ]] || exit 1
    cat "${file}"
    ;;
  insert)
    key="${@: -1}"
    file="${store}/${key}.gpg"
    mkdir -p "${file:h}"
    cat > "${file}"
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "${test_bin}/pass"

SMTP_CAPTURE_DIR="${state_dir}" \
JARO_KEYRING=pass \
PASSWORD_STORE_DIR="${pass_store}" \
jaro_source smtp <<'EOF'
From: Person <person@example.org>
To: Recipient <recipient@example.org>
Subject: SMTP password store test

Body
EOF

assert_equal "$(<"${pass_store}/email/person@example.org.gpg")" "smtp-secret" "smtp password saved in pass"
assert_contains "$(<"${state_dir}/msmtp.conf")" "password smtp-secret" "msmtp uses stored password"

cat > "${test_bin}/pass" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
store="${PASSWORD_STORE_DIR:?}"
cmd="${1:-ls}"
shift || true
case "${cmd}" in
  show)
    print -u2 -- "gpg: public key decryption failed: No such file or directory"
    print -u2 -- "gpg: decryption failed: No such file or directory"
    exit 1
    ;;
  insert)
    key="${@: -1}"
    file="${store}/${key}.gpg"
    mkdir -p "${file:h}"
    cat > "${file}"
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "${test_bin}/pass"

cat > "${test_bin}/pinentry" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
cat >/dev/null
count=0
[[ -r "${PINENTRY_COUNT_FILE}" ]] && count="$(<"${PINENTRY_COUNT_FILE}")"
print -- "$((count + 1))" > "${PINENTRY_COUNT_FILE}"
printf 'D smtp-secret\nOK\n'
EOF
chmod +x "${test_bin}/pinentry"

cat > "${test_bin}/msmtp" <<'EOF'
#!/usr/bin/env zsh
set -euo pipefail
cfg=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -C)
      cfg="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done
count=0
[[ -r "${SMTP_CAPTURE_DIR}/msmtp-count" ]] && count="$(<"${SMTP_CAPTURE_DIR}/msmtp-count")"
count=$((count + 1))
print -- "${count}" > "${SMTP_CAPTURE_DIR}/msmtp-count"
cp "${cfg}" "${SMTP_CAPTURE_DIR}/msmtp-${count}.conf"
cat > "${SMTP_CAPTURE_DIR}/message-${count}.eml"
EOF
chmod +x "${test_bin}/msmtp"

rm -rf "${mail_root}/outbox" "${mail_root}/sent" "${state_dir}/msmtp-count"
mkdir -p "${mail_root}/outbox/new" "${mail_root}/outbox/cur" "${mail_root}/outbox/tmp"
pinentry_count="${state_dir}/pinentry-count"
rm -f "${pinentry_count}"

cat > "${mail_root}/outbox/new/msg1" <<'EOF'
From: Person <person@example.org>
To: One <one@example.org>
Subject: first queued message

Body
EOF

cat > "${mail_root}/outbox/new/msg2" <<'EOF'
From: Person <person@example.org>
To: Two <two@example.org>
Subject: second queued message

Body
EOF

SMTP_CAPTURE_DIR="${state_dir}" \
PINENTRY_COUNT_FILE="${pinentry_count}" \
JARO_KEYRING=pass \
PASSWORD_STORE_DIR="${pass_store}" \
jaro_source send >/dev/null

assert_equal "$(<"${pinentry_count}")" "1" "batch send asks smtp password once"
assert_equal "$(<"${state_dir}/msmtp-count")" "2" "batch send delivers both queued messages"
assert_contains "$(<"${state_dir}/msmtp-2.conf")" "password smtp-secret" "second queued send reuses cached password"
