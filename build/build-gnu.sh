#!/usr/bin/env zsh

distro=unknown

builddir=`pwd`
# cc="${builddir}/cc-static.zsh"
cc="gcc -O3"

pushd ..

which apt-get > /dev/null && distro=debian
which yum > /dev/null && distro=fedora

target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

mkdir -p build/gnu

{ test "$target" = "deps" } || {
    test "$target" = "all" } && {
    deps=()
    case $distro in
    debian)
        print "Building on Debian"
        print "Checking software to install"
        which fetchmail >/dev/null || deps+=(fetchmail)
        which msmtp     >/dev/null || deps+=(msmtp)
        which mutt      >/dev/null || deps+=(mutt)
        which mairix    >/dev/null || deps+=(mairix)
        which pinentry  >/dev/null || deps+=(pinentry-curses)
        which abook     >/dev/null || deps+=(abook)
        which wipe      >/dev/null || deps+=(wipe)

        print "Checking build dependencies"
        which gcc      >/dev/null || deps+=(gcc)
        which bison    >/dev/null || deps+=(bison)
        which flex     >/dev/null || deps+=(flex)
        which make     >/dev/null || deps+=(make)
        which autoconf >/dev/null || deps+=(autoconf)
        which automake >/dev/null || deps+=(automake)
        which sqlite3  >/dev/null || deps+=(sqlite3)
#       which gpgme-config || sudo apt-get install libgpgme11-dev

        { test -r /usr/share/doc/libgnome-keyring-dev/copyright } || {
        deps+=(libglib2.0-dev libgnome-keyring-dev) }


        { test ${#deps} -gt 0 } && {
        print "Installing missing components"
        sudo apt-get install ${=deps} }

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
    print -n "Compiling the address parser... "
    ${=cc} -c helpers.c
    ${=cc} -c rfc2047.c
    ${=cc} -c rfc822_mutt.c;
    ${=cc} -c fetchaddr.c;
    ${=cc} -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822_mutt.o
    popd
    cp src/fetchaddr build/gnu/
    print OK
}

{ test "$target" = "dfasyn" } || {
    test "$target" = "all" } && {
    print "Compiling the generator for deterministic finite state automata... "
    pushd src/dfasyn
    make
    popd
}

{ test "$target" = "parsedate" } || {
    test "$target" = "all" } && {
    print "Compiling the minimalistic RFC822 date re-formatter..."
    pushd src
    ${=cc} -o parsedate parsedate.c
    popd
    cp src/parsedate build/gnu/
    print OK
}

{ test "$target" = "fetchdate" } || {
    test "$target" = "all" } && {
    print "Compiling the date parser... "
    pushd src
    # then the C files made by dfasyn
    ./dfasyn/dfasyn -o nvpscan.c -ho nvpscan.h -r nvpscan.report -u nvp.nfa
    # then the utilities
    ${=cc} -c rfc822_mairix.c
    ${=cc} -c nvp.c nvpscan.c
    ${=cc} -I . -c fetchdate.c
    ${=cc} -o fetchdate rfc822_mairix.o nvpscan.o nvp.o fetchdate.o
    popd
    cp src/fetchdate build/gnu/
    print OK
}



{ test "$target" = "gnome-keyring" } || {
    test "$target" = "all" } && {
    print "Compiling gnome-keyring"
    pushd src/gnome-keyring
    ${=cc} jaro-gnome-keyring.c -o jaro-gnome-keyring \
    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1`
    popd
    cp src/gnome-keyring/jaro-gnome-keyring build/gnu/
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

popd
