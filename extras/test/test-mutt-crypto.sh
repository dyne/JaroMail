#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

command -v mutt >/dev/null 2>&1 || {
  print -- "SKIP test-mutt-crypto.sh: missing mutt"
  exit 0
}

muttrc="${tmp_root}/muttrc"
cat > "${muttrc}" <<EOF
source "${work_root}/mutt/crypto"
EOF

crypto_settings="$(mutt -F "${muttrc}" \
  -Q crypt_use_gpgme \
  -Q crypt_autosmime \
  -Q smime_verify_command \
  -Q smime_verify_opaque_command </dev/null)"

assert_contains "${crypto_settings}" "crypt_use_gpgme is unset" "disable gpgme"
assert_contains "${crypto_settings}" "crypt_autosmime is unset" "disable automatic smime"
assert_contains "${crypto_settings}" 'smime_verify_command="/bin/false"' "fail fast smime verify"
assert_contains "${crypto_settings}" 'smime_verify_opaque_command="/bin/false"' "fail fast opaque smime verify"
