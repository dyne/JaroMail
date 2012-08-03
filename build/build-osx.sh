#!/usr/bin/env zsh

# this script creates a binary build for Apple/OSX
# it requires all needed macports to be installed:

# mutt
# sudo port -vc install mutt-devel +gnuregex +gpgme +headercache +imap +pop +sasl +smtp +ssl +tokyocabinet +universal

# gpg
# sudo port install gpg +universal
# sudo port install pinentry +universal

# msmtp
# sudo port install msmtp +universal

# and local natives: fetchmail, procmail...

builddir=`pwd`

cc="${builddir}/clang-static-osx.sh"
#cflags="-arch x86_64 -arch i386 -O2"


pushd ..

mkdir -p build/osx/dylib

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

	{ test -r build/osx/`basename $1`.bin } || {
		cp $1 build/osx/`basename $1`.bin }
	for d in `cat $tmp | sort | uniq`; do
		if ! [ -r build/osx/dylib/`basename $d` ]; then
			cp $d build/osx/dylib/
			print "`basename $d`"
		fi
	done
	{ test -r build/osx/`basename $1` } || {
		# create a wrapper
		cat <<EOF > build/osx/`basename $1`
#!/usr/bin/env zsh
test -r \$HOME/Mail/jaro/bin/jaro && PDIR=\$HOME/Mail/jaro
test -r jaro/bin/jaro && PDIR="\`pwd\`/jaro"
test \$JAROMAIL && PDIR=\$JAROMAIL
DYLD_LIBRARY_PATH=\$PDIR/bin/dylib:\$DYLD_LIBRARY_PATH
\$PDIR/bin/`basename $1`.bin \${=@}
EOF
		chmod +x build/osx/`basename $1`
	}
}

print "Building Jaro Mail binary stash for Apple/OSX"

if ! [ -r /opt/local/bin/port ]; then
	print "MacPorts not found in /opt/local. Operation aborted."
	return 1
fi


# build apple addressbook query
print "Address book query"
pushd src/ABQuery
xcodebuild > /dev/null
popd
cp src/ABQuery/build/Release/lbdb-ABQuery build/osx/ABQuery


# build our own address parser
pushd src
print "Address parser"
$cc -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
$cc -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o;
popd


# build mairix
pushd src/mairix
print "Search engine and date parser"
CC="$cc" LD=/usr/bin/ld CPP=/usr/bin/cpp \
    ./configure --disable-gzip-mbox --disable-bzip-mbox \
    > /dev/null ; make 2>&1 > /dev/null
popd

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

# build our own msmtp
# port deps: libidn ...
pushd src/msmtp
print "SMTP Simple mail transport protocol agent"
CC="$cc" LD=/usr/bin/ld CPP=/usr/bin/cpp \
    ./configure --without-macosx-keyring --without-gnome-keyring \
    > /dev/null ; make 2>&1 > /dev/null
popd

 #  CFLAGS="${=cflags}" \


# copy all binaries built
cp src/fetchdate build/osx/
cp src/fetchaddr build/osx/
cp src/mairix/mairix build/osx/
cp src/msmtp/src/msmtp build/osx/
copydeps /opt/local/bin/mutt
copydeps /opt/local/bin/mutt_dotlock
mv build/osx/mutt_dotlock build/osx/dotlock
copydeps build/osx/msmtp
copydeps /opt/local/bin/gpg
copydeps /opt/local/bin/pinentry
copydeps /opt/local/bin/lynx




popd
