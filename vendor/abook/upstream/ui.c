
/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include "abook.h"
#include <assert.h>
#include "ui.h"
#include "edit.h"
#include "database.h"
#include "gettext.h"
#include "list.h"
#include "misc.h"
#include "options.h"
#include "filter.h"
#include "xmalloc.h"
#include "color.h"
#include "remap.h"
#include <sys/time.h>
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#ifdef HAVE_SYS_IOCTL_H
#	include <sys/ioctl.h>
#endif

#include "abook_rl.h"

/*
 * external variables
 */

extern char *datafile;

extern bool alternative_datafile;

extern bool db_need_save;

/*
 * internal variables
 */

static bool ui_initialized = FALSE;

static bool should_resize = FALSE;
static bool can_resize = FALSE;
static struct timeval last_click_time;
static int double_click_interval = 200; /* maximum time in milliseconds */

static WINDOW *top = NULL, *bottom = NULL;


static void
init_windows()
{
	top = newwin(LIST_TOP - 1, COLS, 0, 0);
	bottom = newwin(LINES - LIST_BOTTOM, COLS, LIST_BOTTOM, 0);
}

static void
free_windows()
{
	delwin(top);
	delwin(bottom);
}


#ifdef SIGWINCH
static void
resize_abook()
{
#ifdef TIOCGWINSZ
	struct winsize winsz;

	ioctl(0, TIOCGWINSZ, &winsz);
#ifdef DEBUG
	if(winsz.ws_col >= MIN_COLS && winsz.ws_row >= MIN_LINES) {
		fprintf(stderr, "Warning: COLS=%d, LINES=%d\n", winsz.ws_col, winsz.ws_row);
	}
#endif

	if(winsz.ws_col >= MIN_COLS && winsz.ws_row >= MIN_LINES) {
#ifdef HAVE_RESIZETERM
		resizeterm(winsz.ws_row, winsz.ws_col);
#else
		COLS = winsz.ws_col;
		LINES = winsz.ws_row;
#endif
	}

	should_resize = FALSE;
	close_list(); /* we need to recreate windows */
	init_list();
	free_windows();
	init_windows();
	refresh_screen();
	refresh();
#endif /* TIOCGWINSZ */
}


static void
win_changed(int i)
{
	if(can_resize)
		resize_abook();
	else
		should_resize = TRUE;
}
#endif /* SIGWINCH */


int
is_ui_initialized()
{
	return ui_initialized;
}

void
ui_init_curses()
{
	if(!is_ui_initialized())
		initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	if(opt_get_bool(BOOL_USE_MOUSE)) {
		mouseinterval(0);
		timerclear(&last_click_time);
		ui_enable_mouse(TRUE);
	}
	keypad(stdscr, TRUE);
	if(opt_get_bool(BOOL_USE_COLORS) && has_colors()) {
		start_color();
		use_default_colors();
		ui_init_color_pairs_user();
	}
}

void
ui_enable_mouse(bool enabled)
{
	mmask_t mask;
	if(enabled) {
		mask = BUTTON1_CLICKED | BUTTON4_PRESSED;
#if NCURSES_MOUSE_VERSION == 2
		mask |= BUTTON5_PRESSED;
#endif
	} else {
		mask = 0;
	}
	mousemask(mask, NULL);
}

/** Check the time elapsed since last click and tell if it should be
 * interpreted as a double click
 */
static bool
was_double_click() {
	struct timeval click_time, click_diff, maxdiff;
	maxdiff.tv_sec = double_click_interval / 1000;
	maxdiff.tv_usec = (double_click_interval % 1000)*1000;
	gettimeofday(&click_time, NULL);

	timersub(&click_time, &last_click_time, &click_diff);
	last_click_time = click_time;
	return !timercmp(&click_diff, &maxdiff, >);
}

#define CHECK_COLOR_NAME(value, name, DEFNAME) \
	if(!strcmp((name), (value))){ \
		return DEFNAME; \
	}
