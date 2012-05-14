#!/bin/sh

distro=unknown

which apt-get && distro=debian

# no other distro supported atm

case $distro in
    debian)
	echo "Checking software to install..."
	which zsh || sudo apt-get install zsh
	which mutt || sudo apt-get install mutt
	which procmail || sudo apt-get install procmail
	which msmtp || sudo apt-get install msmtp
	which pinentry || sudo apt-get install pinentry
	which fetchmail || sudo apt-get install fetchmail
	which wipe || sudo apt-get install wipe
	echo "All dependencies installed"
	echo -n "Compiling a few sources... "
	cd share/lbdb
	echo -n "dotlock "
	[ -x dotlock ] || gcc -O2 -o dotlock dotlock.c
	echo "fetchaddr"
	[ -x fetchaddr ] || \
	    gcc -O2 -c fetchaddr.c helpers.c rfc2047.c rfc822.c; \
	    gcc -O2 -o fetchaddr fetchaddr.o helpers.o rfc2047.o rfc822.o; \
	cd - > /dev/null
	echo "Done compiling."
	echo "Now run ./install.sh and jaromail will be ready in ~/Mail/jaro"
	;;

    *)
	echo "Error: no distro recognized, build by hand."
	return 1
	;;
esac

