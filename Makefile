PREFIX ?= /usr/local
JARO_LIBEXEC := ${PREFIX}/share/jaromail
JARO_SHARE := ${PREFIX}/share/jaromail
MAN1DIR := ${PREFIX}/share/man/man1
srcdir := $(shell pwd)

.PHONY: all man install clean test test-strict sieve-tools filter-core

all:
	CC="$(CC)" ./build/auto build

# Independent test oracles; never part of the application build or installation.
sieve-tools:
	CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" LDLIBS="$(LDLIBS)" python3 build/sieve-tools.py

# Shared native objects only; Zsh remains the active filter engine.
filter-core:
	$(MAKE) -f build/filter-core.mk filter-core

test: all
	zsh extras/test/run-tests.sh

test-strict: all
	JARO_TEST_REQUIRE_ALL=1 zsh extras/test/run-tests.sh

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
		"${srcdir}/build/gnu/dotlock" "${srcdir}/build/gnu/gpgewrap" \
		"${srcdir}/build/gnu/jaro-filter-compile" "${srcdir}/build/gnu/jaro-filter-legacy" \
		"${srcdir}/build/gnu/jaro-filter" "$(DESTDIR)${JARO_LIBEXEC}/bin"
	@cp -r "${srcdir}/src/zlibs/"* "$(DESTDIR)${JARO_LIBEXEC}/zlibs/"
	@cp -r "${srcdir}/src/zuper/"zuper* "$(DESTDIR)${JARO_LIBEXEC}/zlibs/"
	@rm -f -- "$(DESTDIR)${JARO_LIBEXEC}/zlibs/"*.zwc
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
	rm -f build/gnu/filter/*.o build/gnu/filter/*.d build/gnu/filter/filter-policy-driver
	rm -f build/gnu/jaro-filter-compile build/gnu/jaro-filter-legacy build/gnu/filter/jaro-filter-compile build/gnu/filter/jaro-filter-legacy
	rm -f build/gnu/filter/jaro-filter-compile-test build/gnu/filter/filter-generation-pin build/gnu/filter/filter-compiler-driver
	rm -f build/gnu/filter/jaro-filter build/gnu/filter/jaro-filter-test build/gnu/filter/jaro-filter-signal-test
	rm -f build/gnu/jaro-filter

