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

cc="clang"
cflags="-arch x86_64 -arch i386 -O2"

cd ..

mkdir -p build/osx/dylib

copydeps() {
	# copy a binary and all dependencies until 3rd level
	print "`basename $1`"

	tmp="/tmp/build_`basename $1`";
	rm -f $tmp; touch $tmp

	libs=`otool -L /opt/local/$1 | awk '
		/^\/opt/ {next} /\/opt\/local/ {print $1}'`

	for p1 in ${(f)libs}; do
		echo "$p1" >> $tmp
		# second pass
		libs2=`otool -L $p1 | awk '
			/^\/opt/ {next} /\/opt\/local/ {print $1}'`

		for p2 in ${(f)libs2}; do
			echo "$p2" >> $tmp
			# third pass
			libs3=`otool -L $p2 | awk '
				/^\/opt/ {next} /\/opt\/local/ {print $1}'`

			for p3 in ${(f)libs3}; do
				echo "$p3" >> $tmp
			done # 3rd
		done # 2nd
	done # 1st

	if ! [ -r build/osx/`basename $1`.bin ]; then
		cp /opt/local/$1 build/osx/`basename $1`.bin
	fi
	for d in `cat $tmp | sort | uniq`; do
		if ! [ -r build/osx/dylib/`basename $d` ]; then
			cp $d build/osx/dylib/
			print "`basename $d`"
		fi
	done
	if ! [ -r build/osx/`basename $1` ]; then
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
	fi
}

print "Building Jaro Mail binary stash for Apple/OSX"

if ! [ -r /opt/local/bin/port ]; then
	print "MacPorts not found in /opt/local. Operation aborted."
	return 1
fi
print "Address book query"
pushd src/ABQuery
xcodebuild > /dev/null
popd
cp src/ABQuery/build/Release/lbdb-ABQuery build/osx/ABQuery
pushd src
print "Address parser"
$cc ${=cflags} -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
$cc ${=cflags} -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o;
popd
# mairix
pushd src/mairix
print "Search engine and date parser"
make clean
CC="$cc" CFLAGS="${=cflags}" ./configure --disable-gzip-mbox --disable-bzip-mbox \
    > /dev/null ; make 2>&1 > /dev/null
popd

# fetchdate
pushd src
$cc ${=cflags} -I mairix -c fetchdate.c
$cc ${=cflags} -DHAS_STDINT_H -DHAS_INTTYPES_H \
    -o fetchdate fetchdate.o \
    mairix/datescan.o mairix/db.o mairix/dotlock.o \
    mairix/expandstr.o mairix/glob.o mairix/md5.o \
    mairix/nvpscan.o mairix/rfc822.o mairix/stats.o \
    mairix/writer.o mairix/dates.o mairix/dirscan.o \
    mairix/dumper.o mairix/fromcheck.o mairix/hash.o mairix/mbox.o \
    mairix/nvp.o mairix/reader.o mairix/search.o mairix/tok.o
popd
cp src/fetchdate build/osx/
cp src/fetchaddr build/osx/
cp src/mairix/mairix build/osx/
copydeps bin/mutt
copydeps bin/mutt_dotlock
copydeps bin/msmtp
copydeps bin/gpg
copydeps bin/pinentry
copydeps bin/lynx

mv build/osx/mutt_dotlock \
   build/osx/dotlock

