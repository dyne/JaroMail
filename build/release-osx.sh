#!/usr/bin/env zsh
#
# small script to pack a JaroMail release for OSX
# by Jaromil @dyne.org

{ test "$1" = "" } && {
	echo "usage: $0 version"
	return 1 }

dir=JaroMail
ver=$1
out="`basename ${dir}`-${ver}.dmg"

echo "Making ${dir} release v$ver"

{ test -r ${dir} } || { mkdir ${dir} }

{ test "$ver" = "" } && {
	echo "error: version not specified"
	return 0 }

# creating the directory and populating it
mkdir -p ${dir}
{ test -r JaroMail.app/Contents } && {
	echo "updating to latest app build"
	rm -rf ${dir}/JaroMail.app
	mv JaroMail.app ${dir}/
}
echo "Source app: `du -hs ${dir}/JaroMail.app`"

v=README.txt; cp -v -f ../${v} ${dir}/${v}
v=COPYING.txt; cp -v -f ../${v} ${dir}/${v}
v=ChangeLog.txt; cp -v -f ../${v} ${dir}/${v}
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
