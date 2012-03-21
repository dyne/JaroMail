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

source src/postino source

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
PDIR=\$HOME/.postino
DYLD_LIBRARY_PATH=\$PDIR/bin/dylib:\$DYLD_LIBRARY_PATH
\$PDIR/bin/`basename $1`.bin \$@
EOF
		chmod +x build/osx/`basename $1`
	fi
}

notice "Building Postino binary stash for Apple/OSX"

act "lbdb address book module"
cd aux/lbdb-ABQuery
xcodebuild > /dev/null
cd -
cp aux/lbdb-ABQuery/build/Release/lbdb-ABQuery build/osx/ABQuery

copydeps bin/mutt
copydeps bin/msmtp
copydeps bin/gpg
copydeps bin/pinentry

cp src/postino build/osx

