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
cc="llvm-gcc"
cpp="llvm-cpp-4.2"
# ${builddir}/clang-static-osx.sh"

#cflags="-arch x86_64 -arch i386 -O2"
OSX_SDK=10.7
cflags=(-I/usr/local/include -I/usr/include)
cflags+=(-I/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX${OSX_SDK}.sdk/usr/include)
# cflags+=(-arch x86_64)
# cflags+=(-arch i386)
cflags+=(-O2)


ldflags=(-L/usr/local/lib -L/usr/lib)
ldflags+=(-L/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX${OSX_SDK}.sdk/usr/lib)


target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

pushd ..

mkdir -p build/osx/dylib

root=`pwd`

copydeps() {
	# copy a binary and all dependencies until 3rd level

	libs=(`otool -L $1 | awk '
		/^\// {next} /\/local/ {print $1}'`)

	# for p1 in ${(f)libs}; do
	# 	echo "$p1" >> $tmp
	# 	# second pass
	# 	libs2=`otool -L $p1 | awk '
	# 		/^\// {next} /\/opt\/local/ {print $1}'`

	# 	for p2 in ${(f)libs2}; do
	# 		echo "$p2" >> $tmp
	# 		# third pass
	# 		libs3=`otool -L $p2 | awk '
	# 			/^\// {next} /\/opt\/local/ {print $1}'`

	# 		for p3 in ${(f)libs3}; do
	# 			echo "$p3" >> $tmp
	# 		done # 3rd
	# 	done # 2nd
	# done # 1st

	exe=`basename $1`
	dst=$2
	cp -v $1 $dst
	{ test $? = 0 } || { print "Error copying $1" }
	chmod +w $dst/$exe
	strip $dst/$exe
	for d in ${libs}; do
	    dylib=`basename $d`
	    print "  $dylib"
	    # skip iconv and use the one provided system wide
	    { test "$dylib" = "libiconv.2.dylib" } && {
		install_name_tool -change $d /usr/lib/libiconv.2.dylib $dst/$exe
		continue }
	    # make sure destination is writable
	    dylibdest=$dst/../Frameworks/`basename $d`
	    { test -r $dylibdest } || { cp "$d" "$dylibdest" }
	    install_name_tool -change $d \
		"@executable_path/../Frameworks/`basename $d`" $dst/$exe
	done

}


print "Building Jaro Mail binary stash for Apple/OSX"

if ! [ -r /opt/local/bin/port ]; then
	print "MacPorts not found in /opt/local. Operation aborted."
	return 1
fi

{ test "$target" = "abquery" } || { 
    test "$target" = "all" } && {
# build apple addressbook query
    print "Address book query"
    pushd src/ABQuery
    xcodebuild > /dev/null
    popd
    cp src/ABQuery/build/Release/lbdb-ABQuery build/osx/ABQuery
}

  
{ test "$target" = "fetchaddr" } || { 
    test "$target" = "all" } && {
# build our own address parser
    pushd src
    print "Address parser"
    $cc ${=cflags} -c fetchaddr.c helpers.c rfc2047.c rfc822.c
    $cc -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o ${=ldflags};
    popd
}

