#ifndef _UI_H
#define _UI_H

#include "abook_curses.h"

enum {
	HELP_MAIN,
	HELP_EDITOR
};

int		is_ui_initialized();
void		ui_init_curses();
void		ui_init_color_pairs_user();
void		ui_enable_mouse(bool enabled);
int		init_ui();
void		close_ui();
void		headerline(const char *str);
void            refresh_screen();
int		statusline_msg(const char *msg);
int 		statusline_askchoice(const char *msg, const char *choices,
			short dflt);
char		*ask_filename(const char *prompt);
int		statusline_ask_boolean(const char *msg, int def);
void            clear_statusline();
void		display_help(int help);
void		statusline_addstr(const char *str);
char *		ui_readline(const char *prompt, char *s, size_t limit,
			bool use_completion);
void		refresh_statusline();
void		get_commands();
void		ui_remove_items();
void		ui_merge_items();
void		ui_remove_duplicates();
void		ui_clear_database();
void		ui_find(int next);
void		ui_print_number_of_items();
void		ui_read_database();
char		*get_surname(char *s);
void		ui_print_database();
void		ui_open_datafile();

#if NCURSES_MOUSE_VERSION != 2
#define BUTTON5_PRESSED (0x80 | 0x8000000)
#endif

#include "options.h" /* needed for options_get_bool */

#define UI_HLINE_CHAR		opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'-' : ACS_HLINE
#define UI_VLINE_CHAR		opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'|' : ACS_VLINE
#define UI_TEE_CHAR		opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'-' : ACS_BTEE
#define UI_LBOXLINE_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'/' : ACS_HLINE
#define UI_RBOXLINE_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'\\' : ACS_HLINE
#define UI_ULCORNER_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					' ' : ACS_ULCORNER
#define UI_URCORNER_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					' ' : ACS_URCORNER
#define UI_LLCORNER_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'+' : ACS_LLCORNER
#define UI_LRCORNER_CHAR	opt_get_bool(BOOL_USE_ASCII_ONLY) ? \
					'+' : ACS_LRCORNER

#endif
