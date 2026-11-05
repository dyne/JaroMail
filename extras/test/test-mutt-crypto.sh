#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

if command -v mutt >/dev/null 2>&1; then
  mutt_bin=mutt
elif command -v neomutt >/dev/null 2>&1; then
  mutt_bin=neomutt
else
  print -- "SKIP test-mutt-crypto.sh: missing mutt or neomutt"
  exit 77
fi

muttrc="${tmp_root}/muttrc"
cat > "${muttrc}" <<EOF
source "${work_root}/mutt/crypto"
EOF

set +e
crypto_settings="$("${mutt_bin}" -n -F "${muttrc}" \
  -Q crypt_use_gpgme \
  -Q crypt_autosmime \
  -Q smime_verify_command \
  -Q smime_verify_opaque_command </dev/null 2>"${tmp_root}/query.err")"
query_status=$?
set -e
[[ ! -s "${tmp_root}/query.err" ]]

if [[ "$mutt_bin" == neomutt ]]; then
  # NeoMutt uses assignment syntax and returns 1 when querying a false boolean.
  (( query_status == 0 || query_status == 1 ))
  assert_contains "${crypto_settings}" 'crypt_use_gpgme = no' "disable gpgme"
  crypto_settings="${crypto_settings//crypt_auto_smime/crypt_autosmime}"
  assert_contains "${crypto_settings}" 'crypt_autosmime = no' "disable automatic smime"
  assert_contains "${crypto_settings}" 'smime_verify_command = "/bin/false"' "fail fast smime verify"
  assert_contains "${crypto_settings}" 'smime_verify_opaque_command = "/bin/false"' "fail fast opaque smime verify"
else
  assert_equal "$query_status" 0 "mutt configuration query"
  assert_contains "${crypto_settings}" "crypt_use_gpgme is unset" "disable gpgme"
  assert_contains "${crypto_settings}" "crypt_autosmime is unset" "disable automatic smime"
  assert_contains "${crypto_settings}" 'smime_verify_command="/bin/false"' "fail fast smime verify"
  assert_contains "${crypto_settings}" 'smime_verify_opaque_command="/bin/false"' "fail fast opaque smime verify"
fi
