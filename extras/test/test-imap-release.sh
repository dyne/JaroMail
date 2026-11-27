#!/usr/bin/env zsh
set -euo pipefail
umask 077
source "${0:A:h}/lib/test_helpers.zsh"
mode=${1:-all}
case $mode in
  all|--scale|--dovecot) ;;
  *) print -u2 'Usage: test-imap-release.sh [--scale|--dovecot]'; exit 2 ;;
esac
for dependency in python3 tar gzip; do
  command -v "$dependency" >/dev/null || { print "SKIP missing $dependency"; exit 77; }
done
if [[ $mode != --scale ]]; then
  if [[ -n ${SIEVE_DEPS_PREFIX:-} ]]; then
    export PATH="$SIEVE_DEPS_PREFIX/sbin:$SIEVE_DEPS_PREFIX/bin:$PATH"
  fi
  if ! command -v doveconf >/dev/null || ! command -v dovecot >/dev/null || [[ $(dovecot --version) != 2.4.<->* ]]; then
    print 'SKIP Dovecot 2.4.x with doveconf required'
    exit 77
  fi
fi
socket_parent=${JARO_SERVE_TEST_TMPDIR:-${TMPDIR:-/tmp}}
TMPDIR=${JARO_TEST_LOG_DIR:-${TMPDIR:-/tmp}} test_setup "$0"
trap test_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export HOME="$tmp_root/home" JAROMAILDIR="$mail_root" JARO_KEYRING=fixture
mkdir -m 700 "$HOME"
unset JARO_IMAP_PYTHON JAROWORKDIR JARO_IMAP_TIMEOUT
entry="$prefix_root/bin/jaro"
"$entry" -q init > "$tmp_root/init.log" 2>&1
evidence=$(mktemp -d "${JARO_TEST_LOG_DIR:-$tmp_root}/imap-release.XXXXXXXX")
export JARO_TEST_LOG_DIR="$evidence"
if [[ $mode != --scale ]]; then
  python3 "$repo_root/extras/test/lib/imap_real_cases.py" "$entry" "$tmp_root" "$socket_parent"
fi
if [[ $mode != --dovecot ]]; then
  python3 "$repo_root/extras/bench/imap_scale.py" "$entry" "$tmp_root"
fi
