#!/usr/bin/env zsh

{ test "$PREFIX" = "" } && { PREFIX=/usr/local }

# TODO: separate libexec from share
JARO_LIBEXEC=$PREFIX/share/jaromail
JARO_SHARE=$PREFIX/share/jaromail

mkdir -p $JARO_SHARE
{ test $? = 0 } || {
    print "No permissions to install system-wide."; return 1 }

mkdir -p $JARO_LIBEXEC

{ test -r doc } && { srcdir=. }
{ test -r install-gnu.sh } && { srcdir=.. }

{ test -r $srcdir/src/fetchdate } || {
    print "Error: first build, then install."; return 1 }

mkdir -p $JARO_SHARE/{.mutt,.stats}
cp -ra $srcdir/doc/* $JARO_SHARE/
cp -ra $srcdir/src/mutt/* $JARO_SHARE/.mutt/
cp -ra $srcdir/src/stats/* $JARO_SHARE/.stats/

# copy the executables
mkdir -p $JARO_LIBEXEC/{bin,zlibs}
cp $srcdir/src/jaro $JARO_LIBEXEC/bin
cp -ra $srcdir/build/gnu/* $JARO_LIBEXEC/bin
cp -ra $srcdir/src/zlibs/* $JARO_LIBEXEC/zlibs/
for l in `ls $JARO_LIBEXEC/zlibs/ | grep -v '.zwc$'`; do
    zcompile $JARO_LIBEXEC/zlibs/$l
done

mkdir -p $PREFIX/bin
cat <<EOF > $PREFIX/bin/jaro
#!/usr/bin/env zsh
export JAROWORKDIR=${JARO_SHARE}
${JARO_SHARE}/bin/jaro \${=@}
EOF
chmod +x $PREFIX/bin/jaro
