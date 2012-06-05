#!/usr/bin/env zsh 

# Jaro Mail install script
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


source src/jaro source

# make sure the directory is private
${=mkdir} $MAILDIRS
${=mkdir} $WORKDIR

notice "Installing Jaromail in $WORKDIR"
act "This script will create the ~/Mail folder if it doesn't exist,"
act "then populate it with default Maildirs and the Jaro Mail binaries."
act "At last, a shell execution PATH to ~/Mail/jaro/bin will be added"
act "to your ~/.profile so that you can call 'jaro' from a Terminal."

# install the main jaromail script
${=mkdir} ${WORKDIR}/bin
cp -f src/jaro ${WORKDIR}/bin

# make sure we have a temp and cache dir
${=mkdir} $WORKDIR/tmp $WORKDIR/cache

if ! [ -r $WORKDIR/Filters.txt ]; then
    cat <<EOF > $WORKDIR/Filters.txt
# Example filter configuration for Jaro Mail

# mailinglist filters are in order of importance
# syntax: to <list email> save <folder>
# below some commented out examples, note the use of a prefix,
# which makes it handy when browsing with file completion.

# to	  crypto@lists.dyne	save	dyne.crypto
# to	  dynebolic		save	dyne.dynebolic
# to	  freej			save	dyne.freej
# to	  frei0r-devel		save	dyne.frei0r
# to	  taccuino		save	ml.freaknet
# to	  deadpoets		save	ml.freaknet
# to	  linux-libre		save	gnu.linux-libre
# to	  foundations@lists	save	gnu.foundations
# to	  debian-mentors	save	debian.mentors
# to	  debian-blends		save	debian.blends
# to	  freedombox-discuss	save	debian.freedombox

# Other filters for web 2.0 using folder names with a prefix:
# they can facilitate folder maintainance.

# from      identi.ca	        save	web.identica
# from      Twitter		save	web.twitter
# from      linkedin		save	web.linkedin
# from      googlealerts	save	web.google
# from      facebook		save	web.facebook
# from      FriendFeed		save	web.friendfeed
# from      academia.edu	save	web.academia

EOF
    act "Default filters created"
else
    error "Existing configuration $WORKDIR/Filters.txt skipped"
fi

if ! [ -r $WORKDIR/Mutt.txt ]; then
    cat <<EOF > $WORKDIR/Mutt.txt
# Mutt specific customizations
# uncomment and fill in with your settings

# set locale=""                   # system default locale ("C")
# set signature='~/.signature'    # signature file
# set pgp_sign_as="0xC2B68E39"    # UserID/KeyID for signing

# Customized headers
# unmy_hdr *                      # remove all extra headers first.

# my_hdr From: Jaromil <jaromil@dyne.org>;
# my_hdr Organization: Dyne.org Foundation
# my_hdr X-GPG-Keyserver: pgp.mit.edu
# my_hdr X-GPG-Id: C2B68E39 [expires: 2013-09-25]
# my_hdr X-GPG-Fingerprint: B2D9 9376 BFB2 60B7 601F  5B62 F6D3 FBD9 C2B6 8E39
# my_hdr X-Face: %H:nE)m:Rl>Z?(C7EvRtuUJp4^f@d\#~4pB48~:1:EC)^&9EDcZaKL/*+10(P?g*N0>n8n3&\n kVzfAD\`+RofVAx~ew>FGQmmT7NqlSQx+M8LN5\`,h^aPF[Njx+A~%f!&VJu9!y:~ma/\'^@mvOr@}DyG\n @\"g\`kfy(vyRC

#############

# set attribution='On %{%a, %d %b %Y}, %n wrote:\n'
# set status_format="-%r-Mutt: %f [Msgs:%?M?%M/?%m%?n? New:%n?%?o? Old:%o?%?d? Del:%d?%?F? Flag:%F?%?t? Tag:%t?%?p? Post:%p?%?b? Inc:%b? %?l? %l?]---(%s/%S)-default-%>-(%P)---"

EOF
    act "Default Mutt configuration template created"
else
    error "Existing configuration $WORKDIR/Mutt.txt skipped"
fi

if ! [ -r $WORKDIR/Accounts ]; then
    ${=mkdir} $WORKDIR/Accounts
    cat <<EOF > $WORKDIR/Accounts/README.txt
Directory containing account information

Each file contains a different account: imap, pop or gmail
each account contains all information needed to connect it

Examples are: imap.default.txt and smtp.default.txt

One can have multiple accounts named otherwise than default
EOF
    cat <<EOF > $WORKDIR/Accounts/imap.default.txt
