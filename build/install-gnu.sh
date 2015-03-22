#!/usr/bin/env zsh

PREFIX=${PREFIX:-/usr/local}

# TODO: separate libexec from share
JARO_LIBEXEC=$PREFIX/share/jaromail
JARO_SHARE=$PREFIX/share/jaromail
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

{ test -r doc } && { srcdir=. }
{ test -r install-gnu.sh } && { srcdir=.. }

{ test -r $srcdir/src/fetchaddr } || {
    print "Error: first build, then install."; return 1 }

mkdir -p $JARO_SHARE/{.mutt,.stats}
cp -ra $srcdir/doc/* $JARO_SHARE/
cp -ra $srcdir/src/mutt/* $JARO_SHARE/.mutt/
cp -ra $srcdir/src/stats/* $JARO_SHARE/.stats/

# copy the executables
mkdir -p $JARO_LIBEXEC/{bin,zlibs}
cp $srcdir/src/jaro $JARO_LIBEXEC/bin
cp -ra $srcdir/build/gnu/* $JARO_LIBEXEC/bin
cp -r $srcdir/src/zlibs/* $JARO_LIBEXEC/zlibs/

for l in `ls $JARO_LIBEXEC/zlibs/ | grep '.zwc$'`; do
    rm -f $l
done

for l in `ls $JARO_LIBEXEC/zlibs/ | grep -v '.zwc$'`; do
    zcompile $JARO_LIBEXEC/zlibs/$l
done

mkdir -p $PREFIX/bin
cat <<EOF > $PREFIX/bin/jaro
#!/usr/bin/env zsh
export JAROWORKDIR=${JARO_SHARE}
EOF

zmodload zsh/pcre
# if not installed system-wide then place the Mail into prefix
[[ "$PREFIX" =~ "^/usr" ]] || {
    cat <<EOF >> $PREFIX/bin/jaro
export JAROMAILDIR=${PREFIX}
EOF
}
cat <<EOF >> $PREFIX/bin/jaro
${JARO_SHARE}/bin/jaro \${=@}
EOF
chmod +x $PREFIX/bin/jaro

source ${JARO_SHARE}/bin/jaro source

[[ "$PREFIX" =~ "^/usr" ]] || $PREFIX/bin/jaro init
notice "Jaro Mail succesfully installed in: $PREFIX"
act "Executable path: $PREFIX/bin/jaro"
[[ "$PREFIX" =~ "^/usr" ]] && {
    notice "To initialize your Mail dir use: jaro init"
    act "Default is $HOME/Mail"
    act "Change it via environment variable JAROMAILDIR"
}
return 0
