#!/usr/bin/env zsh
# Snapshot before the normal build/stage helpers can regenerate tracked docs.
set -euo pipefail
bench_repo="${0:A:h:h:h}"
bench_snapshot="$(mktemp -d "${TMPDIR:-/tmp}/jaro-filter-benchmark-source.XXXXXXXX")"
cp -a "$bench_repo"/{src,build,extras,doc,Makefile,.github,README.md,vendor} "$bench_snapshot/"
source "$bench_snapshot/extras/test/lib/test_helpers.zsh"
test_setup "$bench_snapshot/extras/test/test-filter-benchmark.sh"
trap test_cleanup EXIT
python3 "$bench_snapshot/extras/bench/filter_baseline.py" \
  --repo "$repo_root" --work "$work_root" --scratch "$tmp_root/benchmark" "$@"
