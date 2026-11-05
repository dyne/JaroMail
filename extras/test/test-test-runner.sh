#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
runner="$repo_root/extras/test/run-tests.sh"
printf '%s\n' '#!/bin/sh' 'echo executed >> "$RUNNER_MARKER"' > "$tmp_root/pass.sh"
printf '%s\n' '#!/bin/sh' 'echo "SKIP fixture: missing optional tool"' 'exit 77' > "$tmp_root/skip.sh"
printf '%s\n' '#!/bin/sh' 'exit 9' > "$tmp_root/fail.sh"
printf '%s\n' '#!/bin/sh' 'echo "SKIP hidden failure"' 'exit 0' > "$tmp_root/hidden.sh"
export RUNNER_MARKER="$tmp_root/executed" JARO_TEST_REQUIRE_ALL=0
output="$(zsh "$runner" "$tmp_root/pass.sh" "$tmp_root/skip.sh")"
assert_contains "$output" 'Tests: 1 passed, 0 failed, 1 skipped' 'runner counts real skips'
export JARO_TEST_REQUIRE_ALL=1
assert_status 1 zsh "$runner" "$tmp_root/skip.sh" > "$tmp_root/strict.log" 2>&1
export JARO_TEST_REQUIRE_ALL=0
assert_status 1 zsh "$runner" "$tmp_root/fail.sh" "$tmp_root/pass.sh" > "$tmp_root/fail.log" 2>&1
assert_equal "$(wc -l < "$RUNNER_MARKER" | tr -d ' ')" 2 'runner continues after failures'
assert_status 1 zsh "$runner" "$tmp_root/hidden.sh" > "$tmp_root/hidden.log" 2>&1
print -- 'PASS: runner counts, strict prerequisites and failure propagation'
