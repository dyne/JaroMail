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
