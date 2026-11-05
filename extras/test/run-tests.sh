#!/usr/bin/env zsh

set -uo pipefail
test_dir="${0:A:h}"
if (( $# )); then
  tests=("$@")
else
  tests=("$test_dir/run-source.sh" "$test_dir"/test-*.sh(N))
fi
logs="$(mktemp -d "${TMPDIR:-/tmp}/jaromail-test-logs.XXXXXXXX")" || exit 1
trap 'rm -rf -- "$logs"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
passed=0 failed=0 skipped=0
for test_script in "${tests[@]}"; do
  log_file="$logs/${test_script:t}.log"
  zsh "$test_script" > "$log_file" 2>&1
  test_status=$?
  case "$test_status" in
    0)
      if grep -q '^SKIP ' "$log_file"; then
        print -- "FAIL ${test_script:t}: skip reported without exit 77"
        cat "$log_file"
        (( failed += 1 ))
      else
        print -- "PASS ${test_script:t}"
        (( passed += 1 ))
      fi
      ;;
    77)
      print -- "SKIP ${test_script:t}"
      cat "$log_file"
      (( skipped += 1 ))
      ;;
    *)
      print -- "FAIL ${test_script:t} (exit $test_status)"
      cat "$log_file"
      (( failed += 1 ))
      ;;
  esac
done
print -- "Tests: $passed passed, $failed failed, $skipped skipped"
(( failed == 0 )) || exit 1
if [[ "${JARO_TEST_REQUIRE_ALL:-0}" == 1 ]] && (( skipped > 0 )); then
  print -u2 -- 'All tests are required; install the missing dependencies.'
  exit 1
fi
