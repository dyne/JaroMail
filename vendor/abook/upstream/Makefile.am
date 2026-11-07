
bin_PROGRAMS = abook

if ENABLE_VFORMAT_SUPPORT
vformat_SOURCE = vcard.c vcard.h
AM_LDFLAGS = -lvformat
else
vformat_SOURCE =
endif

abook_SOURCES = abook.c abook_rl.c database.c edit.c \
		filter.c getname.c getopt.c getopt1.c gettext.c \
		ldif.c list.c mbswidth.c misc.c options.c \
		remap.c ui.c views.c xmalloc.c \
		\
		abook.h abook_curses.h abook_rl.h database.h edit.h \
		filter.h getname.h getopt.h gettext.h \
		help.h list.h ldif.h mbswidth.h misc.h options.h \
		remap.h ui.h views.h xmalloc.h \
		$(vformat_SOURCE)

EXTRA_DIST = config.rpath  ANNOUNCE BUGS FAQ abook.1 abookrc.5 sample.abookrc \
		abook.spec contrib doc/HOWTO.translating_abook RELEASE_NOTES

abook_LDADD = @LIBINTL@


install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(mandir)/man1 $(DESTDIR)$(mandir)/man5
	$(INSTALL_DATA) $(srcdir)/abook.1 $(DESTDIR)$(mandir)/man1/
	$(INSTALL_DATA) $(srcdir)/abookrc.5 $(DESTDIR)$(mandir)/man5/

uninstall-local:
	-rm -f $(DESTDIR)$(mandir)/man1/abook.1
	-rm -f $(DESTDIR)$(mandir)/man5/abookrc.5


SUBDIRS = po

ACLOCAL_AMFLAGS = -I m4

if USE_INCLUDED_INTL_H
AM_CPPFLAGS = -Iintl
endif
