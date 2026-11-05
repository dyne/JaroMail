#!/usr/bin/env zsh
# Execute actual legacy functions with their original logging and lock helpers.
# No replacement of fn/act/newlock/addressbook functions is allowed here.
setopt NO_RCS
bench_repo=$1
bench_stage=$2
bench_clock=$3
PROGRAM=jaro-filter-benchmark VERSION=6.0
source "$bench_repo/src/zlibs/bootstrap" source || exit $?
read_account || exit $?
mblaze_setup || exit $?
zmodload zsh/datetime
bench_start=$EPOCHREALTIME
case $bench_stage in
  update_filters) update_filters; bench_rc=$? ;;
  emission) update_sieve; bench_rc=$? ;;
  *) print -u2 -- "Unknown stage: $bench_stage"; exit 2 ;;
esac
printf '%.9f\n' "$(( EPOCHREALTIME - bench_start ))" > "$bench_clock"
exit $bench_rc
