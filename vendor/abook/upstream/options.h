#ifndef _OPTIONS_H
#define _OPTIONS_H

#if 0
typedef int bool;
#else
#	include <abook_curses.h> /* bool */
#endif

/*
 * token parsing options
 */

#define TOKEN_ALLOC (1<<1) /* allocate memory for the token */
#define TOKEN_EQUAL (1<<2) /* left hand value of assignment */
#define TOKEN_COMMA (1<<3) /* comma is a separator */

/*
 * bool options
 */

enum bool_opts {
	BOOL_AUTOSAVE,
	BOOL_SHOW_ALL_EMAILS,
	BOOL_MUTT_RETURN_ALL_EMAILS,
	BOOL_USE_ASCII_ONLY,
	BOOL_ADD_EMAIL_PREVENT_DUPLICATES,
	BOOL_SHOW_CURSOR,
	BOOL_USE_COLORS,
	BOOL_USE_MOUSE,
	BOOL_MAX
};

/*
 * int options
 */

enum int_opts {
	INT_EMAILPOS,
	INT_EXTRAPOS,
	INT_SCROLL_SPEED,
	INT_MAXIMUM /* INT_MAX conflicts on some systems */
};

/*
 * string options
 */

enum str_opts {
	STR_EXTRA_COLUMN,
	STR_EXTRA_ALTERNATIVE,
	STR_INDEX_FORMAT,
	STR_MUTT_COMMAND,
	STR_PRINT_COMMAND,
	STR_WWW_COMMAND,
	STR_ADDRESS_STYLE,
	STR_PRESERVE_FIELDS,
	STR_SORT_FIELD,
	STR_COLOR_HEADER_FG,
	STR_COLOR_HEADER_BG,
	STR_COLOR_FOOTER_FG,
	STR_COLOR_FOOTER_BG,
	STR_COLOR_LIST_EVEN_FG,
	STR_COLOR_LIST_EVEN_BG,
	STR_COLOR_LIST_ODD_FG,
	STR_COLOR_LIST_ODD_BG,
	STR_COLOR_LIST_HEADER_FG,
	STR_COLOR_LIST_HEADER_BG,
	STR_COLOR_LIST_HIGHLIGHT_FG,
	STR_COLOR_LIST_HIGHLIGHT_BG,
	STR_COLOR_TAB_BORDER_FG,
	STR_COLOR_TAB_BORDER_BG,
	STR_COLOR_TAB_LABEL_FG,
	STR_COLOR_TAB_LABEL_BG,
	STR_COLOR_FIELD_NAME_FG,
	STR_COLOR_FIELD_NAME_BG,
	STR_COLOR_FIELD_VALUE_FG,
	STR_COLOR_FIELD_VALUE_BG,
	STR_MAX
};


int		opt_get_int(enum int_opts opt);
bool		opt_get_bool(enum bool_opts opt);
char *		opt_get_str(enum str_opts opt);
void		init_opts();
void		free_opts();
int		load_opts(char *filename);

#endif
