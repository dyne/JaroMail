#!/usr/bin/env zsh

distro=unknown

builddir=`pwd`
# cc="${builddir}/cc-static.zsh"
cc="gcc -O3"
[[ "$OSTYPE" = linux-musl ]] && cc="gcc -Os -static"

target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

mkdir -p build/gnu


{ test "$target" = "fetchaddr" } || {
    test "$target" = "all" } && {
    pushd src
    print -n "Compiling the address parser (RFC2047) ... "
    ${=cc} -c helpers.c
    ${=cc} -c rfc2047.c
    ${=cc} -c rfc822.c
    ${=cc} -c -DHAVE_ICONV fetchaddr.c
    ${=cc} -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o
    popd
    cp src/fetchaddr build/gnu/
    print OK
}

{ test "$target" = "parsedate" } || {
    test "$target" = "all" } && {
    print -n "Compiling the date parsers (RFC822) ... "
    pushd src
    ${=cc} -o parsedate parsedate.c
    popd
    cp src/parsedate build/gnu/
    print OK
}

{ test "$target" = "dotlock" } || {
    test "$target" = "all" } && {
    print -n "Compiling the file dotlock... "
    pushd src
    ${=cc} -c dotlock.c -I . -DDL_STANDALONE
    ${=cc} -o dotlock dotlock.o
    popd
    cp src/dotlock build/gnu/
    print OK
}

{ test "$target" = "gpgewrap" } || {
    test "$target" = "all" } && {
    print -n "Compiling the GnuPG wrapper... "
    pushd src
    ${=cc} -c gpgewrap.c -I .
    ${=cc} -o gpgewrap gpgewrap.o
    popd
    cp src/gpgewrap build/gnu/
    print OK
}


# build mixmaster only if specified
{ test "$target" = "mixmaster" } && {
    print "Compiling Mixmaster (anonymous remailer)"
    pushd src/mixmaster-3.0/Src
    mixmaster_sources=(main menustats mix rem rem1 rem2 chain chain1 chain2 nym)
    mixmaster_sources+=(pgp pgpdb pgpdata pgpget pgpcreat pool mail rfc822 mime keymgt)
    mixmaster_sources+=(compress stats crypto random rndseed util buffers maildir parsedate.tab)
    bison parsedate.y
    for s in ${=mixmaster_sources}; do ${=cc} -c ${s}.c; done
    ${=cc} -o mixmaster *.o -lssl
    popd
    cp src/mixmaster-3.0/Src/mixmaster build/gnu
}


print
print "Done building JaroMail!"
print "Now run 'make install' as root to install jaromail in /usr/local"
print "use PREFIX=/home/private/jaromail to avoid system-wide installation."
print