# Name and values are separated by spaces or tabs
# comments start the line with a hash

# Name appearing in From: field
name To Be Configured

# Email address (default is same as login)
email unknown@gmail.com

# Internet address
host imap.gmail.com

# Username
login USERNAME@gmail.com

# Authentication type
auth plain # or kerberos, etc

# Identity certificate: check or ignore
cert ignore

# Transport protocol
transport ssl

# Service port
port 993

# Options when fetching
# to empty your mailbox you can use: fetchall flush
# by default this is 'keep': don't delete mails from server
options keep

# Imap folders
# uncommend to provide a list of folders to be fetched
# folders INBOX, known, priv, lists, ml.unsorted, unsorted

#
# The password field will be filled in automatically
#
EOF
    cat <<EOF > $WORKDIR/Accounts/smtp.default.txt
# Name and values are separated by spaces or tabs
# comments start the line with a hash

# Name for this account
name To Be Configured

# Internet address
host smtp.gmail.com

# Username
login USERNAME@gmail.com

# Transport protocol
transport ssl # or "tls" or "plain"

# Service port
# port 465
port 25
EOF
    act "Default accounts directory created"
else
    error "Existing configuration $WORKDIR/Accounts skipped"
fi

# procmail is entirely generated
# so overwriting it won't hurt
act "Installing procmail scripts"
${=mkdir} $PROCMAILDIR
cp -a src/procmail/* $PROCMAILDIR

# also mutt is safe to override
${=mkdir} $MUTTDIR
cp -a src/mutt/* $MUTTDIR

act "Installing little brother database"
# safe to override
${=mkdir} $WORKDIR/.lbdb
for aw in munge.awk.in munge-keeporder.awk.in tac.awk.in; do
	dst=`echo $aw | sed -e 's/.awk.in$//'`
	cat src/lbdb/$aw \
	| sed -e "s&@AWK@&`which awk`&g" \
	> $WORKDIR/.lbdb/$dst
done
for sh in lbdb-fetchaddr.sh.in lbdb-munge.sh.in lbdb_lib.sh.in lbdbq.sh.in; do
	dst=`echo $sh | sed -e 's/.sh.in$//'`
	cat src/lbdb/$sh \
	| sed -e "s&@SH@&/usr/bin/env zsh&g" \
	| sed -e "s&@DOTLOCK@&${WORKDIR}/.lbdb/dotlock&g" \
	| sed -e "s&@LBDB_FILE@&${WORKDIR}/.lbdb/m_inmail.list&g" \
	| sed -e "s&@LBDB_VERSION@&0.38-jaromail&g" \
	| sed -e "s&@prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@exec_prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@libdir@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@sysconfdir@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@MODULES@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@TAC@&${WORKDIR}/.lbdb/tac&g" \
        | sed -e "s&@TMPDIR@&${WORKDIR}/tmp&g" \
        > $WORKDIR/.lbdb/${dst}
done
lbdb_modules=(m_finger m_gpg m_inmail m_muttalias m_osx_addressbook m_vcf)
for mod in ${lbdb_modules}; do
	cat src/lbdb/${mod}.sh.in \
	| sed -e "s&@SH@&/usr/bin/env zsh&g" \
	| sed -e "s&@LBDB_VERSION@&0.38-jaromail&g" \
	| sed -e "s&@prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@exec_prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@libdir@&${WORKDIR}/.lbdb&g" \
	> $WORKDIR/.lbdb/${mod}
done
cp src/lbdb/dotlock $WORKDIR/bin/
cp src/lbdb/fetchaddr $WORKDIR/bin/

# OS specific lbdb rules
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
	GNU|MAC)
	if [ -r build/osx ]; then
	    cp -a build/osx/* $WORKDIR/bin
	fi
	touch $HOME/.profile
	cat $HOME/.profile | grep '^# Jaro Mail' > /dev/null
	if [ $? != 0 ]; then
	    cat <<EOF >> $HOME/.profile
# Jaro Mail Installer addition on `date`
export PATH=$WORKDIR/bin:\$PATH
# Finished adapting your PATH for Jaro Mail environment
EOF
	fi
	;;
	*) ;;
esac
	
notice "Installation completed" #, now edit your personal settings:"
act "Configure your personal settings, accounts and filters in:"
act "    $WORKDIR"
act "Check the commandline help for a list of commands: jaro -h"

# OS specific post install rules
case $OS in
	GNU)
	cp src/gnome-keyring/jaro-gnome-keyring $WORKDIR/bin/
	;;
	MAC)
	;;
#	open /Applications/TextEdit.app $WORKDIR/Configuration.txt
	*)
	;;
esac
