#!/bin/sh

cd ..

distro=unknown

which apt-get && distro=debian

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
	which gcc || sudo apt-get install bison
	which bison || sudo apt-get install bison
	which flex || sudo apt-get install bison
	[ -r /usr/share/doc/libgnome-keyring-dev/copyright ] || \
	    sudo apt-get install libglib2.0-dev libgnome-keyring-dev

	echo "All dependencies installed"

	echo -n "Compiling the address parser... "

	cd src
	echo -n "fetchaddr "
	[ -x fetchaddr ] || \
	    gcc -Os -c -static fetchaddr.c helpers.c rfc2047.c rfc822.c; \
	    gcc -Os -static -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o

	cd - > /dev/null

	cp src/fetchaddr build/gnu/
	echo
	echo "Compiling the search engine..."
	cd src/mairix
	./configure
	make > /dev/null
	cd - > /dev/null
	cp src/mairix/mairix build/gnu/

	echo "Compiling gnome-keyring"
	cd src/gnome-keyring
	gcc jaro-gnome-keyring.c \
	    `pkg-config --cflags --libs glib-2.0 gnome-keyring-1` \
	    -O2 -o jaro-gnome-keyring
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
