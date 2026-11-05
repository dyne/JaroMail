#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
source "$repo_root/src/zlibs/maildirs"

# Exercise the native fallback even on hosts with mblaze installed.
command() {
  [[ "$1" == -v && "$2" == (mrefile|mdeliver|mmkdir) ]] && return 1
  builtin command "$@"
}
mkdir="mkdir -p"
MAILDIRS="$tmp_root/mail with spaces"
maildirmake "$MAILDIRS/archive"
mkdir -p "$tmp_root/source"
print -r -- original > "$MAILDIRS/archive/cur/same:2,S"
print -r -- incoming > "$tmp_root/source/same:2,S"

maildir_refile "$tmp_root/source/same:2,S" "$MAILDIRS/archive" copy
assert_equal "$(cat "$MAILDIRS/archive/cur/same:2,S")" original "existing message preserved"
assert_equal "$(cat "$last_deliver")" incoming "copied message preserved"
[[ "$last_deliver" == *:2,S && -f "$tmp_root/source/same:2,S" ]]
maildir_refile "$tmp_root/source/same:2,S" "$MAILDIRS/archive"
[[ ! -e "$tmp_root/source/same:2,S" ]]
assert_equal "$(find "$MAILDIRS/archive/cur" -type f | wc -l | tr -d ' ')" 3 "all messages retained"

print -r -- delivered | deliver archive
assert_equal "$(cat "$last_deliver")" delivered "fallback delivery"
[[ "${last_deliver:h:t}" == new ]]

# A partial read must never become visible in new/ or remove a source.
cat() { print -r -- partial; return 1; }
before=("$MAILDIRS/archive/new/"*(N))
if print ignored | deliver archive; then
  print -u2 -- "failed input was accepted"; exit 1
fi
after=("$MAILDIRS/archive/new/"*(N))
assert_equal "${#after}" "${#before}" "failed delivery not published"
print -r -- source > "$tmp_root/source/keep"
if maildir_refile "$tmp_root/source/keep" "$MAILDIRS/archive"; then
  print -u2 -- "failed copy was accepted"; exit 1
fi
[[ -f "$tmp_root/source/keep" ]]
staged=("$MAILDIRS/archive/tmp/"*(N))
assert_equal "${#staged}" 0 "staging files cleaned"
unfunction cat

# Force a publication collision, including a pre-existing dangling symlink.
collision="$MAILDIRS/archive/new/collision"
ln -s "$tmp_root/missing" "$collision"
mktemp() { touch "$MAILDIRS/archive/tmp/collision"; print -r -- "$MAILDIRS/archive/tmp/collision"; }
if print ignored | deliver archive; then
  print -u2 -- "publication collision was accepted"; exit 1
fi
[[ -L "$collision" && ! -e "$tmp_root/missing" ]]
print -- "PASS: collision-safe refile and atomic delivery"
