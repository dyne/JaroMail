/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "abook.h"
#include "abook_rl.h"

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#if defined(HAVE_READLINE_READLINE_H)
#       include <readline/readline.h>
#elif defined(HAVE_READLINE_H)
#       include <readline.h>
#else
#	error "You don't seem to have readline.h"
#	error "No HAVE_READLINE_READLINE_H or HAVE_READLINE_H defined"
#endif

#if defined(HAVE_READLINE_HISTORY_H)
#       include <readline/history.h>
#elif defined(HAVE_HISTORY_H)
#       include <history.h>
#else
#	error "You don't seem to have history.h"
#	error "No HAVE_READLINE_HISTORY_H or HAVE_HISTORY_H defined"
#endif

#ifdef HANDLE_MULTIBYTE
#	include <mbswidth.h>
#endif

#define RL_READLINE_NAME	"Abook"

static int rl_x, rl_y;
static WINDOW *rl_win;

static bool rl_cancelled;

static void
rl_refresh()
{
	/* refresh(); */
	wrefresh(rl_win);
}

#ifdef HANDLE_MULTIBYTE
static int
rline_calc_point()
{
	return (int)mbsnwidth(rl_line_buffer, rl_point, 0);
}
#endif

static void
rline_update()
{
#ifdef HANDLE_MULTIBYTE
	int real_point = rline_calc_point() + rl_x;
#else
	int real_point = rl_point + rl_x;
#endif

	if(real_point > (COLS - 1))
		mvwaddnstr(rl_win, rl_y, rl_x,
			rl_line_buffer + (1 + real_point - COLS),
			COLS - rl_x - 1);
	else
		mvwaddnstr(rl_win, rl_y, rl_x, rl_line_buffer, rl_end);

	wclrtoeol(rl_win);
	wmove(rl_win, rl_y, min(real_point, COLS - 1));

	rl_refresh();
}

static void
rline_compdisp(char **matches, int n, int max_len)
{
	/* dummy */
}

static void
rline_prep_terminal(int dummy)
{
#if (RL_VERSION_MAJOR == 4 && RL_VERSION_MINOR > 2) || (RL_VERSION_MAJOR > 4)
	/* nothing */
#else
	/*
	 * #warning is an extension. Not all compilers support it.
	 */
#	ifdef __GNUC__
#		warning "You seem to have rather old readline version or \
non-GNU version of it. If you have problems please use \
GNU readline 4.3 or newer. \
GNU readline versions 4.0, 4.1 and 4.2 should be OK despite \
of this warning."
#	endif
	/*
	 * this kludge avoids older readline libraries to print a newline
	 */
	extern int readline_echoing_p;
	readline_echoing_p = 0;
#endif
	raw();
	keypad(rl_win, FALSE);
}

static void
rline_deprep_terminal(void)
{
	cbreak();
	keypad(rl_win, TRUE);
}

static int
rl_cancel(int dummy1, int dummy2)
{
	rl_cancelled = TRUE;

	rl_done = 1;

	return 0;
}

static void
abook_rl_init(bool use_completion)
{
	rl_readline_name = RL_READLINE_NAME;

#if RL_VERSION_MAJOR >= 4
	rl_already_prompted = 1;
#endif
	rl_catch_sigwinch = 0;
	rl_erase_empty_line = 0;

	rl_redisplay_function = rline_update;
	rl_completion_display_matches_hook = rline_compdisp;
	rl_prep_term_function = rline_prep_terminal;
	rl_deprep_term_function = rline_deprep_terminal;

	rl_unbind_function_in_map(rl_clear_screen, rl_get_keymap());
	rl_unbind_function_in_map(rl_reverse_search_history, rl_get_keymap());
	rl_unbind_function_in_map(rl_re_read_init_file, rl_get_keymap());

	if(use_completion) {
		rl_bind_key('\t', rl_menu_complete);
	} else {
		rl_unbind_function_in_map(rl_complete, rl_get_keymap());
		rl_unbind_function_in_map(rl_menu_complete, rl_get_keymap());
	}

	rl_bind_key('g' & 31, rl_cancel); /* C-g */

	clear_history();

	rl_cancelled = FALSE;
}

char *
abook_readline(WINDOW *w, int y, int x, char *s, bool use_completion)
{
	char *ret;

	abook_rl_init(use_completion);

	wmove(rl_win = w, rl_y = y, rl_x = x);
	rl_refresh();

	if(s && *s)
		add_history(s);

	ret = readline(NULL);

	if(rl_cancelled && ret) {
		free(ret);
		ret = NULL;
	}

	return ret;
}

