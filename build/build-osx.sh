#!/usr/bin/env zsh

# this script creates a binary build for Apple/OSX
# it requires all needed macports to be installed:


# pinentry
# sudo port install pinentry +universal

# msmtp
# sudo port install msmtp +universal

# and local natives: fetchmail, procmail...

builddir=`pwd`

#cc="${builddir}/cc-static.zsh"
#cc="gcc-4.7"
#cpp="cpp-4.7"

OSX_SDK=10.8
#cc="llvm-gcc"
#cpp="llvm-cpp-4.2"
cflags+=(-I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX${OSX_SDK}.sdk/usr/include)
ldflags+=(-L/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX${OSX_SDK}.sdk/usr/lib)

#cc="gcc"
#cpp="cpp"

# ${builddir}/clang-static-osx.sh"

#cflags="-arch x86_64 -arch i386 -O2"
cflags=(-I/opt/local/include -I/usr/include)
cflags+=(-arch x86_64)
#cflags+=(-arch i386)
cflags+=(-O2)


ldflags=(-L/opt/local/lib -L/usr/lib)


target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

pushd ..

mkdir -p build/osx/dylib

root=`pwd`

copydeps() {
	# copy a binary and all dependencies until 3rd level
	# 1st argument is the binary (full path)
	# 2nd is the .app where to copy it (relative)

	libs=(`otool -L $1 | awk '
		/^\// {next} /\/local/ {print $1}'`)

	exe=`basename $1`
	bin=osx
	lib=osx/dylib
	cp -v $1 $bin/$exe
	{ test $? = 0 } || { print "Error copying $1" }
	chmod +w $bin/$exe
	strip $bin/$exe
	for d in ${libs}; do
	    dylib=`basename $d`
	    print "  $dylib"
	    # skip iconv and use the one provided system wide
#	    { test "$dylib" = "libiconv.2.dylib" } && {
#		install_name_tool -change $d /usr/lib/libiconv.2.dylib $dst/Contents/MacOS/$exe
#		continue }
	    # make sure destination is writable
	    dylibdest=$lib/`basename $d`
	    { test -r $dylibdest } || { cp -v "$d" "$dylibdest" }
	    install_name_tool -change $d \
		"/Applications/JaroMail.app/Contents/Frameworks/`basename $d`" $bin/$exe
	done

}


print "Building Jaro Mail binary stash for Apple/OSX"


if ! [ -r /opt/local/bin/port ]; then
	print "MacPorts not found in /opt/local. Operation aborted."
	return 1
fi

# { test "$target" = "abquery" } || { 
#     test "$target" = "all" } && {
# # build apple addressbook query
#     print "Address book query"
#     pushd src/ABQuery
#     xcodebuild > /dev/null
#     popd
# }

{ test "$target" = "dfasyn" } || { 
    test "$target" = "all" } && {
    pushd src/mairix/dfasyn
    ./configure
    make
    popd
}

{ test "$target" = "mairix" } || { 
    test "$target" = "all" } && {
    pushd src/mairix
    print "Building Mairix"
    ./configure
    make
    popd
}
  
{ test "$target" = "fetchaddr" } || { 
    test "$target" = "all" } && {
# build our own address parser
    pushd src
    print "Address parser"
    $cc ${=cflags} -c fetchaddr.c
    $cc ${=cflags} -c helpers.c
    $cc ${=cflags} -c rfc2047.c
    $cc ${=cflags} -c rfc822_mutt.c
    $cc -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822_mutt.o ${=ldflags};
    popd
}

{ test "$target" = "fetchdate" } || { 
    test "$target" = "all" } && {
# build our own fetchdate
    pushd src
    $cc ${=cflags} ${=ldflags} -I mairix -c fetchdate.c
    $cc ${=cflags} ${=ldflags} -DHAS_STDINT_H -DHAS_INTTYPES_H \
	-o fetchdate fetchdate.o \
	mairix/datescan.o mairix/db.o mairix/dotlock.o \
	mairix/expandstr.o mairix/glob.o mairix/md5.o \
	mairix/nvpscan.o mairix/rfc822.o mairix/stats.o \
	mairix/writer.o mairix/dates.o mairix/dirscan.o \
	mairix/dumper.o mairix/fromcheck.o mairix/hash.o mairix/mbox.o \
	mairix/nvp.o mairix/reader.o mairix/search.o mairix/tok.o
    popd
}


{ test "$target" = "dotlock" } || { 
    test "$target" = "all" } && {
# build our own dotlock
    pushd src
    $cc ${=cflags} -c dotlock.c
    $cc ${=ldflags} -o dotlock dotlock.o
    popd
}

# { test "$target" = "msmtp" } || { 
#     test "$target" = "all" } && {
# # build our own msmtp
# # port deps: libidn gnutls
#     pushd src/msmtp
#     print "SMTP Simple mail transport protocol agent"
#     CC="$cc" LD=/usr/bin/ld CPP=/usr/bin/cpp \
# 	./configure --without-macosx-keyring --without-gnome-keyring \
# 	> /dev/null ; make 2>&1 > /dev/null
#     popd
# }

pushd build
appdst=JaroMail.app

# copy all built binaries in place
{ test "$target" = "install" } || { 
    test "$target" = "all" } && {

    mkdir -p $bindst
    mkdir -p $appdst/Contents/Frameworks

# static ones do not require relocated links
    cp -v ${root}/src/fetchdate
    cp -v ${root}/src/fetchaddr
#    cp -v ${root}/src/ABQuery/build/Release/lbdb-ABQuery
    copydeps /opt/local/bin/mutt
    copydeps /opt/local/bin/mutt_dotlock
    copydeps /opt/local/bin/pgpewrap
#    copydeps /opt/local/bin/procmail
    copydeps /opt/local/bin/fetchmail
    copydeps /opt/local/bin/elinks
    copydeps /opt/local/bin/gfind
    copydeps /opt/local/bin/msmtp
    copydeps /opt/local/bin/gpg
    copydeps /opt/local/bin/pinentry
    copydeps /opt/local/bin/pinentry-curses
    copydeps /opt/local/bin/abook

    # system wide
    # rm build/osx/dylib/libiconv.2.dylib
}
