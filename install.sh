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



JAROMAILDIR=${1-$HOME/Mail}

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
${=mkdir} $MAILDIRS/tmp $MAILDIRS/cache

if ! [ -r $MAILDIRS/Filters.txt ]; then
    cp -v doc/Filters.txt $MAILDIRS/Filters.txt
    act "Default filters created"
else
    error "Existing configuration $MAILDIRS/Filters.txt skipped"
fi

if ! [ -r $MAILDIRS/Applications.txt ]; then
    cp -v doc/Applications.txt $MAILDIRS/Applications.txt
    act "Default helper applications settings created"
else
    error "Existing configuration $MAILDIRS/Applications.txt skipped"
fi


if ! [ -r $MAILDIRS/Mutt.txt ]; then
    cp -v doc/Mutt.txt $MAILDIRS/Mutt.txt
    act "Default Mutt configuration template created"
else
    error "Existing configuration $MAILDIRS/Mutt.txt skipped"
fi

if ! [ -r $MAILDIRS/Accounts ]; then
    cp -v -r doc/Accounts $MAILDIRS/Accounts
    act "Default accounts directory created"
else
    error "Existing configuration $MAILDIRS/Accounts skipped"
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
${=mkdir} $WORKDIR/.procmail
cp -a src/procmail/* $WORKDIR/.procmail

# also mutt is safe to override
${=mkdir} $WORKDIR/.mutt
cp -a src/mutt/* $WORKDIR/.mutt

# all statistics
${=mkdir} $WORKDIR/.stats
cp -a src/stats/* $WORKDIR/.stats

{ test "$bin" = "1" } && {
    cp -v src/fetchaddr $WORKDIR/bin/
    { test -r src/fetchdate } && {
	# fetchdate is only optionally used for stats
	cp -v src/fetchdate $WORKDIR/bin/ }

    case $OS in
	MAC)
	    cp -v -r build/osx/* $WORKDIR/bin
	    ;;

	GNU) cp -v -a build/gnu/* $WORKDIR/bin
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
act "Refresh configuration"
JAROMAILDIR=$MAILDIRS JAROWORKDIR=$WORKDIR src/jaro update -q

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
    cp -f doc/jaromail-manual.pdf $MAILDIRS/Manual.pdf }

notice "Done! now configure your personal settings, accounts and filters in:"
act "    $MAILDIRS"
act "Make sure jaro is in your PATH! binaries are found in:"
act "    $WORKDIR/bin"
act "For a brief list of commands: jaro -h"
act "For a complete introduction read the manual in $MAILDIRS/Manual.pdf"


