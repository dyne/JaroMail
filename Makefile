PREFIX ?= /usr/local
JARO_LIBEXEC := ${PREFIX}/share/jaromail
JARO_SHARE := ${PREFIX}/share/jaromail
MAN1DIR := ${PREFIX}/share/man/man1
srcdir := $(shell pwd)

all:
	./build/auto build

man:
	@pandoc -s -t man \
		--metadata title="jaromail" --metadata section="1" \
		--metadata date="$$(date +%Y-%m-%d)" \
		${srcdir}/doc/jaromail-manual.md -o ${srcdir}/doc/jaromail.1
	@pandoc -s -t man \
		--metadata title="jaro" --metadata section="1" \
		--metadata date="$$(date +%Y-%m-%d)" \
		${srcdir}/doc/jaro.1.md -o ${srcdir}/doc/jaro.1

# { test -r $srcdir/src/fetchaddr } || {
# print "Error: first build, then install."; return 1 }
install: man
	$(info Installing JaroMail in ${JARO_SHARE})
	@mkdir -p ${JARO_SHARE}/mutt ${JARO_SHARE}/stats
	@mkdir -p ${JARO_LIBEXEC}/bin ${JARO_LIBEXEC}/zlibs
	@mkdir -p ${MAN1DIR}
	@chmod -R a+rX ${JARO_SHARE}
	@cp -r ${srcdir}/doc/* ${JARO_SHARE}/
	@cp -r ${srcdir}/src/mutt/* ${JARO_SHARE}/mutt/
	@cp -r ${srcdir}/src/stats/* ${JARO_SHARE}/stats/
	@cp ${srcdir}/doc/jaromail.1 ${MAN1DIR}/jaromail.1
	@cp ${srcdir}/doc/jaro.1 ${MAN1DIR}/jaro.1
	@cp ${srcdir}/src/jaro ${JARO_LIBEXEC}/bin
	@cp -r ${srcdir}/build/gnu/* ${JARO_LIBEXEC}/bin
	@cp -r ${srcdir}/src/zlibs/* ${JARO_LIBEXEC}/zlibs/
	@cp -r ${srcdir}/src/zuper/zuper* ${JARO_LIBEXEC}/zlibs/
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

