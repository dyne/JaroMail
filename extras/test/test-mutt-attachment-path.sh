#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

capture_file="${tmp_root}/mutt-env"
export MUTT_ENV_CAPTURE="${capture_file}"

cat > "${test_bin}/mutt" <<'EOF'
#!/usr/bin/env sh
if [ "$1" = "-v" ]; then
  printf '%s\n' 'Mutt 2.2 test build'
  exit 0
fi
printf '%s\n' "${JAROMAILDIR:-}" > "${MUTT_ENV_CAPTURE}"
EOF
chmod +x "${test_bin}/mutt"

print -- 'application/pdf true' > "${mail_root}/Applications.txt"
jaro_source open >/dev/null

assert_equal "$(cat "${capture_file}")" "${mail_root}" "mutt inherits active mail root"

muttrc_file="${mail_root}/.mutt/rc"
mailcap_file="${mail_root}/.mutt/mailcap"
assert_contains "$(cat "${muttrc_file}")" "setenv JAROMAILDIR \"${mail_root}\"" "mutt rc exports active mail root"
assert_contains "$(cat "${mailcap_file}")" "a=\"\${JAROMAILDIR:-${mail_root}}/tmp\" && mkdir -p \"\$a\"" "runtime attachment tmp path"

attachment="${tmp_root}/invoice.pdf"
print -- 'invoice' > "${attachment}"
mailcap_line="$(awk '/^application\/pdf;/ { print; exit }' "${mailcap_file}")"
mailcap_command="${mailcap_line#*; }"
mailcap_command="${mailcap_command//\%s/${attachment}}"
env -u JAROMAILDIR sh -c "${mailcap_command}"

assert_equal "$(cat "${mail_root}/tmp/invoice.pdf")" "invoice" "mailcap uses active mail root fallback"