short
opt_color_to_color(enum str_opts enum_name)
{
	char* name = opt_get_str(enum_name);
	CHECK_COLOR_NAME(name, "default", COLOR_DEFAULT)
	else CHECK_COLOR_NAME(name, "black", COLOR_BLACK)
	else CHECK_COLOR_NAME(name, "red", COLOR_RED)
	else CHECK_COLOR_NAME(name, "green", COLOR_GREEN)
	else CHECK_COLOR_NAME(name, "yellow", COLOR_YELLOW)
	else CHECK_COLOR_NAME(name, "blue", COLOR_BLUE)
	else CHECK_COLOR_NAME(name, "magenta", COLOR_MAGENTA)
	else CHECK_COLOR_NAME(name, "cyan", COLOR_CYAN)
	else CHECK_COLOR_NAME(name, "white", COLOR_WHITE)
    else return COLOR_DEFAULT;
}

void
ui_init_color_pairs_user()
{
	init_pair(CP_HEADER, opt_color_to_color(STR_COLOR_HEADER_FG),
	                     opt_color_to_color(STR_COLOR_HEADER_BG));
	init_pair(CP_FOOTER, opt_color_to_color(STR_COLOR_FOOTER_FG),
	                     opt_color_to_color(STR_COLOR_FOOTER_BG));
	init_pair(CP_LIST_EVEN, opt_color_to_color(STR_COLOR_LIST_EVEN_FG),
	                        opt_color_to_color(STR_COLOR_LIST_EVEN_BG));
	init_pair(CP_LIST_ODD,  opt_color_to_color(STR_COLOR_LIST_ODD_FG),
	                        opt_color_to_color(STR_COLOR_LIST_ODD_BG));
	init_pair(CP_LIST_HEADER, opt_color_to_color(STR_COLOR_LIST_HEADER_FG),
	                     opt_color_to_color(STR_COLOR_LIST_HEADER_BG));
	init_pair(CP_LIST_HIGHLIGHT, opt_color_to_color(STR_COLOR_LIST_HIGHLIGHT_FG),
	                     opt_color_to_color(STR_COLOR_LIST_HIGHLIGHT_BG));
	init_pair(CP_TAB_BORDER, opt_color_to_color(STR_COLOR_TAB_BORDER_FG),
	                     opt_color_to_color(STR_COLOR_TAB_BORDER_BG));
	init_pair(CP_TAB_LABEL, opt_color_to_color(STR_COLOR_TAB_LABEL_FG),
	                     opt_color_to_color(STR_COLOR_TAB_LABEL_BG));
	init_pair(CP_FIELD_NAME, opt_color_to_color(STR_COLOR_FIELD_NAME_FG),
	                     opt_color_to_color(STR_COLOR_FIELD_NAME_BG));
	init_pair(CP_FIELD_VALUE, opt_color_to_color(STR_COLOR_FIELD_VALUE_FG),
	                     opt_color_to_color(STR_COLOR_FIELD_VALUE_BG));
}

int
init_ui()
{
	ui_init_curses();
#ifdef DEBUG
        fprintf(stderr, "init_abook():\n");
        fprintf(stderr, "  COLS = %d, LINES = %d\n", COLS, LINES);
#endif
	if( LINES < MIN_LINES || COLS < MIN_COLS ) {
		clear(); refresh(); endwin();
		fprintf(stderr, _("Your terminal size is %dx%d\n"), COLS, LINES);
		fprintf(stderr, _("Terminal is too small. Minimum terminal "
				"size for abook is "
				"%dx%d\n"), MIN_COLS, MIN_LINES);
		return 1;
	}

	init_list();
	init_windows();

	ui_initialized = TRUE;

#ifdef SIGWINCH
	signal(SIGWINCH, win_changed);
#endif

	return 0;
}

void
close_ui()
{
	close_list();
	free_windows();
	clear();
	refresh();
	endwin();

	ui_initialized = FALSE;
}


void
headerline(const char *str)
{
	werase(top);

	wattrset(top, COLOR_PAIR(CP_HEADER));
	mvwhline(top, 0, 0, ' ', COLS);
	mvwhline(top, 1, 0, UI_HLINE_CHAR, COLS);

	mvwprintw(top, 0, 0, "%s | %s", PACKAGE " " VERSION, str);

	refresh();
	wrefresh(top);
}