{ test "$target" = "mairix" } || { 
    test "$target" = "all" } && {
# build mairix
    pushd src/mairix
    print "Search engine and date parser"
    CFLAGS="${=cflags} ${=ldflags} -L/usr/local/opt/bison/lib" \
    CFLAGS="$CFLAGS -L/usr/local/opt/flex/lib -I/usr/local/opt/flex/include" \
    CC="$cc" LD=ld CPP="$cpp"  \
	./configure --disable-gzip-mbox --disable-bzip-mbox ; make 
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

# build our own mutt
{ test "$target" = "mutt" } || { 
    test "$target" = "all" } && {
    echo "Compiling Mutt (MUA)"
    pushd src/mutt-1.5.21

    CC="$gcc" CPP="$cpp" CFLAGS="${=cflags} -I/usr/local/Cellar/gettext/0.18.2/include" \
    CPPFLAGS="${=cflags}" LDFLAGS="${=ldflags} -L/usr/local/Cellar/gettext/0.18.2/lib" ./configure \
	--with-ssl --with-gnutls --enable-imap --disable-debug \
	--with-slang --disable-gpgme \
	--enable-hcache --with-regex --with-tokyocabinet \
	--with-mixmaster=${root}/src/mixmaster --enable-pgp
    make keymap_defs.h
    make reldate.h
    CFLAGS="-I/usr/local/Cellar/gettext/0.18.2/include" make hcversion.h
    make mutt
    make pgpewrap
    popd
}

# build our own lynx (no ssl)
{ test "$target" = "lynx" } || {
  test "$target" = "all" } && {
  echo "Compiling Lynx (html 2 txt)"
  pushd src/lynx2-8-7
    CC="$gcc" CPP="$cpp" CFLAGS="${=cflags} -I/usr/local/Cellar/gettext/0.18.2/include" \
    CPPFLAGS="${=cflags}" LDFLAGS="${=ldflags} -L/usr/local/Cellar/gettext/0.18.2/lib" ./configure \
    --disable-trace --enable-nls --with-screen=slang --enable-widec --enable-default-colors \
    --disable-file-upload --disable-persistent-cookie --with-bzlib --with-zlib \
    --disable-finger --disable-gopher --disable-news --disable-ftp --disable-dired \
    --enable-cjk --enable-japanese-utf8 --enable-charset-selection
  make
  popd
}

 #  CFLAGS="${=cflags}" \

pushd build
dst=JaroMail.app/Contents/MacOS

# copy all built binaries in place
{ test "$target" = "install" } || { 
    test "$target" = "all" } && {

    mkdir -p $dst

# static ones do not require relocated links
    cp -v ${root}/src/fetchdate $dst
    cp -v ${root}/src/fetchaddr $dst
    cp -v ${root}/src/mairix/mairix $dst
    cp -v ${root}/src/dotlock $dst


    copydeps ${root}/src/mutt-1.5.21/mutt      $dst
    copydeps ${root}/src/mutt-1.5.21/pgpewrap  $dst
    copydeps /opt/local/bin/fetchmail          $dst
    copydeps ${root}/src/lynx2-8-7/lynx        $dst
    cp       ${root}/src/lynx2-8-7/lynx.cfg    $dst/lynx.cfg

    copydeps /usr/local/bin/gfind     $dst
    copydeps /usr/local/bin/msmtp     $dst
    copydeps /usr/local/bin/gpg       $dst
    copydeps /usr/local/bin/pinentry  $dst
    copydeps /usr/local/bin/abook     $dst

    # rename to avoid conflicts
    mv $dst/gpg  $dst/gpg-jaro
    mv $dst/mutt $dst/mutt-jaro



    # system wide
    # rm build/osx/dylib/libiconv.2.dylib
}

cat <<EOF > $dst/jaroshell.sh
export PATH="$PATH:/Applications/JaroMail.app/Contents/MacOS"
export GNUPGHOME="$HOME/.gnupg"
export MAILDIRS="$HOME/Library/Application\ Support/JaroMail"
export WORKDIR="/Applications/JaroMail.app/Contents/Resources"
mkdir -p $MAILDIRS $WORKDIR
clear
zsh
EOF

cat <<EOF > $dst/JaroMail.command
#!/bin/zsh
# JaroMail startup script for Mac .app
# Copyright (C) 2012-2013 by Denis Roio <Jaromil@dyne.org>
# GNU GPL V3 (see COPYING)
osascript <<EOF
tell application "System Events" to set terminalOn to (exists process "Terminal")
tell application "Terminal"
   if (terminalOn) then
        activate
        do script "source /Applications/JaroMail.app/Contents/MacOS/jaroshell.sh; exit"
   else
        do script "source /Applications/JaroMail.app/Contents/MacOS/jaroshell.sh; exit" in front window
   end if
end tell
EOF
echo "EOF" >> $dst/JaroMail.command

chmod +x $dst/JaroMail.command

cat <<EOF > JaroMail.app/Contents/PkgInfo
APPLJAROMAIL
EOF

cat <<EOF > JaroMail.app/Contents/Info.plist
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
	<string>JaroMail.command</string>
	<key>CFBundleIconFile</key>
	<string>jaromail.icns</string>
	<key>CFBundleIdentifier</key>
	<string>org.dyne.jaromail</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>JAROMAIL</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>CFBundleVersion</key>
	<string>1.0</string>
	<key>CSResourcesFileMapped</key>
	<true/>
</dict>
</plist>
EOF

popd

mkdir -p build/JaroMail.app/Contents/Resources/
./install.sh build/JaroMail.app/Contents/Resources/
