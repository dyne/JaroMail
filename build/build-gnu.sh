#!/usr/bin/env zsh

distro=unknown

builddir=`pwd`
# cc="${builddir}/cc-static.zsh"
cc="gcc -O3 -fomit-frame-pointer -ffast-math"

pushd ..

which apt-get && distro=debian
which yum && distro=fedora

target=all
{ test -z $1 } || { target="$1" }
# no other distro supported atm

mkdir -p build/gnu

case $distro in
    debian)

	{ test "$target" = "deps" } || { 
	    test "$target" = "all" } && {
		echo "Checking software to install..."
		which zsh || sudo apt-get install zsh
		which msmtp || sudo apt-get install msmtp
	
		echo "Checking build dependencies"
		which gcc || sudo apt-get install gcc
		which bison || sudo apt-get install bison
		which flex || sudo apt-get install flex
		which make || sudo apt-get install make
		which autoconf || sudo apt-get install autoconf
		which automake || sudo apt-get install automake
		which sqlite3 || sudo apt-get install sqlite3

		{ test -r /usr/include/bzlib.h } || {
		    sudo apt-get install libbz2-dev }
		{ test -r /usr/share/doc/libgnome-keyring-dev/copyright } || {
		    sudo apt-get install libglib2.0-dev libgnome-keyring-dev }
		{ test -r /usr/lib/pkgconfig/tokyocabinet.pc } || {
		    sudo apt-get install libtokyocabinet-dev }
		{ test -r /usr/share/doc/libslang2-dev/copyright } || {
		    sudo apt-get install libslang2-dev }
		{ test -r /usr/share/doc/libssl-dev/copyright } || {
		    sudo apt-get install libssl-dev }
		{ test -r /usr/share/doc/libgnutls-dev/copyright } || {
		    sudo apt-get install libgnutls-dev }

		which gpgme-config || sudo apt-get install libgpgme11-dev
		echo "All dependencies installed"
	}

	{ test "$target" = "dotlock" } || { 
	    test "$target" = "all" } && {
	    pushd src
	    echo -n "Compiling the file lock utility... "
	    ${=cc} -o dotlock dotlock.c
	    popd
	    cp src/dotlock build/gnu/dotlock
	}

	{ test "$target" = "fetchaddr" } || { 
	    test "$target" = "all" } && {
		pushd src
		echo -n "Compiling the address parser... "
		${=cc} -c fetchaddr.c helpers.c rfc2047.c rfc822.c;
		${=cc} -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o -lbz2
		popd
		cp src/fetchaddr build/gnu/
	}

	{ test "$target" = "mairix" } || { 
	    test "$target" = "all" } && {
		echo "Compiling the search engine..."
		pushd src/mairix
		CC="$cc" ./configure > /dev/null
		make > make.log
		popd
		cp src/mairix/mairix build/gnu/
	}

	{ test "$target" = "fetchdate" } || {
	    test "$target" = "all" } && { 
		echo -n "Compiling the date parser... "
		pushd src
		${=cc} -I mairix -c fetchdate.c
		${=cc} -DHAS_STDINT_H -DHAS_INTTYPES_H -DUSE_GZIP_MBOX \
		    -o fetchdate fetchdate.o \
		    mairix/datescan.o mairix/db.o mairix/dotlock.o \
		    mairix/expandstr.o mairix/glob.o mairix/md5.o \
		    mairix/nvpscan.o mairix/rfc822.o mairix/stats.o \
		    mairix/writer.o mairix/dates.o mairix/dirscan.o \
		    mairix/dumper.o mairix/fromcheck.o mairix/hash.o mairix/mbox.o \
		    mairix/nvp.o mairix/reader.o mairix/search.o mairix/tok.o \
		    -lz -lbz2
		popd
		cp src/fetchdate build/gnu/
	}
		


	{ test "$target" = "gnome-keyring" } || { 
	    test "$target" = "all" } && {
		echo "Compiling gnome-keyring"
		pushd src/gnome-keyring
		${=cc} jaro-gnome-keyring.c -o jaro-gnome-keyring \
		    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1`
		popd
		cp src/gnome-keyring/jaro-gnome-keyring build/gnu/
	}

	{ test "$target" = "mutt" } || { 
	    test "$target" = "all" } && {
		echo "Compiling Mutt (MUA)"
		pushd src/mutt-1.5.21
		{ test -r configure } || { autoreconf -i }
		CC="$cc" LDFLAGS="-lm" ./configure \
		    --with-ssl --with-gnutls --enable-imap --disable-debug --with-slang --disable-gpgme \
		    --enable-hcache --with-regex --with-tokyocabinet --with-mixmaster --enable-pgp 
		make > make.log
		popd
		cp src/mutt-1.5.21/mutt build/gnu/mutt-jaro
		cp src/mutt-1.5.21/pgpewrap build/gnu/pgpewrap
	}

	{ test "$target" = "mixmaster" } && { 
		echo "Compiling Mixmaster (anonymous remailer)"
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



	echo "Done compiling."
	echo "Now run ./install.sh and Jaro Mail will be ready in ~/Mail"
	echo "or \"./install.sh path\" to install it somewhere else."
	;;
    

    fedora)


	echo "Checking software to install..."
	which zsh || sudo yum install zsh
	which mutt || sudo yum install mutt
	which procmail || sudo yum install procmail
	which msmtp || sudo yum install msmtp
	which pinentry || sudo yum install pinentry
	which fetchmail || sudo yum install fetchmail
	which wipe || sudo yum install wipe
	which abook || sudo yum install abook

	echo "Checking build dependencies"
	which gcc || sudo yum install gcc
	which bison || sudo yum install bison
	which flex || sudo yum install flex
	rpm -q glib2-devel || sudo yum install glib2-devel
	rpm -q libgnome-keyring-devel || sudo yum install libgnome-keyring-devel
	rpm -q bzip2-devel || sudo yum install bzip2-devel
	rpm -q zlib-devel || sudo yum install zlib-devel

	echo "All dependencies installed"
	cd src
	echo -n "Compiling the address parser... "


	echo "fetchaddr"
	gcc $cflags -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
	    gcc $cflags -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o

	echo "dotlock"
	gcc $cflags -c dotlock.c
	gcc $cflags -o dotlock dotlock.o
	cd - > /dev/null

	echo "Compiling the search engine..."
	cd src/mairix
	./configure
	make clean > /dev/null
	make > /dev/null
	cd - > /dev/null


	echo -n "Compiling the date parser... "
	cd src
	gcc $cflags -I mairix -c fetchdate.c
	gcc $cflags -DHAS_STDINT_H -DHAS_INTTYPES_H -DUSE_GZIP_MBOX \
	    -o fetchdate fetchdate.o \
	    mairix/datescan.o mairix/db.o mairix/dotlock.o \
	    mairix/expandstr.o mairix/glob.o mairix/md5.o \
	    mairix/nvpscan.o mairix/rfc822.o mairix/stats.o \
	    mairix/writer.o mairix/dates.o mairix/dirscan.o \
	    mairix/dumper.o mairix/fromcheck.o mairix/hash.o mairix/mbox.o \
	    mairix/nvp.o mairix/reader.o mairix/search.o mairix/tok.o \
	    -lz -lbz2
	echo "fetchdate"
	cd - > /dev/null

	cp src/mairix/mairix build/gnu/
	cp src/fetchaddr build/gnu/
	cp src/fetchdate build/gnu/
	cp src/dotlock build/gnu/

	echo "Compiling gnome-keyring"
	cd src/gnome-keyring
	gcc jaro-gnome-keyring.c \
	    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1` \
	    $cflags -o jaro-gnome-keyring
	cd - > /dev/null
	cp src/gnome-keyring/jaro-gnome-keyring build/gnu/
	strip build/gnu/*
	echo "Done compiling."
	echo "Now run ./install.sh and Jaro Mail will be ready in ~/Mail"
	echo "or \"./install.sh path\" to install it somewhere else."
	;;



    *)
	echo "Error: no distro recognized, build by hand."
	;;
esac

popd
