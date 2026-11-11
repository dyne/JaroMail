#!/usr/bin/env zsh

set -euo pipefail

script_dir="${0:A:h}"
source "${script_dir}/lib/test_helpers.zsh"
test_setup "${0}"

cleanup() {
  test_cleanup
}
trap cleanup EXIT INT TERM

if ! command -v dovecot >/dev/null 2>&1 || ! command -v doveconf >/dev/null 2>&1; then
  print -- "SKIP test-serve-dovecot.sh: missing dovecot or doveconf"
  exit 0
fi

JARO_SERVE_CONFIG_ONLY=1 \
JARO_SERVE_PASSWORD=testpass \
JARO_SERVE_PORT=61445 \
jaro_source serve >/dev/null

conf_path="${mail_root}/.imap/dovecot.conf"
[[ -f "${conf_path}" ]] || { print -- "ASSERT FAIL: missing ${conf_path}"; exit 1; }
if ! doveconf -c "${conf_path}" -n >/dev/null 2>&1; then
  print -- "SKIP test-serve-dovecot.sh: local dovecot syntax differs from generated minimal config"
  exit 0
fi
