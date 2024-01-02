DESTDIR?=
PREFIX?=/usr

all:
	@echo "Nothing to be make. Make install to ${DESTDIR}${PREFIX}/share/zuper"

install:
	install -d ${DESTDIR}${PREFIX}/share/zuper
	install -p -m 644 zuper      ${DESTDIR}${PREFIX}/share/zuper/zuper
	install -p -m 644 zuper.init ${DESTDIR}${PREFIX}/share/zuper/zuper.init
