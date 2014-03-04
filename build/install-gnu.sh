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

cp -ra $srcdir/doc/* $JARO_SHARE/
cp -ra $srcdir/src/procmail $JARO_SHARE/.procmail
cp -ra $srcdir/src/mutt $JARO_SHARE/.mutt
cp -ra $srcdir/src/stats $JARO_SHARE/.stats

{ test -r $srcdir/build/gnu/fetchaddr } || {
    print "Error: first build, then install."; return 1 }

# copy the executables
cp -ra $srcdir/build/gnu/* $JARO_LIBEXEC/
cp -ra $srcdir/src/zlibs $JARO_LIBEXEC/
cp $srcdir/src/jaro $JARO_LIBEXEC/

cat <<EOF > $PREFIX/bin/jaro
#!/usr/bin/env zsh
export JAROWORKDIR=${JARO_SHARE}
${JARO_SHARE}/jaro \${=@}
EOF
chmod +x $PREFIX/bin/jaro