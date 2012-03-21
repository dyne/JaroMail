#!/usr/bin/env zsh

# Postino install script

if ! [ -r src/postino ]; then
    echo "Error: this script should be run from inside a postino software distribution"
    exit 1
fi


WORKDIR=$HOME/.postino
MAILDIRS=$HOME/Mail

umask 007 # James Bond ;^)

if [ $1 ]; then WORKDIR=$1; fi
# make sure the directory is private
mkdir -p $WORKDIR
mkdir -p $MAILDIRS

source src/postino

notice "Installing Postino in $WORKDIR"

if [ $? != 0 ]; then
    error "Postino directory $WORKDIR is not private, set its permissions to 700"
    error "Refusing to proceed."
    exit 1
fi

# install the main postino script
mkdir -p ${WORKDIR}/bin
cp src/postino ${WORKDIR}/bin

# make sure we have a temp and cache dir
mkdir -p $WORKDIR/tmp $WORKDIR/cache

if ! [ -r $MAILDIRS/Configuration.txt ]; then
    cat <<EOF > $MAILDIRS/Configuration.txt
# Name appearing in From: field
FULLNAME="Anonymous"

# MAIL USER (the left and right parts of an email)
USER=username
# @
DOMAIN=gmail.com

# IMAP (RECEIVE)
IMAP_ADDRESS=imap.gmail.com
IMAP_LOGIN=\${USER}@\${DOMAIN}

# SMTP (SEND)
SMTP_ADDRESS=smtp.gmail.com
SMTP_LOGIN=\${USER}@\${DOMAIN}
SMTP_PORT=465

# SMTP_CERTIFICATE=gmail.pem
# LOCAL FILES
# to change the location of this directory,
# export POSTINO_DIR as env var
# the defaults below should be ok, they place
# mutt, procmail, mstmp and other confs in ~/.postino

MAILDIRS=$MAILDIRS
MUTTDIR=$WORKDIR/.mutt
PROCMAILDIR=$WORKDIR/.procmail
CERTIFICATES=$HOME/.ssl/certs

# directory of the sieve filter
# REMOTE_FILTER=/var/mail/...
EOF
    act "Default configuration created"
else
    error "Existing $MAILDIRS/Configuration.txt skipped"
fi

# source the default configuration
source $MAILDIRS/Configuration.txt

if ! [ -r $MAILDIRS/Filters.txt ]; then
    cat <<EOF > $MAILDIRS/Filters.txt
# Example filter configuration for Postino

# accepted email addresses
to	  jaromil@dyne.org	save	priv
to	  jaromil@kyuzz.org	save	priv
to	  jaromil@enemy.org	save	priv
to	  jaromil@montevideo.nl	save	priv

# mailinglist filters, in order of importance
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
    error "Existing configuration $MAILDIRS/Filters.txt skipped"
fi

source $MAILDIRS/Configuration.txt

# procmail is entirely generated
# so overwriting it won't hurt
act "Installing procmail scripts"
mkdir -p $PROCMAILDIR
cp -a share/procmail/* $PROCMAILDIR

# also mutt is safe to override
mkdir -p $MUTTDIR
cp -a share/mutt/* $MUTTDIR

act "Installing little brother database"
# safe to override
mkdir -p $WORKDIR/.lbdb
for aw in munge.awk.in munge-keeporder.awk.in tac.awk.in; do
	dst=`echo $aw | sed -e 's/.awk.in$//'`
	cat share/lbdb/$aw \
	| sed -e "s&@AWK@&/usr/bin/env awk&g" \
	> $WORKDIR/.lbdb/$dst
done
for sh in lbdb-fetchaddr.sh.in lbdb-munge.sh.in lbdb_lib.sh.in lbdbq.sh.in; do
	dst=`echo $sh | sed -e 's/.sh.in$//'`
	cat share/lbdb/$sh \
	| sed -e "s&@SH@&/usr/bin/env zsh&g" \
	| sed -e "s&@LBDB_VERSION@&0.38-postino&g" \
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
	| sed -e "s&@LBDB_VERSION@&0.38-postino&g" \
	| sed -e "s&@prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@exec_prefix@&${WORKDIR}/.lbdb&g" \
	| sed -e "s&@libdir@&${WORKDIR}/.lbdb&g" \
	> $WORKDIR/.lbdb/${mod}
done
chmod +x $WORKDIR/.lbdb/*
ln -sf $WORKDIR/.lbdb/lbdb-fetchaddr $WORKDIR/bin/
ln -sf $WORKDIR/.lbdb/lbdbq $WORKDIR/bin/
cp share/lbdb/lbdb.rc.in ${WORKDIR}/.lbdb/lbdb.rc
ln -sf $WORKDIR/.lbdb $HOME/
####


# generate initial configuration
src/postino update


case `uname -s` in
	Darwin)
		if [ -r build/osx ]; then
			cp -a build/osx/* $WORKDIR/bin
		fi
		touch $HOME/.profile
		cat $HOME/.profile | grep '^# Postino' > /dev/null
		if [ $? != 0 ]; then
			cat <<EOF >> $HOME/.profile
# Postino Installer addition on `date`
export PATH=$WORKDIR/bin:\$PATH
# Finished adapting your PATH
EOF
		fi
	;;
	Linux)
		# TODO
	;;
esac
	
notice "Installation completed, now edit your personal settings:"
act "$MAILDIRS/Configuration.txt"
if [ `uname -s` = Darwin ]; then
	open /Applications/TextEdit.app $MAILDIRS/Configuration.txt
fi
