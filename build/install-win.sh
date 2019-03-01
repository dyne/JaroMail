#!/usr/bin/env zsh

PREFIX=${PREFIX:-$HOME/Mail}

# TODO: separate libexec from share
JARO_LIBEXEC=$HOME/Mail/system
JARO_SHARE=$HOME/Mail
mkdir -p $HOME/Mail
touch $JARO_SHARE || {
    print "Error: cannot write to $JARO_SHARE"
    print "Run as root or set PREFIX to another location"
    return 1
}

# cleanup previous installs
rm -rf "$JARO_SHARE"
mkdir -p "$JARO_SHARE"

{ test $? = 0 } || {
    print "No permissions to install system-wide."; return 1 }

srcdir=.

{ test -r ./src/fetchaddr } || {
    print "Error: first build, then install."; return 1 }

mkdir -p $JARO_SHARE/{.mutt,.stats}
cp -ra $srcdir/doc/* $JARO_SHARE/
cp -ra $srcdir/src/mutt/* $JARO_SHARE/.mutt/
cp -ra $srcdir/src/stats/* $JARO_SHARE/.stats/

# copy the executables
mkdir -p $JARO_LIBEXEC/{bin,zlibs}
cp $srcdir/src/jaro $JARO_LIBEXEC/bin
cp -ra $srcdir/build/win/* $JARO_LIBEXEC/bin
cp -ra $srcdir/src/zlibs/* $JARO_LIBEXEC/zlibs/
cp -ra $srcdir/doc/jaromail-manual.pdf $JARO_LIBEXEC/

for l in `ls $JARO_LIBEXEC/zlibs/ | grep '.zwc$'`; do
    rm -f $l
done

for l in `ls $JARO_LIBEXEC/zlibs/ | grep -v '.zwc$'`; do
    zcompile $JARO_LIBEXEC/zlibs/$l
done

# fix pinentry w32 path
[[ -r /usr/bin/pinentry ]] || {
    ln -sf /usr/bin/pinentry-w32 /usr/bin/pinentry
}

mkdir -p /usr/local/bin
rm -f /usr/local/bin/jaro
cat <<EOF > /usr/local/bin/jaro
#!/usr/bin/env zsh
export JAROWORKDIR=${JARO_LIBEXEC}
export JAROMAILDIR=${PREFIX}
${JARO_LIBEXEC}/bin/jaro \$*
EOF
chmod +x /usr/local/bin/jaro

source ${JARO_LIBEXEC}/bin/jaro source

jaro init

notice "Jaro Mail succesfully installed in: $PREFIX"
act "Executable path: /usr/local/bin/jaro"

return 0
