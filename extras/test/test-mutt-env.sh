#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

cat > "${test_bin}/mutt" <<'EOF'
#!/usr/bin/env sh
case "$1" in
  -v)
    printf '%s\n' 'Mutt 2.2 test build'
    ;;
esac
exit 0
EOF
chmod +x "${test_bin}/mutt"

custom_bin="${tmp_root}/custom-bin"
mkdir -p "${custom_bin}"
export PATH="${custom_bin}:${PATH}"

jaro_source init >/dev/null
jaro_source open >/dev/null

path_line="$(awk '/^setenv PATH / { print; exit }' "${mail_root}/.mutt/rc")"
assert_contains "${path_line}" "setenv PATH \"" "mutt rc exports PATH"
assert_contains "${path_line}" "${custom_bin}" "mutt rc preserves inherited custom path"