void
refresh_screen()
{
	const char *remap_table;
	char *helpline;
	int i, size;

#ifdef SIGWINCH
	if(should_resize) {
		resize_abook();
		return;
	}
#endif
	clear();

	refresh_statusline();

	remap_table = get_remap_table();
	helpline = strdup(gettext(MAIN_HELPLINE));
	size = strlen(helpline);

	for (i=1; i<size; ++i)
		if (helpline[i] == ':')
			helpline[i-1] = remap_table[helpline[i-1]];

	headerline(helpline);
	free(helpline);

	list_headerline();

	refresh_list();
}


int
statusline_msg(const char *msg)
{
	int c;

	clear_statusline();
	statusline_addstr(msg);
	c = getch();
#ifdef DEBUG
	fprintf(stderr, "statusline_msg(\"%s\")\n", msg);
#endif
	clear_statusline();

	return c;
}

void
statusline_addstr(const char *str)
{
	mvwaddstr(bottom, 1, 0, str);
	refresh();
	wrefresh(bottom);
}

/* Same as statusline_addstr(), but hilight "<str>" sequences if the terminal
 * supports it */
static void
statusline_addhlstr(const char *str)
{
#if defined(A_BOLD) && defined(A_NORMAL) && defined(A_DIM)
	const char *p = str, *start = str;
	char *tmp;
	int pos = 0;

	while(1) {
		if(!*p || strchr("<>", *p)) {
			if(p - start > 0) {
				wattrset(bottom, (*p == '>') ? A_BOLD : A_NORMAL);
				tmp = xstrndup(start, p - start);
				mvwaddstr(bottom, 1, pos, tmp);
				pos += strwidth(tmp);
				free(tmp);
			}
			if(*p) {
				start = p + 1;

				/* show tag markers */
				wattrset(bottom, A_DIM);
				mvwaddch(bottom, 1, pos++, *p);
			}
		}

		if(!*p) {
			wattrset(bottom, A_NORMAL);
			break;
		}

		p++;
	}
#else
	mvwaddstr(bottom, 1, 0, str);
#endif

	refresh();
	wrefresh(bottom);
}

int
statusline_askchoice(const char *msg, const char *choices, short dflt)
{
	char *s;
	int ch;

	assert((dflt >= 0) && (dflt <= strlen(choices)));

	if(dflt) {
		s = strdup_printf("%s [%c]", msg, choices[dflt - 1]);
		statusline_addhlstr(s);
		free(s);
	} else
		statusline_addhlstr(msg);

	while(1)
	{
		ch = tolower(getch());

		if(ch == 7) /* ctrl+G */
			return 0;

		if(dflt && (ch == '\r')) /* default choice */
			return dflt;

		if((s = strchr(choices, ch)))
			return (s - choices + 1);
	}
}

char *
ui_readline(const char *prompt, char *s, size_t limit, bool use_completion)
{
	int y, x;
	char *ret;

	mvwaddstr(bottom, 1, 0, prompt);

	getyx(bottom, y, x);

	ret = abook_readline(bottom, y, x, s, use_completion);

	if(ret) {
		strtrim(ret);
		if(strlen(ret) > limit && limit > 0)
			ret[limit] = '\0';
	}

	return ret;
}

int
statusline_ask_boolean(const char *msg, int def)
{
	int ret;
	char *msg2 = strconcat(msg,  def ? _(" (Y/n)?") : _(" (y/N)?"), NULL);
	char ch;

	statusline_addstr(msg2);

	free(msg2);

	ch = tolower(getch());

	if(ch == *(S_("keybinding for no|n")))
		ret = FALSE;
	else if(ch == *(S_("keybinding for yes|y")))
		ret = TRUE;
	else
		ret = def;

	clear_statusline();

	return ret;
}

void
refresh_statusline()
{
	werase(bottom);

	wattrset(bottom, COLOR_PAIR(CP_FOOTER));
	mvwhline(bottom, 0, 0, UI_HLINE_CHAR, COLS);

	refresh();
	wrefresh(bottom);
}

char *
ask_filename(const char *prompt)
{
	char *buf = NULL;

	clear_statusline();

	buf = ui_readline(prompt, NULL, -1, 1);

	return buf;
}

void
clear_statusline()
{
	wmove(bottom, 1, 0);
	wclrtoeol(bottom);
	wrefresh(bottom);
	refresh();
}

/*
 * help
 */

#include "help.h"

