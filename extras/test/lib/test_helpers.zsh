#!/usr/bin/env zsh

set -euo pipefail

export LC_ALL=C
export LANG=C

test_setup() {
  script_path="${1}"
  repo_root="${script_path:A:h:h:h}"
  tmp_root="${TMPDIR:-/tmp}/jaromail-source-test.$$"
  mail_root="${tmp_root}/mail"
  prefix_root="${tmp_root}/prefix"
  work_root="${prefix_root}/share/jaromail"
  test_bin="${tmp_root}/bin"

  mkdir -p "${mail_root}"
  mkdir -p "${test_bin}"
  cd "${repo_root}"
  make >/dev/null
  PREFIX="${prefix_root}" make install >/dev/null
cat > "${test_bin}/mdeliver" <<'EOF'
#!/usr/bin/env sh
verbose=0
if [ "$1" = "-v" ]; then
  verbose=1
  shift
fi
dest="$1"
[ -n "$dest" ] || exit 1
mkdir -p "$dest/new" "$dest/cur" "$dest/tmp"
base="$(date +%s).M$$P$RANDOMQ1.test"
path="$dest/new/$base"
cat > "$path"
[ $verbose -eq 1 ] && printf '%s\n' "$path"
exit 0
EOF
  chmod +x "${test_bin}/mdeliver"

  export PATH="${test_bin}:${repo_root}/src:${repo_root}/build/gnu:${PATH}"
  rehash
}

test_cleanup() {
  rm -rf "${tmp_root}"
}

jaro_source() {
  JAROMAILDIR="${mail_root}" \
  JAROWORKDIR="${work_root}" \
  "${repo_root}/src/jaro" "$@"
}

lorem_message() {
cat <<'EOF'
Lorem ipsum dolor sit amet

Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do
eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad
minim veniam, quis nostrud exercitation ullamco laboris nisi ut
aliquip ex ea commodo consequat. Duis aute irure dolor in
reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla
pariatur. Excepteur sint occaecat cupidatat non proident, sunt in
culpa qui officia deserunt mollit anim id est laborum.
EOF
}

assert_equal() {
  got="${1}"
  expected="${2}"
  label="${3:-assert_equal}"
  if [[ "${got}" != "${expected}" ]]; then
    print -- "ASSERT FAIL (${label})"
    print -- "--- got ---"
    print -- "${got}"
    print -- "--- expected ---"
    print -- "${expected}"
    return 1
  fi
}

assert_success() {
  label="${1:-assert_success}"
  if [[ $? -ne 0 ]]; then
    print -- "ASSERT FAIL (${label})"
    return 1
  fi
}

assert_contains() {
  haystack="${1}"
  needle="${2}"
  label="${3:-assert_contains}"
  if [[ "${haystack}" != *"${needle}"* ]]; then
    print -- "ASSERT FAIL (${label}): missing '${needle}'"
    return 1
  fi
}
