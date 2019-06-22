#!/usr/bin/env zsh
#
# small script to pack a JaroMail release for OSX
# by Jaromil @dyne.org

{ test "$1" = "" } && {
	echo "usage: $0 version"
	return 1 }

dir=JaroMail
ver="$1"



out="`basename ${dir}`-${ver}.dmg"

echo "Making ${dir} release v$ver"

{ test -r ${dir} } || { mkdir ${dir} }

{ test "$ver" = "" } && {
	echo "error: version not specified"
	return 0 }

appdst=JaroMail.app
WORKDIR=${appdst}/Contents/Resources/jaro
bindst=${WORKDIR}/bin

mkdir -p $WORKDIR/{bin,zlibs,.mutt,.stats}

# copying inside the fresh scripts
print "Compiling Jaro Mail ZLibs"
{ test -d ${WORKDIR}/zlibs } && { rm -f $WORKDIR/zlibs/* }
cp ../src/zlibs/* $WORKDIR/zlibs/
for z in `find $WORKDIR/zlibs -type f`; do
    zcompile -R ${z}
done


cp -r ../src/mutt/*     $WORKDIR/.mutt/
cp -r ../src/stats/*    $WORKDIR/.stats/
cp ../doc/Applications.txt $WORKDIR/
cp ../doc/Filters.txt      $WORKDIR/
cp ../doc/Aliases.txt      $WORKDIR/
cp ../doc/jaromail-manual.pdf     $WORKDIR/jaromail-manual.pdf
cp ../doc/JaroMail.icns	   $appdst/Contents/Resources
mkdir -p $WORKDIR/Accounts
cp -r ../doc/Accounts/*      $WORKDIR/Accounts/


print "Copying binaries and adjusting relocations"
mkdir -p $bindst
cp ../src/jaro $bindst
cp -v osx/* $bindst
mkdir -p $appdst/Contents/Frameworks/
cp -v osx/dylib/* $appdst/Contents/Frameworks/


# setting up the OSX app wrappers
mkdir -p $appdst/Contents/MacOS
cat <<EOF > $appdst/Contents/MacOS/jaroenv.sh
#!/usr/bin/env zsh
export PATH="/Applications/JaroMail.app/Contents/Resources/jaro/bin:\$PATH"
export GNUPGHOME="\$HOME/.gnupg"
export JAROMAILDIR="\$HOME/Library/Application Support/JaroMail"
export JAROWORKDIR="/Applications/JaroMail.app/Contents/Resources/jaro"
export TERMINFO="/usr/share/terminfo"
EOF

cat <<EOF > $appdst/Contents/MacOS/jaroshell.sh
#!/usr/bin/env zsh
. /Applications/JaroMail.app/Contents/MaxOS/jaroenv.sh
pushd "\$JAROMAILDIR"
clear
print "Welcome to Jaro Mail"
print type 'jaro help' for a list of commands"
jaro init
EOF
chmod +x $appdst/Contents/MacOS/jaroshell.sh

cat <<EOF > $appdst/Contents/MacOS/JaroMail.command
#!/usr/bin/env zsh
# JaroMail startup script for Mac .app
# Copyright (C) 2012-2014 by Denis Roio <Jaromil@dyne.org>
# GNU GPL V3 (see COPYING)
osascript <<EOF
tell application "Finder"
    set _home to system attribute "HOME"
    set maildirs to _home & "/Library/Application Support/JaroMail"
    make new Finder window to POSIX file maildirs
    activate
end tell
tell application "System Events" to set terminalOn to (exists process "Terminal")
if application "Terminal" is not running then
	tell application "Terminal" to activate
end if
tell application "Terminal"
   if (terminalOn) then
	do script "source /Applications/JaroMail.app/Contents/MacOS/jaroshell.sh"
   else
	do script "source /Applications/JaroMail.app/Contents/MacOS/jaroshell.sh" in front window
	set custom title of front window to "Jaro Mail"
   end if
end tell
EOF
echo "EOF" >> $appdst/Contents/MacOS/JaroMail.command

chmod +x $appdst/Contents/MacOS/JaroMail.command

cat <<EOF > $appdst/Contents/PkgInfo
APPLJAROMAIL
EOF

cat <<EOF > $appdst/Contents/Info.plist
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
	<string>JaroMail.command</string>
	<key>CFBundleIconFile</key>
	<string>JaroMail.icns</string>
	<key>CFBundleIdentifier</key>
	<string>org.dyne.jaromail</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>JAROMAIL</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>CFBundleVersion</key>
	<string>1.0</string>
	<key>CSResourcesFileMapped</key>
	<true/>
</dict>
</plist>
EOF




# creating the directory and populating it
mkdir -p ${dir}
{ test -r JaroMail.app/Contents } && {
	echo "updating to latest app build"
	rm -rf ${dir}/JaroMail.app
	cp -r JaroMail.app ${dir}/
}
echo "Source app: `du -hs ${dir}/JaroMail.app`"

v=README; cp -v -f ../${v}.md ${dir}/${v}.txt

v=COPYING; cp -v -f ../${v}.txt ${dir}/${v}.txt
v=ChangeLog; cp -v -f ../${v}.md ${dir}/${v}.txt
v=jaromail-manual.pdf; cp -v -f ../doc/${v} ${dir}/${v}
ln -sf /Applications ${dir}/Applications

echo "Generating release: $out"
echo "Source dir: `du -hs $dir`"
echo "proceed? (y|n)"
read -q ok
echo
{ test "$ok" = y } || {
	echo "operation aborted."
	return 0 }

rm -f ${out}
hdiutil create -fs HFS+ -volname "$dir $ver" -srcfolder \
	${dir} "${out}"

{ test $? = 0 } || {
	echo "error creating image: ${out}"
	return 0 }

ls -lh ${out}

hdiutil internet-enable -yes ${out}

rm -f ${out}.sha
shasum ${out} > ${out}.sha

echo "sign release? (y|n)"
read -q ok
echo
{ test "$ok" = y } || {
	echo "done."
	return 1 }
gpg -b -a ${out}.sha
