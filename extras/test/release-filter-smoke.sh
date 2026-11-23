#!/usr/bin/env bash
set -euo pipefail
bundle=${1:?expected the extracted release directory}
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
export JAROMAILDIR="$fixture/mail with spaces"
mkdir -p "$JAROMAILDIR"/incoming/{cur,new,tmp}
printf 'from-address fixture@example.org move Archive\n' > "$JAROMAILDIR/Filters.txt"
printf 'From: fixture@example.org\nSubject: release check\n\nbody\n' > "$JAROMAILDIR/incoming/new/test"
"$bundle/share/jaromail/bin/jaro-filter-compile" --root "$JAROMAILDIR"
for cmd in filter fetch; do
  "$bundle/bin/jaro" -n "$cmd" > "$fixture/report"
  grep -F Archive "$fixture/report" | grep -F '[classified]'
  test -f "$JAROMAILDIR/incoming/new/test"
  test ! -e "$JAROMAILDIR/Archive"
done
