#ifndef _ABOOK_CURSES_H
#define _ABOOK_CURSES_H

#include "config.h"

#ifdef HAVE_NCURSES_H
#	include <ncurses.h>
#else
#	include <curses.h>
#endif

#ifndef getnstr
#	define getnstr(s, n)           wgetnstr(stdscr, s, n)
#endif

#endif
