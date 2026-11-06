#!/usr/bin/env zsh
source "${0:A:h}/lib/module_helpers.zsh"
checkout="$tmp_root/checkout with spaces"
mkdir -p "$checkout"
cp -R "$repo_root/src" "$repo_root/doc" "$repo_root/Makefile" "$checkout/"
mkdir -p "$checkout/build"
cp "$repo_root/build/"*(.N) "$checkout/build/"
cd "$checkout"
man_before="$(sha256sum doc/jaro.1 doc/jaromail.1)"
# Application build/install uses committed manuals, with no Pandoc or Sieve tools.
unset SIEVE_DEPS_PREFIX SIEVE_TOOLS_OUTPUT
mkdir -p "$tmp_root/no-sieve-tools"
for tool in sievec sieve-test doveconf dovecot pandoc; do
  printf '%s\n' '#!/bin/sh' 'echo unexpected-Sieve-runtime >&2' 'exit 90' > "$tmp_root/no-sieve-tools/$tool"
  chmod +x "$tmp_root/no-sieve-tools/$tool"
done
export PATH="$tmp_root/no-sieve-tools:$PATH"
make clean >/dev/null
export JARO_BUILD_LOG="$tmp_root/compiler.log" JARO_CACHE_LOG="$tmp_root/cache.log"
export REAL_CC="$(command -v cc)"
cat > "$tmp_root/compiler" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$JARO_BUILD_LOG"
case "${COMPILER_MODE:-ok}" in
  fail) exit 77 ;;
  link-fail) case " $* " in *' -c '*) ;; *) exit 78 ;; esac ;;
esac
exec "$REAL_CC" "$@"
EOF
cat > "$tmp_root/cache" <<'EOF'
#!/bin/sh
case " $* " in *' -c '*) ;; *) exit 79 ;; esac
printf '%s\n' "$*" >> "$JARO_CACHE_LOG"
exec "$@"
EOF
chmod +x "$tmp_root/compiler" "$tmp_root/cache"
export CC="$tmp_root/compiler" CCACHE="$tmp_root/cache"
export CFLAGS='-O0 -DJARO_CFLAGS_CHECK=1' CPPFLAGS='-DJARO_CPPFLAGS_CHECK=1'
export LDFLAGS='-Wl,--as-needed' LDLIBS='-lm'
make > "$tmp_root/build.log" 2>&1
assert_equal "$(sha256sum doc/jaro.1 doc/jaromail.1)" "$man_before" "build preserves committed manuals"
for helper in fetchaddr parsedate dotlock gpgewrap jaro-filter jaro-filter-compile jaro-filter-legacy; do
  [[ -x "build/gnu/$helper" ]]
done
compiler_log="$(cat "$JARO_BUILD_LOG")"
for flag in -DJARO_CFLAGS_CHECK=1 -DJARO_CPPFLAGS_CHECK=1 -Wl,--as-needed -lm; do
  assert_contains "$compiler_log" "$flag" "caller build flags"
done
[[ -s "$JARO_CACHE_LOG" ]]

make clean >/dev/null
[[ ! -e build/gnu/fetchaddr ]]
export COMPILER_MODE=fail
assert_status 77 ./build/build-gnu.sh > "$tmp_root/failed-build.log" 2>&1
if grep -q 'Done building' "$tmp_root/failed-build.log"; then exit 1; fi
assert_status 2 make > "$tmp_root/failed-make.log" 2>&1
prefix="$tmp_root/prefix with spaces"
assert_status 2 make install "PREFIX=$prefix" > "$tmp_root/failed-install.log" 2>&1
[[ ! -e "$prefix/bin/jaro" && ! -e build/gnu/fetchaddr ]]
export COMPILER_MODE=link-fail
assert_status 78 ./build/build-gnu.sh > "$tmp_root/failed-link.log" 2>&1
unset COMPILER_MODE

# A clean staged installation builds helpers and preserves argument boundaries.
make -j2 install "PREFIX=$prefix" > "$tmp_root/install.log" 2>&1
assert_equal "$(sha256sum doc/jaro.1 doc/jaromail.1)" "$man_before" "install preserves committed manuals"
cmp doc/jaro.1 "$prefix/share/man/man1/jaro.1"
cmp doc/jaromail.1 "$prefix/share/man/man1/jaromail.1"
[[ -x "$prefix/share/jaromail/bin/dotlock" ]]
[[ -x "$prefix/share/jaromail/bin/jaro-filter" ]]
[[ ! -e build/sieve-tools ]]
cat > "$prefix/share/jaromail/bin/jaro" <<'EOF'
#!/usr/bin/env zsh
print -r -- "$#"
printf '<%s>\n' "$@"
EOF
out="$("$prefix/bin/jaro" 'two words' '*' '')"
assert_equal "$out" $'3\n<two words>\n<*>\n<>' "installed wrapper arguments"

assert_status 2 ./build/build-gnu.sh unknown > "$tmp_root/unknown.log" 2>&1
assert_status 2 make man > "$tmp_root/explicit-man.log" 2>&1
assert_contains "$(cat "$tmp_root/explicit-man.log")" 'unexpected-Sieve-runtime' "explicit manual regeneration invokes Pandoc"
assert_equal "$(sha256sum doc/jaro.1 doc/jaromail.1)" "$man_before" "failed regeneration preserves manuals"
print -- 'PASS: build flags, cache, failure propagation and installation of committed manuals without Pandoc'
