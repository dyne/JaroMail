#!/usr/bin/env zsh

PREFIX=${PREFIX:-/usr/local}
target=gnu
os=`uname -s`
case $os in
    Cygwin) target=win ;;
    Linux) target=gnu ;;
esac

# TODO: separate libexec from share
JARO_SHARE_DIR=$PREFIX/share
JARO_LIBEXEC=$JARO_SHARE_DIR/jaromail
JARO_SHARE=$JARO_SHARE_DIR/jaromail
mkdir -p $JARO_SHARE_DIR || {
    print "Error: cannot create $PREFIX or $JARO_SHARE_DIR"
    print "Run as root or set PREFIX to another location"
    return 1
}
touch $JARO_SHARE || {
    print "Error: cannot write to $JARO_SHARE"
    print "Run as root or set PREFIX to another location"
    return 1
}

print "Installing in $JARO_SHARE"

# cleanup previous installs
rm -rf "$JARO_SHARE"
mkdir -p "$JARO_SHARE"

{ test $? = 0 } || {
    print "No permissions to install system-wide."; return 1 }

{ test -r doc } && { srcdir=. }
{ test -r install-${target}.sh } && { srcdir=.. }

{ test -r $srcdir/src/fetchaddr } || {
    print "Error: first build, then install."; return 1 }

mkdir -p $JARO_SHARE/{.mutt,.stats}
cp -r $srcdir/doc/* $JARO_SHARE/
cp -r $srcdir/src/mutt/* $JARO_SHARE/.mutt/
cp -r $srcdir/src/stats/* $JARO_SHARE/.stats/

# copy the executables
mkdir -p $JARO_LIBEXEC/{bin,zlibs}
cp $srcdir/src/jaro $JARO_LIBEXEC/bin
cp -r $srcdir/build/${target}/* $JARO_LIBEXEC/bin
cp -r $srcdir/src/zlibs/* $JARO_LIBEXEC/zlibs/
cp -r $srcdir/src/zuper/{zuper,zuper.init} $JARO_LIBEXEC/zlibs

for l in `ls $JARO_LIBEXEC/zlibs/ | grep '.zwc$'`; do
    rm -f $l
done

for l in `ls $JARO_LIBEXEC/zlibs/ | grep -v '.zwc$'`; do
    zcompile $JARO_LIBEXEC/zlibs/$l
done

chmod -R a+rX "$JARO_SHARE"

mkdir -p $PREFIX/bin
cat <<EOF > $PREFIX/bin/jaro
#!/usr/bin/env zsh
export JAROWORKDIR=${JARO_SHARE}
EOF

# zmodload zsh/pcre
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

[[ "$PREFIX" =~ "^/usr" ]] || $PREFIX/bin/jaro init
print "Jaro Mail succesfully installed in: $PREFIX"
print "Executable path: $PREFIX/bin/jaro"
[[ "$PREFIX" =~ "^/usr" ]] && {
    print "To initialize your Mail dir use: jaro init"
    print "Default is $HOME/Mail"
    print "Change it via environment variable JAROMAILDIR"
}
return 0
