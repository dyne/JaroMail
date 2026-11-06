#!/usr/bin/env zsh

set -eu
cd "${0:A:h:h}"
target="${1:-all}"
case "$target" in
    all|fetchaddr|parsedate|dotlock|gpgewrap|jaro-filter-compile|mutt-kz|mixmaster) ;;
    *) print -u2 -- "Unknown build target: $target"; exit 2 ;;
esac

default_cflags=-O3
default_ldflags=""
if [[ "$OSTYPE" == linux-musl ]]; then
    default_cflags=-Os
    default_ldflags=-static
fi
cc=(${=CC:-gcc})
cppflags=(${=CPPFLAGS:-})
cflags=(${=CFLAGS-$default_cflags})
ldflags=(${=LDFLAGS-$default_ldflags})
ldlibs=(${=LDLIBS:-})
cache=()
if [[ "${cc[1]:t}" != (ccache|sccache|distcc) ]]; then
    if (( ${+CCACHE} )); then
        cache=(${=CCACHE})
    elif command -v ccache >/dev/null 2>&1; then
        cache=(ccache)
    fi
fi

compile() {
    "${cache[@]}" "${cc[@]}" "${cppflags[@]}" "${cflags[@]}" -c "$@"
}

link_helper() {
    "${cc[@]}" "${cflags[@]}" "${ldflags[@]}" -o "$@" "${ldlibs[@]}"
}

build_fetchaddr() {
    compile src/helpers.c -o src/helpers.o &&
        compile src/rfc2047.c -o src/rfc2047.o &&
        compile src/rfc822.c -o src/rfc822.o &&
        compile -DHAVE_ICONV src/fetchaddr.c -o src/fetchaddr.o &&
        link_helper src/fetchaddr src/fetchaddr.o src/helpers.o src/rfc2047.o src/rfc822.o &&
        cp src/fetchaddr build/gnu/
}

build_parsedate() {
    compile src/parsedate.c -o src/parsedate.o &&
        link_helper src/parsedate src/parsedate.o &&
        cp src/parsedate build/gnu/
}

build_dotlock() {
    compile -I src -DDL_STANDALONE src/dotlock.c -o src/dotlock.o &&
        link_helper src/dotlock src/dotlock.o &&
        cp src/dotlock build/gnu/
}

build_gpgewrap() {
    compile -I src src/gpgewrap.c -o src/gpgewrap.o &&
        link_helper src/gpgewrap src/gpgewrap.o &&
        cp src/gpgewrap build/gnu/
}

build_jaro-filter-compile() {
    make -f build/filter-core.mk filter-compile "CC=${CC:-gcc}" "CCACHE=${CCACHE-${cache[*]}}" \
        "CPPFLAGS=${CPPFLAGS-}" "CFLAGS=${cflags[*]}" "LDFLAGS=${ldflags[*]}" "LDLIBS=${LDLIBS-}" &&
        cp build/gnu/filter/jaro-filter-compile build/gnu/filter/jaro-filter-legacy build/gnu/filter/jaro-filter build/gnu/
}

build_mutt-kz() {
    (cd src/mutt-kz &&
        ./configure --enable-notmuch --enable-sidebar --enable-hcache --enable-gpgme --enable-imap --enable-pop &&
        make)
}

build_mixmaster() {
    (
        cd src/mixmaster-3.0/Src || exit $?
        sources=(main menustats mix rem rem1 rem2 chain chain1 chain2 nym
            pgp pgpdb pgpdata pgpget pgpcreat pool mail rfc822 mime keymgt
            compress stats crypto random rndseed util buffers maildir parsedate.tab)
        bison parsedate.y || exit $?
        for source_file in "${sources[@]}"; do
            compile "${source_file}.c" -o "${source_file}.o" || exit $?
        done
        link_helper mixmaster *.o -lssl && cp mixmaster ../../../build/gnu/
    )
}

mkdir -p build/gnu || exit $?
if [[ "$target" == all ]]; then
    targets=(fetchaddr parsedate dotlock gpgewrap jaro-filter-compile)
else
    targets=("$target")
fi
for helper in "${targets[@]}"; do
    print -- "Building $helper ..."
    "build_$helper" || exit $?
done
print -- "Done building JaroMail!"
print -- "Run 'make install' to install, or set PREFIX for a private installation."
