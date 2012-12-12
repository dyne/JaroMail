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

{ test -r src/jaro } || {
    echo "Error: this script should be run from inside a Jaro Mail source distribution"
    exit 1 }



if [ $1 ]; then
    MAILDIRS=$1;
else
    MAILDIRS=$HOME/Mail
fi

WORKDIR=$MAILDIRS/jaro
PROCMAILDIR=$WORKDIR/.procmail
MUTTDIR=$WORKDIR/.mutt

source src/jaro source

bin=1
{ test -r src/fetchaddr } || {
    error "Jaro Mail was not built yet."
    error "Only scripts will be installed or updated"
    error "Look into the build/ directory for scripts to build binaries."
    bin=0
}

# make sure the directory is private
${=mkdir} $MAILDIRS
${=mkdir} $WORKDIR

notice "Installing Jaromail in $WORKDIR"
act "This script will create the ~/Mail folder if it doesn't exist,"
act "then populate it with default Maildirs and the Jaro Mail binaries."

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

# from      identi.ca		save	web.identica
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

if ! [ -r $WORKDIR/Applications.txt ]; then
    cat <<EOF > $WORKDIR/Applications.txt
# Example configuration to match mime/type to applications
# each line should start with a mime/type and then indicate an executable

# Example:
# application/rtf  oowriter

EOF
    act "Default helper applications settings created"
else
    error "Existing configuration $WORKDIR/Applications.txt skipped"
fi


if ! [ -r $WORKDIR/Mutt.txt ]; then
    cat <<EOF > $WORKDIR/Mutt.txt
# Mutt specific customizations
# uncomment and fill in with your settings

## dark background (uncomment to switch)
# source \$HOME/Mail/jaro/.mutt/colors-solarized-dark-256
# source \$HOME/Mail/jaro/.mutt/colors-solarized-dark-16

## light background (uncomment to switch)
# source \$HOME/Mail/jaro/.mutt/colors-solarized-light-256
# source \$HOME/Mail/jaro/.mutt/colors-solarized-light-16

# set locale=""                   # system default locale ("C")
# set signature='~/.signature'    # signature file
# set pgp_sign_as="0xC2B68E39"    # UserID/KeyID for signing

# Customized headers example
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

# Aliases also received on this mail
# alias mimesis@gmail.com
# alias nemesis@gmail.com

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
# uncomment to provide a list of folders to be fetched
# folders INBOX, known, priv, lists, ml.unsorted, unsorted

# Remote sieve
# command to upload a sieve filter to the server
# %% will be filled in automatically with our file
# remote_sieve_cmd scp %% assata.dyne.org:/var/mail/sieve-scripts/email


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

# our own libraries
act "Compiling Jaro Mail ZLibs"
{ test -d ${WORKDIR}/zlibs } && { rm -f $WORKDIR/zlibs/* }
${=mkdir} $WORKDIR/zlibs
cp -a src/zlibs/* $WORKDIR/zlibs
for z in `find $WORKDIR/zlibs -type f`; do
    zcompile -R ${z}
done

# procmail is entirely generated
# so overwriting it won't hurt
act "Installing procmail scripts"
${=mkdir} $PROCMAILDIR
cp -a src/procmail/* $PROCMAILDIR

# also mutt is safe to override
${=mkdir} $MUTTDIR
cp -a src/mutt/* $MUTTDIR

# all statistics
${=mkdir} $WORKDIR/.stats
cp -a src/stats/* $WORKDIR/.stats

{ test $bin = 1 } && {
    cp src/fetchaddr $WORKDIR/bin/
    { test -r src/fetchdate } && {
	# fetchdate is only optionally used for stats
	cp src/fetchdate $WORKDIR/bin/ }

    case $OS in
	MAC)
	    cp -r build/osx/* $WORKDIR/bin
	    ;;

	GNU) cp -a build/gnu/* $WORKDIR/bin
#rm -f $WORKDIR/bin/dotlock
#cat <<EOF > $WORKDIR/bin/dotlock
#!/usr/bin/env zsh
#mutt_dotlock \${=@}
#EOF
#chmod a+x $WORKDIR/bin/dotlock
	    ;;
    esac
}

# generate initial configuration
MAILDIRS=$MAILDIRS WORKDIR=$WORKDIR src/jaro update -q

touch $HOME/.profile
cat $HOME/.profile | grep '^# Jaro Mail' > /dev/null
if [ $? != 0 ]; then
    cat <<EOF >> $HOME/.profile
# Jaro Mail Installer addition on `date`
export PATH=$WORKDIR/bin:\$PATH
# Finished adapting your PATH for Jaro Mail environment
EOF
fi

# update the manual
{ test -r doc/jaromail-manual.pdf } && {
    cp -f doc/jaromail-manual.pdf $WORKDIR/Manual.pdf }

notice "Done! now configure your personal settings, accounts and filters in:"
act "    $WORKDIR"
act "To read the commandline help, with a list of commands: jaro -h"
act "Make sure jaro is in your PATH! it was just added to your ~/.profile"
act "    $WORKDIR/bin"


# OS specific post install rules
case $OS in
    GNU) ;;
    MAC)
	# import addressbook
	notice "Importing addressbook"
	import_macosx
	notice "Installation done, opening filemanager on config file dir."
	open $WORKDIR ;;
    *)
	;;
esac
