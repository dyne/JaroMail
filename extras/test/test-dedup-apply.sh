#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
for target in jaro-dedup-test jaro-dedup; do
    if ! python3 "$repo_root/build/cmake-build.py" --output-dir "$repo_root/build/gnu" --testing --target "$target" > "$tmp_root/build.log" 2>&1; then
        cat "$tmp_root/build.log"
        exit 1
    fi
done
python3 "$repo_root/extras/test/dedup_apply_test.py" || exit $?
python3 "$repo_root/extras/test/dedup-cli-smoke.py" --cli "$repo_root/src/jaro" || exit $?

# Exercise the old handler with a disposable lastlog list and formail double.
source "$repo_root/src/zlibs/maildirs"
MAILDIRS="$tmp_root/legacy"
mkdir -p "$MAILDIRS"/{Primary,Archive}/{cur,new,tmp} || exit 1
print -r -- $'Message-ID: <same>\n\nfirst' > "$MAILDIRS/Primary/cur/first"
print -r -- $'Message-ID: <same>\n\ndifferent' > "$MAILDIRS/Archive/new/later"
ztmp() { ztmpfile="$tmp_root/ids"; : > "$ztmpfile"; }
maildirs_lastlog() { print -r -- 'Primary Archive'; }
formail() {
    local header
    [[ $1 == -D && $2 == 128000000 && $3 == "$tmp_root/ids" ]] || return 2
    IFS= read -r header || return 2
    grep -Fxq -- "$header" "$3" && return 0
    print -r -- "$header" >> "$3"
    return 1
}
find=find
option_params=(lastlog)
cmd_rmdupes
[[ -f "$MAILDIRS/Primary/cur/first" && ! -e "$MAILDIRS/Archive/new/later" ]] || exit 1
print -r -- 'PASS legacy rmdupes lastlog delegates to the Message-ID cache'
