#!/usr/bin/env zsh

# jaromail install script
#
# Copyleft (C) 2010-2012 Denis Roio <jaromil@dyne.org>
#
# This source  code is free  software; you can redistribute  it and/or
# modify it under the terms of  the GNU Public License as published by
# the Free  Software Foundation; either  version 3 of the  License, or
# (at your option) any later version.
#
# This source code is distributed in  the hope that it will be useful,
# but  WITHOUT ANY  WARRANTY;  without even  the  implied warranty  of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# Please refer to the GNU Public License for more details.
#
# You should have received a copy of the GNU Public License along with
# this source code; if not, write to:
# Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

if ! [ -r src/jaro ]; then
    echo "Error: this script should be run from inside a jaromail software distribution"
    exit 1
fi

if [ $1 ]; then 
    MAILDIRS=$1;
else
    MAILDIRS=$HOME/Mail
fi

WORKDIR=$MAILDIRS/jaro
PROCMAILDIR=$WORKDIR/.procmail
MUTTDIR=$WORKDIR/.mutt

umask 007 # James Bond ;^)


source src/jaro

# make sure the directory is private
${=mkdir} $MAILDIRS
${=mkdir} $WORKDIR

notice "Installing Jaromail in $WORKDIR"

# install the main jaromail script
${=mkdir} ${WORKDIR}/bin
cp src/jaro ${WORKDIR}/bin

# make sure we have a temp and cache dir
${=mkdir} $WORKDIR/tmp $WORKDIR/cache

if ! [ -r $WORKDIR/Filters.txt ]; then
    cat <<EOF > $WORKDIR/Filters.txt
# Example filter configuration for Jaro Mail

# mailinglist filters are in order of importance
# syntax: to <list email> save <folder>

to	  crypto@lists.dyne	save	dyne.crypto
to	  dynebolic		save	dyne.dynebolic
to	  freej			save	dyne.freej
to	  frei0r-devel		save	dyne.frei0r
to	  taccuino		save	ml.freaknet
to	  deadpoets		save	ml.freaknet
to	  linux-libre		save	gnu.linux-libre
to	  foundations@lists	save	gnu.foundations
to	  debian-mentors	save	debian.mentors
to	  debian-blends		save	debian.blends
to	  freedombox-discuss	save	debian.freedombox

# other filters for web 2.0 services
# using folder names with a prefix. can facilitate
# folder maintainance.

from      identi.ca	        save	web.identica
from      Twitter		save	web.twitter
from      linkedin		save	web.linkedin
from      googlealerts		save	web.google
from      facebook		save	web.facebook
from      FriendFeed		save	web.friendfeed
from      academia.edu		save	web.academia

EOF
    act "Default filters created"
else
    error "Existing configuration $WORKDIR/Filters.txt skipped"
fi

if ! [ -r $WORKDIR/Accounts ]; then
    ${=mkdir} $WORKDIR/Accounts
    cat <<EOF > $WORKDIR/Accounts/README.txt
Directory containing account information

Each file contains a different account: imap, pop or gmail
each account contains all information needed to connect it

For example a file named imap.gmail.txt should contain:

----8<----8<----8<----8<----8<----8<----8<----8<----8<----
# Name and values are separated by spaces or tabs

# Name appearing in From: field
name Anonymous

host imap.gmail.com

login USERNAME@gmail.com

auth plain # or kerberos, etc

transport ssl

port 993

cert /path/to/cert

# the password field will be filled in automatically

----8<----8<----8<----8<----8<----8<----8<----8<----8<----

Or a file named smtp.gmail.txt should contain:

----8<----8<----8<----8<----8<----8<----8<----8<----8<----
# Name and values are separated by spaces or tabs

name USERNAME gmail

host smtp.gmail.com

login USERNAME@gmail.com

transport ssl # or "tls" or "plain"

port 465

----8<----8<----8<----8<----8<----8<----8<----8<----8<----

EOF
    act "Default accounts directory created"
else
    error "Existing configuration $WORKDIR/Accounts skipped"
fi

# procmail is entirely generated
# so overwriting it won't hurt
act "Installing procmail scripts"
${=mkdir} $PROCMAILDIR
cp -a share/procmail/* $PROCMAILDIR

# also mutt is safe to override
${=mkdir} $MUTTDIR
cp -a share/mutt/* $MUTTDIR

act "Installing little brother database"
# safe to override
${=mkdir} $WORKDIR/.lbdb
for aw in munge.awk.in munge-keeporder.awk.in tac.awk.in; do
	dst=`echo $aw | sed -e 's/.awk.in$//'`
	cat share/lbdb/$aw \
	| sed -e "s&@AWK@&`which awk`&g" \
	> $WORKDIR/.lbdb/$dst
done
for sh in lbdb-fetchaddr.sh.in lbdb-munge.sh.in lbdb_lib.sh.in lbdbq.sh.in; do
	dst=`echo $sh | sed -e 's/.sh.in$//'`
	cat share/lbdb/$sh \
	| sed -e "s&@SH@&/usr/bin/env zsh&g" \
	| sed -e "s&@DOTLOCK@&mutt_dotlock&g" \
	| sed -e "s&@LBDB_FILE&${WORKDIR}/.lbdb/m_inmail.list&g" \
	| sed -e "s&@LBDB_VERSION@&0.38-jaromail&g" \
	| sed -e "s&@prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@exec_prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@libdir@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@sysconfdir@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@MODULES@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@TAC@&${WORKDIR}/.lbdb/tac.awk&g" \
        > $WORKDIR/.lbdb/${dst}
done
lbdb_modules=(m_finger m_gpg m_inmail m_muttalias m_osx_addressbook m_vcf)
for mod in ${lbdb_modules}; do
	cat share/lbdb/${mod}.sh.in \
	| sed -e "s&@SH@&/usr/bin/env zsh&g" \
	| sed -e "s&@LBDB_VERSION@&0.38-jaromail&g" \
	| sed -e "s&@prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@exec_prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@libdir@&${WORKDIR}/.lbdb&g" \
	> $WORKDIR/.lbdb/${mod}
done
chmod +x $WORKDIR/.lbdb/*
ln -sf $WORKDIR/.lbdb/lbdb-fetchaddr $WORKDIR/bin/
ln -sf $WORKDIR/.lbdb/lbdbq $WORKDIR/bin/

case $OS in
    GNU)
	echo "METHODS=(m_inmail)" > ${WORKDIR}/.lbdb/lbdb.rc
	;;
    MAC)
	# use ABQuery on mac
	echo "METHODS=(m_inmail m_osx_addressbook)" > ${WORKDIR}/.lbdb/lbdb.rc
	;;
esac
####


# generate initial configuration
MAILDIRS=$MAILDIRS WORKDIR=$WORKDIR src/jaro update


case $OS in
	GNU)
	if [ -r build/osx ]; then
	    cp -a build/osx/* $WORKDIR/bin
	fi
	touch $HOME/.profile
	cat $HOME/.profile | grep '^# Jaro Mail' > /dev/null
	if [ $? != 0 ]; then
	    cat <<EOF >> $HOME/.profile
# Jaro Mail Installer addition on `date`
export PATH=$WORKDIR/bin:\$PATH
# Finished adapting your PATH
EOF
	fi
	;;
    MAC)
		# TODO
	;;
esac
	
notice "Installation completed" #, now edit your personal settings:"
case $OS in
	GNU)
	;;
	MAC)
	;;
#	open /Applications/TextEdit.app $WORKDIR/Configuration.txt
	*)
	;;
esac
