#!/bin/sh

cd ..

distro=unknown

cflags="-O2"

which apt-get && distro=debian
which yum && distro=fedora


# no other distro supported atm

case $distro in
    debian)
	mkdir -p build/gnu

	echo "Checking software to install..."
	which zsh || sudo apt-get install zsh
	which mutt || sudo apt-get install mutt
	which procmail || sudo apt-get install procmail
	which msmtp || sudo apt-get install msmtp
	which pinentry || sudo apt-get install pinentry-curses
	which fetchmail || sudo apt-get install fetchmail
	which wipe || sudo apt-get install wipe

	echo "Checking build dependencies"
	which gcc || sudo apt-get install gcc
	which bison || sudo apt-get install bison
	which flex || sudo apt-get install flex
	[ -r /usr/share/doc/libgnome-keyring-dev/copyright ] || \
	    sudo apt-get install libglib2.0-dev libgnome-keyring-dev

	echo "All dependencies installed"
	cd src
	echo -n "Compiling the address parser... "


	echo "fetchaddr"
	gcc $cflags -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
	    gcc $cflags -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o
	cd - > /dev/null

	echo "Compiling the search engine..."
	cd src/mairix
	./configure
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
	    -lz
	echo "fetchdate"
	cd - > /dev/null

	cp src/mairix/mairix build/gnu/
	cp src/fetchaddr build/gnu/
	cp src/fetchdate build/gnu/

	echo "Compiling gnome-keyring"
	cd src/gnome-keyring
	gcc jaro-gnome-keyring.c \
	    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1` \
	    $cflags -o jaro-gnome-keyring
	cd - > /dev/null
	cp src/gnome-keyring/jaro-gnome-keyring build/gnu/

	echo "Done compiling."
	echo "Now run ./install.sh and Jaro Mail will be ready in ~/Mail"
	echo "or \"./install.sh path\" to install it somewhere else."
	;;


    fedora)
	mkdir -p build/gnu

	echo "Checking software to install..."
	which zsh || sudo yum install zsh
	which mutt || sudo yum install mutt
	which procmail || sudo yum install procmail
	which msmtp || sudo yum install msmtp
	which pinentry || sudo yum install pinentry
	which fetchmail || sudo yum install fetchmail
	which wipe || sudo yum install wipe

	echo "Checking build dependencies"
	which gcc || sudo yum install gcc
	which bison || sudo yum install bison
	which flex || sudo yum install flex
	{ test -r /usr/share/doc/libgnome-keyring-3.2.0/COPYING } && {
	    sudo yum install glib2-devel libgnome-keyring-devel }

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
	    mairix/nvp.o mairix/reader.o mairix/search.o mairix/tok.o
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

	echo "Done compiling."
	echo "Now run ./install.sh and Jaro Mail will be ready in ~/Mail"
	echo "or \"./install.sh path\" to install it somewhere else."
	;;



    *)
	echo "Error: no distro recognized, build by hand."
	;;
esac
