# Lightweight module fixtures: no installed runtime or network tools required.
source "${${(%):-%x}:A:h}/test_helpers.zsh"
repo_root="${${(%):-%x}:A:h:h:h:h}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/jaromail-module.XXXXXXXX")"
trap 'test_cleanup' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fn() { :; }
func() { :; }
notice() { :; }
act() { :; }
warning() { :; }
error() { print -u2 -r -- "$*"; }
ckreq() { return 0; }

assert_status() {
  local expected_status="$1" actual_status=0
  shift
  "$@" || actual_status=$?
  assert_equal "$actual_status" "$expected_status" "exit status of $1"
}
