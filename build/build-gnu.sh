#!/usr/bin/env zsh

distro=unknown

builddir=`pwd`
# cc="${builddir}/cc-static.zsh"
cc="gcc -O3"

which apt-get > /dev/null && distro=debian
which yum > /dev/null && distro=fedora

target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

mkdir -p build/gnu

debian_req() {
    for p in "$@"; do
        { dpkg --get-selections "$p" | grep -q "[[:space:]]install$" } || {
            sudo apt-get install "$p" } || {
            print "Failed to install $p"
            return 1
        }
    done
    return 0
}


{ test "$target" = "deps" } || {
    test "$target" = "all" } && {
    case $distro in
        debian)
            deps=(fetchmail msmtp mutt pinentry-curses)
            deps+=(wipe notmuch sqlite3 alot abook)
            deps+=(gcc make libglib2.0-dev libgnome-keyring-dev)

        print "Building on Debian"
        print "Checking software to install"
        debian_req $deps
        ;;

    fedora)

        print "Building on Fedora"
        print "Checking software to install..."
        which zsh || sudo yum install zsh
        which mutt || sudo yum install mutt
        which procmail || sudo yum install procmail
        which msmtp || sudo yum install msmtp
        which pinentry || sudo yum install pinentry
        which fetchmail || sudo yum install fetchmail
        which wipe || sudo yum install wipe
        which abook || sudo yum install abook
        which notmuch || sudo yum install notmuch
        which alot || sudo yum install alot

        print "Checking build dependencies"
        which gcc || sudo yum install gcc
        which bison || sudo yum install bison
        which flex || sudo yum install flex
        rpm -q glib2-devel || sudo yum install glib2-devel
        rpm -q libgnome-keyring-devel || sudo yum install libgnome-keyring-devel
        rpm -q bzip2-devel || sudo yum install bzip2-devel
        rpm -q zlib-devel || sudo yum install zlib-devel

        ;;

    *)
        print "Error: no distro recognized, build by hand."
        ;;
    esac

    print "All dependencies installed"
}

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


{ test "$target" = "gnome-keyring" } || {
    test "$target" = "all" } && {
    print -n "Compiling gnome-keyring... "
    pushd src/gnome-keyring
    ${=cc} jaro-gnome-keyring.c -o jaro-gnome-keyring \
    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1`
    popd
    cp src/gnome-keyring/jaro-gnome-keyring build/gnu/
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

