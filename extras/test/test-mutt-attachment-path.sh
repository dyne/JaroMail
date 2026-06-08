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

cat > "${test_bin}/msedge.exe" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$1" > "${EXE_PATH_CAPTURE}"
EOF
chmod +x "${test_bin}/msedge.exe"

print -- 'application/octet-stream true' > "${mail_root}/Applications.txt"
cat > "${mail_root}/mailcap" <<'EOF'
application/pdf; msedge.exe %s
text/plain; less %s
EOF
mkdir -p "${mail_root}/.mutt"
print -- 'application/*; cp %s /stale/tmp' > "${mail_root}/.mutt/mailcap"
jaro_source open >/dev/null
jaro_source open >/dev/null

assert_equal "$(cat "${capture_file}")" "${mail_root}" "mutt inherits active mail root"

muttrc_file="${mail_root}/.mutt/rc"
mailcap_file="${mail_root}/.mutt/mailcap"
assert_contains "$(cat "${muttrc_file}")" "setenv JAROMAILDIR \"${mail_root}\"" "mutt rc exports active mail root"
assert_contains "$(cat "${muttrc_file}")" "set mailcap_path = \"${mail_root}/.mutt/mailcap:" "mutt uses rebuilt mailcap"
assert_contains "$(cat "${mailcap_file}")" "a=\"\${JAROMAILDIR:-${mail_root}}/Attachments\" && mkdir -p \"\$a\"" "persistent attachment path"
if grep -q '/stale/tmp' "${mailcap_file}"; then
  print -- "ASSERT FAIL (stale mailcap entry removed)"
  exit 1
fi
assert_equal "$(grep -c '^application/pdf;' "${mailcap_file}")" "1" "mailcap rebuilt once per launch"
assert_contains "$(cat "${mailcap_file}")" 'w=$(/usr/bin/wslpath -w "$p") && msedge.exe "$w"' "Windows handler converts staged attachment path"
assert_contains "$(cat "${mailcap_file}")" 'text/plain; a="${JAROMAILDIR:-' "Unix handler stages attachment"

attachment="${tmp_root}/invoice.pdf"
print -- 'invoice' > "${attachment}"
export EXE_PATH_CAPTURE="${tmp_root}/exe-path"
mailcap_line="$(awk '/^application\/pdf;/ { print; exit }' "${mailcap_file}")"
mailcap_command="${mailcap_line#*; }"
mailcap_command="${mailcap_command//\%s/${attachment}}"
sh -c "${mailcap_command}"
staged_attachment="${mail_root}/Attachments/invoice.pdf"
assert_equal "$(cat "${staged_attachment}")" "invoice" "attachment persists after opening"
assert_equal "$(cat "${EXE_PATH_CAPTURE}")" "$(/usr/bin/wslpath -w "${staged_attachment}")" "Windows handler receives staged path"

print -- 'updated invoice' > "${attachment}"
sh -c "${mailcap_command}"
assert_equal "$(cat "${staged_attachment}")" "updated invoice" "reopening overwrites attachment"

mailcap_line="$(awk '/^application\/octet-stream;/ { print; exit }' "${mailcap_file}")"
mailcap_command="${mailcap_line#*; }"
mailcap_command="${mailcap_command//\%s/${attachment}}"
env -u JAROMAILDIR sh -c "${mailcap_command}"
assert_equal "$(cat "${staged_attachment}")" "updated invoice" "fallback uses persistent attachment path"
