#ifndef _ABOOK_RL_H
#define _ABOOK_RL_H

#include "abook_curses.h"

char		*abook_readline(WINDOW *w, int y, int x, char *s,
		bool use_completion);

#endif