void
display_help(int help)
{
	int i;
	char **tbl;
	WINDOW *helpw;

	switch(help) {
		case HELP_MAIN:
			tbl = mainhelp;
			break;
		case HELP_EDITOR:
			tbl = editorhelp;
			break;
		default:return;
	}

	helpw = newwin(LINES - 5, COLS - 6, 2, 3);
	erase();
	headerline(_("help"));

	for(i = 0; tbl[i] != NULL; i++) {
		waddstr(helpw, gettext(tbl[i]));
		if( (!((i + 1) % (LINES - 8))) ||
			(tbl[i + 1] == NULL) ) {
			refresh();
			wrefresh(helpw);
			refresh_statusline();
			if(statusline_msg(_("Press any key to continue..."))
					== 'q')
				break;
			wclear(helpw);
		}
	}

	clear_statusline();
	delwin(helpw);
}

/*
 * end of help
 */

extern char *selected;

void
get_commands()
{
	const char *remap_table;
	int ch;

	remap_table = get_remap_table();

	for(;;) {
		can_resize = TRUE; /* it's safe to resize now */
		if(!opt_get_bool(BOOL_SHOW_CURSOR))
			hide_cursor();
		if(should_resize)
			refresh_screen();
		ch = getch();
		if(!opt_get_bool(BOOL_SHOW_CURSOR))
			show_cursor();
		can_resize = FALSE; /* it's not safe to resize anymore */
		if(ch == KEY_MOUSE) {
			MEVENT event;
			bool double_clicked = was_double_click();
			if(getmouse(&event) == OK) {
				if(event.bstate & BUTTON1_CLICKED
				   || event.bstate & BUTTON1_DOUBLE_CLICKED) {
					if(event.y == 0) {
						return;
					}
					list_set_curitem(event.y + list_get_firstitem() - LIST_TOP);
					if(double_clicked) {
						edit_item(-1);
					} else {
						refresh_list();
					}
				} else if(event.bstate & BUTTON4_PRESSED) {
					scroll_list_up();
				} else if(event.bstate & BUTTON5_PRESSED) {
					scroll_list_down();
				}
			}
		}

		if (ch > 0 && ch <= 127)
			ch = remap_table[ch];

		switch(ch) {
			case 'q': return;
			case 'Q': quit_abook(QUIT_DONTSAVE);	break;
			case 'P': print_stderr(selected_items() ?
						  -1 : list_get_curitem());
				  return;
			case '?':
				  display_help(HELP_MAIN);
				  refresh_screen();
				  break;
			case 'a': add_item();		break;
			case '\r': edit_item(-1);	break;
			case KEY_DC:
			case 'd':
			case 'r': ui_remove_items();	break;
			case 'M': ui_merge_items();	break;
			case 'D': duplicate_item();	break;
			case 'U': ui_remove_duplicates(); break;
			case 12: refresh_screen();	break;

			case 'k':
			case KEY_UP: scroll_up();	break;
			case 'j':
			case KEY_DOWN: scroll_down();	break;
			case 'K':
			case KEY_PPAGE: page_up();	break;
			case 'J':
			case KEY_NPAGE: page_down();	break;

			case 'g':
			case KEY_HOME: goto_home();	break;
			case 'G':
			case KEY_END: goto_end();	break;

			case 'w': save_database(0); // we may reconsider force_save = 1
				  break;
			case 'l': ui_read_database();	break;
			case 'i': import_database();	break;
			case 'e': export_database();	break;
			case 'C': ui_clear_database();	break;

			case 'o': ui_open_datafile();	break;

			case 's': sort_by_field("name");break;
			case 'S': sort_surname();	break;
			case 'F': sort_by_field(NULL);	break;

			case '/': ui_find(0);		break;
			case 'n':
			case '\\': ui_find(1);		break;

			case ' ': if(list_get_curitem() >= 0) {
				   list_invert_curitem_selection();
				   ui_print_number_of_items();
				   refresh_list();
				  }
				break;
			case '+': select_all();
				  refresh_list();
				break;
			case '-': select_none();
				  refresh_list();
				break;
			case '*': invert_selection();
				  refresh_list();
				 break;
			case 'A': move_curitem(MOVE_ITEM_UP);
				break;
			case 'Z': move_curitem(MOVE_ITEM_DOWN);
				break;

			case 'm': launch_mutt(selected_items() ?
						  -1 : list_get_curitem());
				  refresh_screen();
				  break;

			case 'p': ui_print_database(); break;

			case 'v': launch_wwwbrowser(list_get_curitem());
				  refresh_screen();
				  break;
		}
	}
}

