PREFIX ?= /usr/local
JARO_LIBEXEC := ${PREFIX}/share/jaromail
JARO_SHARE := ${PREFIX}/share/jaromail
srcdir := $(shell pwd)

all:
	./build/auto build

# { test -r $srcdir/src/fetchaddr } || {
# print "Error: first build, then install."; return 1 }
install:
	$(info Installing JaroMail in ${JARO_SHARE})
	@mkdir -p ${JARO_SHARE}/mutt ${JARO_SHARE}/stats
	@mkdir -p ${JARO_LIBEXEC}/bin ${JARP_LIBEXEC}/zlibs
	@chmod -R a+rX ${JARO_SHARE}
	@cp -r ${srcdir}/doc/* ${JARO_SHARE}/
	@cp -r ${srcdir}/src/mutt/* ${JARO_SHARE}/mutt/
	@cp -r ${srcdir}/src/stats/* ${JARO_SHARE}/stats/
	@cp ${srcdir}/src/jaro ${JARO_LIBEXEC}/bin
	@cp -r ${srcdir}/build/gnu/* ${JARO_LIBEXEC}/bin
	@cp -r ${srcdir}/src/zlibs/* ${JARO_LIBEXEC}/zlibs/
	@cp -r ${srcdir}/src/zuper/zuper* ${JARO_LIBEXEC}/zlibs
	@mkdir -p ${PREFIX}/bin
	@echo "#!/usr/bin/env zsh" > ${PREFIX}/bin/jaro
	@echo "export JAROWORKDIR=${JARO_SHARE}" >> ${PREFIX}/bin/jaro
	@echo "${JARO_SHARE}/bin/jaro \$${=@}" >> ${PREFIX}/bin/jaro
	@chmod +x ${PREFIX}/bin/jaro
	@echo "JaroMail succesfully installed"
	@echo "To initialize your Mail dir use: jaro init PATH"
	@echo "Default PATH is \$$HOME/Mail"

clean:
	rm -f src/*.o
	rm -f src/gpgewrap
	rm -f src/fetchaddr
	rm -f src/parsedate
	rm -f src/dotlock



