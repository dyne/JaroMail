PREFIX ?= /usr/local
JARO_LIBEXEC := ${PREFIX}/share/jaromail
JARO_SHARE := ${PREFIX}/share/jaromail
MAN1DIR := ${PREFIX}/share/man/man1
srcdir := $(shell pwd)

.PHONY: all man install clean

all:
	CC="$(CC)" ./build/auto build

man:
	@pandoc -s -t man \
		--metadata title="jaromail" --metadata section="1" \
		--metadata date="$$(date +%Y-%m-%d)" \
		"${srcdir}/doc/jaromail-manual.md" -o "${srcdir}/doc/jaromail.1"
	@pandoc -s -t man \
		--metadata title="jaro" --metadata section="1" \
		--metadata date="$$(date +%Y-%m-%d)" \
		"${srcdir}/doc/jaro.1.md" -o "${srcdir}/doc/jaro.1"

install: all man
	$(info Installing JaroMail in ${JARO_SHARE})
	@mkdir -p "$(DESTDIR)${JARO_SHARE}/mutt" "$(DESTDIR)${JARO_SHARE}/stats"
	@mkdir -p "$(DESTDIR)${JARO_LIBEXEC}/bin" "$(DESTDIR)${JARO_LIBEXEC}/zlibs"
	@mkdir -p "$(DESTDIR)${MAN1DIR}"
	@chmod -R a+rX "$(DESTDIR)${JARO_SHARE}"
	@cp -r "${srcdir}/doc/"* "$(DESTDIR)${JARO_SHARE}/"
	@cp -r "${srcdir}/src/mutt/"* "$(DESTDIR)${JARO_SHARE}/mutt/"
	@cp -r "${srcdir}/src/stats/"* "$(DESTDIR)${JARO_SHARE}/stats/"
	@cp "${srcdir}/doc/jaromail.1" "$(DESTDIR)${MAN1DIR}/jaromail.1"
	@cp "${srcdir}/doc/jaro.1" "$(DESTDIR)${MAN1DIR}/jaro.1"
	@cp "${srcdir}/src/jaro" "$(DESTDIR)${JARO_LIBEXEC}/bin"
	@cp "${srcdir}/build/gnu/fetchaddr" "${srcdir}/build/gnu/parsedate" \
		"${srcdir}/build/gnu/dotlock" "${srcdir}/build/gnu/gpgewrap" "$(DESTDIR)${JARO_LIBEXEC}/bin"
	@cp -r "${srcdir}/src/zlibs/"* "$(DESTDIR)${JARO_LIBEXEC}/zlibs/"
	@cp -r "${srcdir}/src/zuper/"zuper* "$(DESTDIR)${JARO_LIBEXEC}/zlibs/"
	@mkdir -p "$(DESTDIR)${PREFIX}/bin"
	@printf '%s\n' '#!/usr/bin/env zsh' \
		'export JAROWORKDIR="${JARO_SHARE}"' \
		'exec "${JARO_SHARE}/bin/jaro" "$$@"' > "$(DESTDIR)${PREFIX}/bin/jaro"
	@chmod +x "$(DESTDIR)${PREFIX}/bin/jaro"
	@echo "JaroMail succesfully installed"
	@echo "To initialize your Mail dir use: jaro init PATH"
	@echo "Default PATH is \$$HOME/Mail"

clean:
	rm -f src/*.o
	rm -f src/gpgewrap
	rm -f src/fetchaddr
	rm -f src/parsedate
	rm -f src/dotlock
	rm -f build/gnu/fetchaddr build/gnu/parsedate build/gnu/dotlock build/gnu/gpgewrap

