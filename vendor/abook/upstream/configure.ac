dnl abook configure.in

AC_INIT([Abook],[0.6.2],[abook-devel@lists.sourceforge.net],[abook],[http://abook.sourceforge.net])
AC_CONFIG_SRCDIR([abook.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE

dnl ---------------
dnl gettext support
dnl ---------------

AC_USE_SYSTEM_EXTENSIONS
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.21])

AC_DEFUN([ABOOK_EXPAND_PREFIX], [
	$1=$2
	dnl expanding twice, since from autoconf 2.60 on, $datadir refers to
	dnl $datarootdir which in turn refers to $prefix
	$1=`(	test "x$prefix" = xNONE && prefix="$ac_default_prefix"
		eval tmp_abook_prefix=\""[$]$1"\"
		eval echo $tmp_abook_prefix
		)`
])

ABOOK_EXPAND_PREFIX(abook_localedir, "$datadir/locale")
localedir="\$(datadir)/locale"

AC_ARG_WITH(localedir,
	[  --with-localedir=PATH      Where the locale files are installed ])
if test "x$with_localedir" != "x"; then
	abook_localedir="$with_localedir"
	localedir="$with_localedir"
fi

AC_DEFINE_UNQUOTED(LOCALEDIR, "$abook_localedir", [locale directory])
AC_SUBST(localedir)

AM_CONDITIONAL(USE_INCLUDED_INTL_H, test x$USE_INCLUDED_LIBINTL = xyes)

dnl ----------------------
dnl end of gettext support
dnl ----------------------

AC_PROG_CC
AC_SEARCH_LIBS([strerror],[cposix])
if test "x$U" != "x"; then
	AC_MSG_ERROR(Compiler not ANSI compliant)
fi
AC_C_INLINE
AC_PROG_INSTALL

AC_CHECK_HEADERS(unistd.h locale.h sys/ioctl.h iconv.h)
AC_CHECK_HEADER(stdarg.h,AC_DEFINE(HAVE_STDARG_H, 1, [Define if you have the <stdarg.h> header file.]),AC_MSG_ERROR([*** stdarg.h is missing on your system ***]))
AC_FUNC_STRCOLL
AC_CHECK_FUNCS(setlocale)

ac_widec_funcs=yes
AC_CHECK_HEADER(wchar.h,[
	ac_have_wchar_h=yes
	AC_DEFINE(HAVE_WCHAR_H, 1, [Define if you have the <wchar.h> header file.])],
	[ac_have_wchar_h=no])
AC_CHECK_FUNCS(mbtowc wcwidth mbrtowc mbsinit,,ac_widec_funcs=no)
AC_CHECK_DECLS(wcwidth)
AC_CHECK_TYPE(wchar_t,,ac_widec_funcs=no)

if test x$ac_widec_funcs = xyes -a x$ac_have_wchar_h = xyes; then
	ac_widec_possible=yes
else
	ac_widec_possible=no
fi

dnl -------------------
dnl (n)curses detection
dnl -------------------

abook_cv_curses=/usr
AC_ARG_WITH(curses, [  --with-curses=DIR       Where ncurses is installed ],
	[if test $withval != yes; then
		abook_cv_curses=$withval
	fi
	if test x$abook_cv_curses != x/usr; then
		LDFLAGS="-L${abook_cv_curses}/lib $LDFLAGS"
		CPPFLAGS="$CPPFLAGS -I${abook_cv_curses}/include"
	fi])

AC_CHECK_LIB(ncursesw, initscr,
	[LIBS="$LIBS -lncursesw"
	if test x$abook_cv_curses = x/usr -a -d /usr/include/ncursesw; then
		CPPFLAGS="$CPPFLAGS -I/usr/include/ncursesw"
	fi
	AC_CHECK_HEADERS(ncurses.h)],[
	ac_widec_possible=no
	AC_CHECK_LIB(ncurses, initscr,
		[LIBS="$LIBS -lncurses"
		if test x$abook_cv_curses = x/usr -a -d /usr/include/ncurses; then
			CPPFLAGS="$CPPFLAGS -I/usr/include/ncurses"
		fi
		AC_CHECK_HEADERS(ncurses.h)],
		[CF_CURSES_LIBS])
	])

dnl --------------------------
dnl end of (n)curses detection
dnl --------------------------

dnl ------------------
dnl readline detection
dnl ------------------

abook_cv_readline=/usr
AC_ARG_WITH(readline, [  --with-readline=DIR     Where readline is installed ],
	[if test $withval != yes; then
		abook_cv_readline=$withval
	fi
	if test x$abook_cv_readline != x/usr; then
		LDFLAGS="-L${abook_cv_readline}/lib $LDFLAGS"
		CPPFLAGS="$CPPFLAGS -I${abook_cv_readline}/include"
	fi])

AC_LIB_READLINE

if test x$ac_cv_lib_readline = xno -o x$ac_cv_lib_readline_history = xno; then
	AC_MSG_ERROR([*** readline library not found or it doesn't support history ***])
fi

dnl -------------------------
dnl end of readline detection
dnl -------------------------

dnl ------------------
dnl vformat detection
dnl ------------------

AC_ARG_ENABLE(vformat, [  --enable-vformat          Use libvformat is available ], [case "${enableval}" in
	yes) vformat=true ;;
	no) vformat=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-vformat) ;;
	esac], [vformat=false])
if test x$vformat = xtrue; then
   AC_CHECK_HEADER([vf_iface.h], [
     AC_CHECK_LIB([vformat], [vf_read_file], [
     MODULES="$MODULES m_vcf"
     AC_DEFINE(HAVE_VFORMAT,1,[Defined if the vformat library is available])
     have_vformat=yes
  ])
])
fi
AM_CONDITIONAL(ENABLE_VFORMAT_SUPPORT,test x"$have_vformat" = "xyes")

dnl -------------------------
dnl end of vformat detection
dnl -------------------------


if test x$ac_widec_possible = xyes; then
	AC_DEFINE(HANDLE_MULTIBYTE, 1, [Handle multibyte characters])
fi

AC_CHECK_FUNCS(resizeterm)

AC_CHECK_FUNCS(snprintf vsnprintf)

AC_CHECK_FUNCS(strcasestr, AC_DEFINE(HAVE_STRCASESTR))

AC_ARG_ENABLE(debug, [  --enable-debug          Enable debugging support ], [case "${enableval}" in
	yes) debug=true ;;
	no) debug=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-debug) ;;
	esac], [debug=false])

if test x$debug = xtrue; then
	CPPFLAGS="-DDEBUG=1 $CPPFLAGS"
	CFLAGS="-g $CFLAGS"
fi

if test x$GCC = xyes; then
	CFLAGS="-Wall -fgnu89-inline $CFLAGS"
fi

AC_CONFIG_FILES([Makefile abook.spec po/Makefile.in])
AC_OUTPUT
