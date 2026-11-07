#ifndef _ABOOK_H
#define _ABOOK_H

#include <stdio.h>

FILE		*abook_fopen (const char *path, const char *mode);
void		quit_abook(int save_db);
void		launch_wwwbrowser(int item);
void		launch_mutt(int item);
void		print_stderr(int item);
#ifdef _AIX
int		strcasecmp (const char *, const char *);
int		strncasecmp (const char *, const char *, size_t);
#endif

#define MAIN_HELPLINE        N_("q:quit  ?:help  a:add  r:remove")

#define Y_STATUSLINE   	(LINES - 2)

#define MIN_LINES	20
#define MIN_COLS	70

#define DEFAULT_UMASK	066
#define DIR_IN_XDG_DATA	"abook"
#define DIR_IN_HOME	".abook"
#define DATAFILE	"addressbook"

#define RCFILE		"abookrc"

#define QUIT_SAVE	1
#define QUIT_DONTSAVE	0

/*
 * some "abookwide" useful macros
 */

#define hide_cursor()	curs_set(0)
#define show_cursor()	curs_set(1)

#define safe_atoi(X)    ((X == NULL) ? 0 : atoi(X))
#define safe_str(X)	((X == NULL) ? "" : X)

#ifndef min
#       define min(x,y) (((x)<(y)) ? (x):(y))
#endif

#ifndef max
#       define max(x,y) (((x)>(y)) ? (x):(y))
#endif

#define ISSPACE(c)	isspace((unsigned char)c)

#define SKIPWS(c)	while(*(c) && ISSPACE(*(c))) c++
#define SKIPNONWS(c)	while(*(c) && ! ISSPACE(*(c))) c++

#ifndef DEBUG
#	define NDEBUG	1
#else
#	undef NDEBUG
#endif

#endif
