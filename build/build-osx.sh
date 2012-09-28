#!/usr/bin/env zsh

# this script creates a binary build for Apple/OSX
# it requires all needed macports to be installed:

# mutt
# sudo port -vc install mutt-devel +gnuregex +gpgme +headercache +imap +pop +sasl +smtp +ssl +tokyocabinet +universal

# pinentry
# sudo port install pinentry +universal

# msmtp
# sudo port install msmtp +universal

# and local natives: fetchmail, procmail...

builddir=`pwd`

#cc="${builddir}/cc-static.zsh"
cc="${builddir}/clang-static-osx.sh"

#cflags="-arch x86_64 -arch i386 -O2"

cflags=(-I/opt/local/include -I/usr/include)
cflags+=(-arch x86_64)
cflags+=(-arch i386)
cflags+=(-O2)


ldflags="-L/opt/local/lib -L/usr/lib"


target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

pushd ..

mkdir -p build/osx/dylib

root=`pwd`

copydeps() {
	# copy a binary and all dependencies until 3rd level
	print "`basename $1`"

	tmp="/tmp/build_`basename $1`";
	rm -f $tmp; touch $tmp

	libs=`otool -L $1 | awk '
		/^\// {next} /\/opt\/local/ {print $1}'`

	for p1 in ${(f)libs}; do
		echo "$p1" >> $tmp
		# second pass
		libs2=`otool -L $p1 | awk '
			/^\// {next} /\/opt\/local/ {print $1}'`

		for p2 in ${(f)libs2}; do
			echo "$p2" >> $tmp
			# third pass
			libs3=`otool -L $p2 | awk '
				/^\// {next} /\/opt\/local/ {print $1}'`

			for p3 in ${(f)libs3}; do
				echo "$p3" >> $tmp
			done # 3rd
		done # 2nd
	done # 1st

	cp $1 build/osx/`basename $1`.bin
	strip build/osx/`basename $1`.bin
	for d in `cat $tmp | sort | uniq`; do
	    if ! [ -r build/osx/dylib/`basename $d` ]; then
		cp $d build/osx/dylib/
		print "`basename $d`"
	    fi
	done

		# create a wrapper
	cat <<EOF > build/osx/`basename $1`
#!/usr/bin/env zsh
test -r \$HOME/Mail/jaro/bin/jaro && PDIR=\$HOME/Mail/jaro
test -r jaro/bin/jaro && PDIR="\`pwd\`/jaro"
test \$JAROMAIL && PDIR=\$JAROMAIL
DYLD_LIBRARY_PATH=\$PDIR/bin/dylib:\$DYLD_LIBRARY_PATH \\
\$PDIR/bin/`basename $1`.bin \${=@}
EOF
	chmod +x build/osx/`basename $1`

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
    $cc -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
	$cc -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o;
    popd
}

{ test "$target" = "mairix" } || { 
    test "$target" = "all" } && {
# build mairix
    pushd src/mairix
    print "Search engine and date parser"
    CC="$cc" LD=/usr/bin/ld CPP=/usr/bin/cpp \
	./configure --disable-gzip-mbox --disable-bzip-mbox \
	> /dev/null ; make 2>&1 > /dev/null
    popd
}

{ test "$target" = "fetchdate" } || { 
    test "$target" = "all" } && {
# build our own fetchdate
    pushd src
    $cc -I mairix -c fetchdate.c
    $cc -DHAS_STDINT_H -DHAS_INTTYPES_H \
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
    $cc -c dotlock.c
    $cc -o dotlock dotlock.o
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

    CC=clang CFLAGS="$cflags" LDFLAGS="$ldflags" ./configure \
	--with-ssl --with-gnutls --enable-imap --disable-debug \
	--with-slang --disable-gpgme \
	--enable-hcache --with-regex --with-tokyocabinet \
	--with-mixmaster=${root}/src/mixmaster --enable-pgp \
	> /dev/null
    make mutt > /dev/null
    { test $? = 0 } && {
	 mv mutt mutt-jaro
	 mv mutt_dotlock dotlock-mutt
    }
    popd
}


 #  CFLAGS="${=cflags}" \

{ test "$target" = "install" } || { 
    test "$target" = "all" } && {

    mkdir -p build/osx/dylib

# copy all binaries built
    cp -v src/fetchdate build/osx/
    cp -v src/fetchaddr build/osx/
    cp -v src/mairix/mairix build/osx/
# cp src/msmtp/src/msmtp build/osx/
    cp -v src/dotlock build/osx/
    copydeps ${root}/src/mutt-1.5.21/mutt-jaro
    copydeps ${root}/src/mutt-1.5.21/dotlock-mutt
    copydeps ${root}/src/mutt-1.5.21/pgpewrap
    copydeps /opt/local/bin/gfind
    copydeps /opt/local/bin/msmtp
    copydeps /opt/local/bin/pinentry
    copydeps /opt/local/bin/abook
    copydeps /opt/local/bin/lynx

}


popd
