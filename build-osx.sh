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

source src/jaro source

mkdir -p build/osx/dylib

copydeps() {
	# copy a binary and all dependencies until 3rd level
	act "`basename $1`"

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
			act "`basename $d`"
		fi
	done
	if ! [ -r build/osx/`basename $1` ]; then
		# create a wrapper
		cat <<EOF > build/osx/`basename $1`
#!/usr/bin/env sh
PDIR=\$HOME/Mail/jaro
DYLD_LIBRARY_PATH=\$PDIR/bin/dylib:\$DYLD_LIBRARY_PATH
\$PDIR/bin/`basename $1`.bin \$@
EOF
		chmod +x build/osx/`basename $1`
	fi
}

notice "Building Jaro Mail binary stash for Apple/OSX"

if ! [ -r /opt/local/bin/port ]; then
	error "MacPorts not found in /opt/local. Operation aborted."
	return 1
fi
act "lbdb address book module"
cd src/lbdb-ABQuery
xcodebuild > /dev/null
cd -
cp src/lbdb-ABQuery/build/Release/lbdb-ABQuery build/osx/ABQuery
cd src/lbdb
gcc -O2 -c -m32 fetchaddr.c helpers.c rfc2047.c rfc822.c; \
gcc -O2 -m32 -o fetchaddr.32 fetchaddr.o helpers.o rfc2047.o rfc822.o;
gcc -O2 -c -m64 fetchaddr.c helpers.c rfc2047.c rfc822.c; \
gcc -O2 -m64 -o fetchaddr.64 fetchaddr.o helpers.o rfc2047.o rfc822.o;
lipo -arch i386 fetchaddr.32 -arch x86_64 fetchaddr.64 -output fetchaddr -create
gcc -O2 -m32 -o dotlock.32 dotlock.c
gcc -O2 -m64 -o dotlock.64 dotlock.c
lipo -arch i386 dotlock.32 -arch x86_64 dotlock.64 -output dotlock -create
rm -f *.32 *.64
cd -
cp src/lbdb/dotlock   build/osx/
cp src/lbdb/fetchaddr build/osx/

copydeps bin/mutt
copydeps bin/msmtp
copydeps bin/gpg
copydeps bin/pinentry
copydeps bin/lynx

cp src/jaro build/osx