void
ui_remove_items()
{
	if(list_is_empty())
		return;

	if(statusline_ask_boolean(_("Remove selected item(s)"), FALSE))
		remove_selected_items();

	clear_statusline();
	refresh_list();
}

void
ui_merge_items()
{
	if(statusline_ask_boolean(_("Merge selected items"), FALSE))
		merge_selected_items();

	clear_statusline();
	refresh_list();
}

void		ui_remove_duplicates()
{
	if(statusline_ask_boolean(_("Remove duplicates"), FALSE))
		remove_duplicates();
	
	clear_statusline();
	refresh_list();
}

void
ui_clear_database()
{
	if(statusline_ask_boolean(_("Clear WHOLE database"), FALSE)) {
		close_database();
		db_need_save = TRUE;
		refresh_list();
	}
}

void
ui_find(int next)
{
	int item = -1;
	static char findstr[MAX_FIELD_LEN];
	int search_fields[] = {NAME, EMAIL, NICK, NOTES, -1};

	clear_statusline();

	if(next) {
		if(!*findstr)
			return;
	} else {
		char *s;
		s = ui_readline("/", findstr, MAX_FIELD_LEN - 1, 0);
		refresh_screen();
		if(s == NULL) {
			return; /* user cancelled (ctrl-G) */
		} else {
			strncpy(findstr, s, MAX_FIELD_LEN);
			free(s);
		}
	}

	if( (item = find_item(findstr, list_get_curitem() + !!next,
			search_fields)) < 0 &&
			(item = find_item(findstr, 0, search_fields)) >= 0)
		statusline_addstr(_("Search hit bottom, continuing at top"));

	if(item >= 0) {
		list_set_curitem(item);
		refresh_list();
	}
}

void
ui_print_number_of_items()
{
	char *str = strdup_printf("    %c" "|%3d/%3d",
				  db_need_save ? '*' : ' ',
				  selected_items(),
				  db_n_items());

	attrset(COLOR_PAIR(CP_HEADER));
	mvaddstr(0, COLS-strlen(str), str);

	free(str);
}

void
ui_read_database()
{
	char *msg;

	if(!list_is_empty()) {
		msg = strdup_printf(_("Your current data will be lost - "
				"Press '%c' to continue"),
				*(S_("keybinding for yes|y")));
		if(!statusline_ask_boolean(msg, FALSE)) {
			free(msg);
			return;
		}
		free(msg);
	}

	load_database(datafile);
	refresh_list();
}


void
ui_print_database()
{
	FILE *handle;
	char *command = opt_get_str(STR_PRINT_COMMAND);
	int mode;

	if(list_is_empty())
		return;

	switch(statusline_askchoice(_("Print <a>ll, print <s>elected, or <c>ancel?"), S_("keybindings:all/selected/cancel|asc"), 3)) {
		case 1:
			mode = ENUM_ALL;
			break;
		case 2:
			if( !selected_items() ) {
				statusline_msg(_("No selected items"));
				return;
			}
			mode = ENUM_SELECTED;
			break;
		default:
			refresh_screen();
			return;
	}

	clear_statusline();

	if( ! *command || (handle = popen(command, "w")) == NULL)
		return;

	fexport("text", handle, mode);

	pclose(handle);
}


void
ui_open_datafile()
{
	char *filename;

	filename = ask_filename(_("File to open: "));

	if(!filename || ! *filename) {
		free(filename);
		refresh_screen();
		return;
	}

	if(opt_get_bool(BOOL_AUTOSAVE))
		save_database(0);
	else if(statusline_ask_boolean(_("Save current database"), FALSE))
		save_database(1);

	close_database();

	load_database(filename);

	if(list_is_empty()) {
		statusline_msg(_("Sorry, the specified file appears not to be a valid abook addressbook"));
		load_database(datafile);
	} else {
		free(datafile);
		datafile = xstrdup(filename);
	}

	refresh_screen();
	free(filename);

	alternative_datafile = TRUE;
}
