
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
#include <wchar.h>
#include <wctype.h>
#include "abook.h"
#include <assert.h>
#include "ui.h"
#include "edit.h"
#include "database.h"
#include "sqlite-editor.h"
#include "contact/text.h"
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
#include <limits.h>

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

static volatile sig_atomic_t should_resize = FALSE;
static bool can_resize = FALSE;
static struct timeval last_click_time;
static int double_click_interval = 200; /* maximum time in milliseconds */

static WINDOW *top = NULL, *bottom = NULL;
static char contact_query[1024];
static char contact_feedback[256];
static bool entering_query;
static void inspect_contact_mailboxes(void);

static int min_lines(void) { return jaro_editor_profile ? 12 : MIN_LINES; }
static int min_cols(void) { return jaro_editor_profile ? 48 : MIN_COLS; }


static void
init_windows()
{
	top = newwin(LIST_TOP - 1, COLS, 0, 0);
	bottom = newwin(LINES - LIST_BOTTOM, COLS, LIST_BOTTOM, 0);
	keypad(bottom, TRUE);
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

	should_resize = FALSE;
	if(ioctl(0, TIOCGWINSZ, &winsz) < 0)
		return;
#ifdef DEBUG
	if(winsz.ws_col >= min_cols() && winsz.ws_row >= min_lines()) {
		fprintf(stderr, "Warning: COLS=%d, LINES=%d\n", winsz.ws_col, winsz.ws_row);
	}
#endif

	if(winsz.ws_col >= min_cols() && winsz.ws_row >= min_lines()) {
#ifdef HAVE_RESIZETERM
		resizeterm(winsz.ws_row, winsz.ws_col);
#else
		COLS = winsz.ws_col;
		LINES = winsz.ws_row;
#endif
	}

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
	if(can_resize && !jaro_editor_profile)
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
	if(opt_get_bool(BOOL_USE_COLORS) && has_colors() && start_color() == OK)
		if(use_default_colors() == OK || !jaro_editor_profile)
			ui_init_color_pairs_user();
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
	if( LINES < min_lines() || COLS < min_cols() ) {
		clear(); refresh(); endwin();
		fprintf(stderr, _("Your terminal size is %dx%d\n"), COLS, LINES);
		fprintf(stderr, _("Terminal is too small. Minimum terminal "
				"size for abook is "
				"%dx%d\n"), min_cols(), min_lines());
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


static void
draw_contact_header(const char *context, bool browsing)
{
	const char *title = "JaroMail v" JARO_MAIL_VERSION;
	const char *pending = _("Unsaved changes");
	int title_end = db_need_save ? max(1, COLS - strwidth(pending) - 1) : COLS;
	wattrset(top, COLOR_PAIR(CP_HEADER));
	werase(top);
	wattron(top, A_BOLD);
	mvwaddnstr(top, 0, 0, title, bytes2width(title, title_end - 1));
	wattroff(top, A_BOLD);
	if(db_need_save)
		mvwaddnstr(top, 0, title_end, pending, bytes2width(pending, COLS - title_end - 1));
	if(context) {
		mvwaddnstr(top, 1, 0, context, bytes2width(context, COLS - 1));
		return;
	}

	const char *label = _("List: ");
	int col = strwidth(label);
	char count[96] = "";
	if(browsing) {
		int marked = selected_items();
		snprintf(count, sizeof count, _("%d/%d contacts"), list_visible_count(), db_n_items());
		if(marked) {
			size_t size = strlen(count);
			snprintf(count + size, sizeof count - size, _("  %d marked"), marked);
		}
	}
	int end = max(col + 1, COLS - strwidth(count) - 1);
	char *safe = jaro_contact_display_text((struct jaro_contact_text){
		jaro_editor_list, strlen(jaro_editor_list)});
	mvwaddstr(top, 1, 0, label);
	if(safe) {
		mvwaddnstr(top, 1, col, safe, bytes2width(safe, end - col - 1));
		free(safe);
	}
	if(*count)
		mvwaddnstr(top, 1, end, count, bytes2width(count, COLS - end - 1));
}

void
headerline(const char *str)
{
	werase(top);

	wattrset(top, COLOR_PAIR(CP_HEADER));
	mvwhline(top, 0, 0, ' ', COLS);
	mvwhline(top, 1, 0, UI_HLINE_CHAR, COLS);

	if(jaro_editor_profile)
		draw_contact_header(str, FALSE);
	else
		mvwprintw(top, 0, 0, "%s | %s", PACKAGE " " VERSION, str);

	refresh();
	wrefresh(top);
}

/* Prefer the configured binding even when the default key still works. */
char
ui_binding(char command)
{
	const char *remap = get_remap_table();
	for(int key = ' '; key < 127; key++)
		if(key != command && remap[key] == command)
			return key;
	return command;
}

int
ui_emacs_key(int key)
{
	switch(key) {
		case 1:  return KEY_HOME;  /* Ctrl+A */
		case 2:  return KEY_LEFT;  /* Ctrl+B */
		case 5:  return KEY_END;   /* Ctrl+E */
		case 6:  return KEY_RIGHT; /* Ctrl+F */
		case 14: return KEY_DOWN;  /* Ctrl+N */
		case 16: return KEY_UP;    /* Ctrl+P */
		case 22: return KEY_NPAGE; /* Ctrl+V */
		default: return key;
	}
}

void
ui_browse_footer(void)
{
	draw_contact_header(NULL, TRUE);
	wnoutrefresh(top);
	werase(bottom);
	wattrset(bottom, COLOR_PAIR(CP_FOOTER));
	if(entering_query || *contact_query) {
		const char *tail = contact_query;
		while(strwidth(tail) > COLS - 4) {
			tail++;
			while(((unsigned char)*tail & 0xc0) == 0x80)
				tail++;
		}
		mvwprintw(bottom, 1, 0, "%c %s", ui_binding('/'), tail);
		char *keys = entering_query ? xstrdup(_("Enter edit  Tab mailboxes  Esc clear  Up/Down")) :
			strdup_printf(_("Enter edit  Tab mailboxes  Esc clear  %c help"), ui_binding('?'));
		mvwaddnstr(bottom, 0, 0, keys, bytes2width(keys, COLS - 1));
		free(keys);
		if(entering_query)
			wmove(bottom, 1, 2 + strwidth(tail));
	} else {
		mvwaddstr(bottom, 0, 0, _("Enter edit  Tab mailboxes  Space mark"));
		char *keys = strdup_printf(_("%c filter  %c lists  %c add  %c save  %c help"),
			ui_binding('/'), ui_binding('o'), ui_binding('a'), ui_binding('w'), ui_binding('?'));
		mvwaddnstr(bottom, 1, 0, keys, bytes2width(keys, COLS - 1));
		free(keys);
	}
	if(!entering_query && (*contact_feedback || selected_items())) {
		wmove(bottom, 0, 0);
		wclrtoeol(bottom);
		const char *message = contact_feedback;
		char *remove = NULL;
		if(!*message)
			message = remove = strdup_printf(_("%c Remove %d marked  Space toggle mark"),
				ui_binding('d'), selected_items());
		mvwaddnstr(bottom, 0, 0, message, bytes2width(message, COLS - 1));
		free(remove);
	}
	wnoutrefresh(bottom);
}

void
ui_contact_editor_footer(void)
{
	werase(bottom);
	wattrset(bottom, COLOR_PAIR(CP_FOOTER));
	const char *hint = *contact_feedback ? contact_feedback : _("Up/Down fields  Enter edit  Left/Right views");
	mvwaddnstr(bottom, 0, 0, hint, bytes2width(hint, COLS - 1));
	char *keys = strdup_printf(_("%c save  Ctrl+R reload/discard  %c back  %c help"),
		ui_binding('w'), ui_binding('q'), ui_binding('?'));
	mvwaddnstr(bottom, 1, 0, keys, bytes2width(keys, COLS - 1));
	free(keys);
	wnoutrefresh(bottom);
}

void
ui_contact_draft_changed(void)
{
	*contact_feedback = 0;
}

const char *
ui_contact_field_error(int item)
{
	if(jaro_editor_failed_item() != item || strcmp(contact_feedback, jaro_editor_diagnostic()))
		return NULL;
	const char *key = jaro_editor_failed_field();
	return key && !strcmp(key, "nickname") ? "nick" : key;
}

static void
set_contact_feedback(const char *message)
{
	snprintf(contact_feedback, sizeof contact_feedback, "%s", message);
}

int
ui_save_contacts(void)
{
	int result = jaro_editor_save();
	if(result < 0) {
		int item = jaro_editor_failed_item();
		if(is_valid_item(item))
			list_set_curitem(item);
		set_contact_feedback(jaro_editor_diagnostic());
		return -1;
	}
	set_contact_feedback(result ? _("Saved to this list") : _("No changes to save"));
	return 0;
}

static int
contact_prompt(const char *question, const char *choices)
{
	werase(bottom);
	wattrset(bottom, COLOR_PAIR(CP_FOOTER));
	mvwaddnstr(bottom, 0, 0, question, bytes2width(question, COLS - 1));
	mvwaddnstr(bottom, 1, 0, choices, bytes2width(choices, COLS - 1));
	wrefresh(bottom);
	int choice = getch();
	return choice >= 0 && choice <= UCHAR_MAX ? tolower(choice) : choice;
}

static bool
confirm_contact_discard(const char *question)
{
	return contact_prompt(question, _("y discard  Esc keep editing")) == 'y';
}

int
ui_reload_contacts(bool confirm)
{
	bool discarded = db_need_save;
	if(confirm && discarded && !confirm_contact_discard(_("Discard draft and reload this list?")))
		return -1;
	if(jaro_editor_load()) {
		set_contact_feedback(jaro_editor_diagnostic());
		return -1;
	}
	set_contact_feedback(discarded ? _("Reloaded this list") : "");
	return 0;
}

enum ui_contact_leave
ui_leave_contacts(bool discard)
{
	if(!db_need_save)
		return UI_CONTACT_LEAVE;
	if(discard)
		return confirm_contact_discard(_("Discard unsaved changes and quit?")) ?
			UI_CONTACT_DISCARD : UI_CONTACT_STAY;
	if(!opt_get_bool(BOOL_AUTOSAVE)) {
		int choice = contact_prompt(_("This list has unsaved changes"),
			_("s save  d discard  Esc keep editing"));
		if(choice == 'd')
			return UI_CONTACT_DISCARD;
		if(choice != 's')
			return UI_CONTACT_STAY;
	}
	return ui_save_contacts() ? UI_CONTACT_STAY : UI_CONTACT_LEAVE;
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

	if(jaro_editor_profile) {
		headerline(NULL);
		list_update_matches();
		list_headerline();
		refresh_list();
		return;
	}

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
ui_handle_resize(void)
{
	if(!should_resize)
		return 0;
	refresh_screen();
	return 1;
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
		ch = getch();
		if(ch < 0 || ch > UCHAR_MAX)
			continue;
		ch = tolower(ch);

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

	wmove(bottom, 1, 0);
	wclrtoeol(bottom);
	mvwaddstr(bottom, 1, 0, prompt);

	getyx(bottom, y, x);

	ret = abook_readline(bottom, y, x, s, use_completion, jaro_editor_profile && !limit);

	if(ret) {
		if(!jaro_editor_profile || limit)
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

static void
display_contact_help(void)
{
	erase();
	headerline(_("Contacts help"));
	mvprintw(2, 0, _("Arrows, %c/%c or Ctrl+P/N: move"), ui_binding('j'), ui_binding('k'));
	mvaddstr(3, 0, _("Enter: edit  Tab: inspect mailboxes"));
	mvprintw(4, 0, _("%c: filter as you type  Esc: clear"), ui_binding('/'));
	mvprintw(5, 0, _("%c: choose list  %c: load/reload"), ui_binding('o'), ui_binding('l'));
	mvprintw(6, 0, _("Space: mark  %c: remove marked or current"), ui_binding('d'));
	mvprintw(7, 0, _("%c: save  %c: quit  %c: discard and quit"), ui_binding('w'), ui_binding('q'), ui_binding('Q'));
	mvprintw(8, 0, _("%c: add  %c: duplicate  %c: merge"), ui_binding('a'), ui_binding('D'), ui_binding('M'));
	mvprintw(9, 0, _("%c: compose  %c: open URL  %c: sort"), ui_binding('m'), ui_binding('v'), ui_binding('s'));
	mvaddstr(LINES - 1, 0, _("Press any key to return"));
	refresh();
	getch();
}

void
display_help(int help)
{
	int i;
	char **tbl;
	WINDOW *helpw;

	switch(help) {
		case HELP_MAIN:
			if(jaro_editor_profile) {
				display_contact_help();
				return;
			}
			tbl = mainhelp;
			break;
		case HELP_EDITOR:
			if(jaro_editor_profile) {
				erase();
				headerline(_("Editor keys"));
				mvaddstr(2, 0, _("Up/Down, Tab or Ctrl+P/N: focus field"));
				mvaddstr(3, 0, _("Enter or 1-9/A-Z: edit field"));
				mvprintw(4, 0, _("%c/%c or Ctrl+B/F: change view"), ui_binding('h'), ui_binding('l'));
				mvprintw(5, 0, _("%c/%c: previous/next contact"), ui_binding('k'), ui_binding('j'));
				mvprintw(6, 0, _("%c: save  Ctrl+R: reload/discard"), ui_binding('w'));
				mvprintw(7, 0, _("%c: undo  %c: back"), ui_binding('u'), ui_binding('q'));
				mvaddstr(LINES - 1, 0, _("Press any key to return"));
				refresh();
				getch();
				return;
			}
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
					== ui_binding('q'))
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

static void
set_contact_query(void)
{
	list_set_query(contact_query);
	refresh_list();
}

static void
filter_contacts(void)
{
	int action = 0;
	entering_query = TRUE;
	show_cursor();
	for(;;) {
		if(should_resize)
			refresh_screen();
		ui_browse_footer();
		doupdate();
		wtimeout(bottom, 100);
		wint_t key;
		int kind = wget_wch(bottom, &key);
		if(kind == ERR)
			continue;
		size_t length = strlen(contact_query);
		if(kind == OK && key < 32) {
			int movement = ui_emacs_key((int)key);
			if(movement != (int)key) {
				key = movement;
				kind = KEY_CODE_YES;
			}
		}
		if(kind == KEY_CODE_YES && key == KEY_UP)
			scroll_up();
		else if(kind == KEY_CODE_YES && key == KEY_DOWN)
			scroll_down();
		else if(kind == KEY_CODE_YES && key == KEY_HOME)
			goto_home();
		else if(kind == KEY_CODE_YES && key == KEY_END)
			goto_end();
		else if(kind == KEY_CODE_YES && key == KEY_NPAGE)
			page_down();
		else if(kind == KEY_CODE_YES && key == KEY_PPAGE)
			page_up();
		else if((kind == KEY_CODE_YES && key == KEY_ENTER) ||
		        (kind == OK && (key == '\r' || key == '\n'))) {
			action = '\r';
			break;
		} else if(kind == OK && key == '\t') {
			action = '\t';
			break;
		} else if(kind == OK && (key == 27 || key == 7)) {
			*contact_query = 0;
			break;
		} else if((kind == KEY_CODE_YES && key == KEY_BACKSPACE) ||
		       (kind == OK && (key == 8 || key == 127))) {
			if(length) {
				do length--;
				while(((unsigned char)contact_query[length] & 0xc0) == 0x80);
				contact_query[length] = 0;
				set_contact_query();
			}
		} else if(kind == OK && key == 21) { /* Ctrl+U clears the input. */
			*contact_query = 0;
			set_contact_query();
		} else if(kind == OK && iswprint(key)) {
			mbstate_t state = {0};
			char character[MB_LEN_MAX];
			size_t size = wcrtomb(character, (wchar_t)key, &state);
			if(size != (size_t)-1 && size < sizeof contact_query - length) {
				memcpy(contact_query + length, character, size);
				contact_query[length + size] = 0;
				set_contact_query();
			}
		}
	}
	wtimeout(bottom, -1);
	entering_query = FALSE;
	if(!opt_get_bool(BOOL_SHOW_CURSOR))
		hide_cursor();
	set_contact_query();
	if(action == '\r')
		edit_item(-1);
	else if(action == '\t')
		inspect_contact_mailboxes();
}

static abook_list *
contact_menu(const char *title, abook_list *rows, const char *active, const char *actions)
{
	const char *remap = get_remap_table();
	int count = 0, choice = 0, offset = 0, column_offset = 0;
	abook_list *result = NULL;
	for(abook_list *row = rows; row; row = row->next, count++)
		if(active && !strcmp(row->data, active))
			choice = count;

	wtimeout(stdscr, 100);
	hide_cursor();
	for(;;) {
		if(should_resize)
			refresh_screen();
		int height = LINES - 4;
		if(choice < offset)
			offset = choice;
		else if(choice >= offset + height)
			offset = choice - height + 1;
		erase();
		headerline(title);
		int pos = 0, max_column_offset = 0;
		for(abook_list *row = rows; row && pos < offset + height; row = row->next, pos++) {
			if(pos < offset)
				continue;
			int line = 2 + pos - offset;
			const char *mark = active && !strcmp(row->data, active) ? "*" : " ";
			char *safe = jaro_contact_display_text((struct jaro_contact_text){row->data, strlen(row->data)});
			if(pos == choice)
				attron(A_REVERSE);
			mvprintw(line, 0, "%c%s ", pos == choice ? '>' : ' ', mark);
			if(safe) {
				const char *display = safe;
				if(pos == choice) {
					max_column_offset = max(0, strwidth(safe) - (COLS - 4));
					column_offset = min(column_offset, max_column_offset);
					display += bytes2width(safe, column_offset);
				}
				addnstr(display, bytes2width(display, COLS - 4));
				free(safe);
			}
			attroff(A_REVERSE);
		}
		if(!count)
			mvaddstr(2, 1, _("No mailboxes"));
		if(max_column_offset)
			mvaddstr(LINES - 2, 0, _("Left/Right scroll"));
		mvaddnstr(LINES - 1, 0, actions, bytes2width(actions, COLS - 1));
		refresh();

		int key;
		do key = getch();
		while(key == ERR && !should_resize);
		if(key == 27 || key == 7)
			break;
		if(key > 0 && key < 127)
			key = remap[key];
		key = ui_emacs_key(key);
		if(key == 'q')
			break;
		if(key == '\r' || key == '\n' || key == KEY_ENTER) {
			if(count)
				result = abook_list_get(rows, choice);
			break;
		}
		if((key == KEY_UP || key == 'k') && choice > 0) {
			choice--;
			column_offset = 0;
		} else if((key == KEY_DOWN || key == 'j') && choice + 1 < count) {
			choice++;
			column_offset = 0;
		} else if(key == KEY_HOME || key == KEY_END || key == KEY_NPAGE || key == KEY_PPAGE) {
			if(count) {
				if(key == KEY_HOME) choice = 0;
				if(key == KEY_END) choice = count - 1;
				if(key == KEY_NPAGE) choice = min(count - 1, choice + height);
				if(key == KEY_PPAGE) choice = max(0, choice - height);
				column_offset = 0;
			}
		} else if(key == KEY_LEFT || key == 'h') {
			column_offset = max(0, column_offset - 8);
		} else if(key == KEY_RIGHT || key == 'l') {
			column_offset = min(max_column_offset, column_offset + 8);
		}
	}
	wtimeout(stdscr, -1);
	refresh_screen();
	return result;
}

static void
inspect_contact_mailboxes(void)
{
	int item = list_get_curitem();
	if(!is_valid_item(item))
		return;
	abook_list *addresses = mailbox_list_from_csv(safe_str(db_fget(item, EMAIL)));
	char *title = strdup_printf(_("Mailboxes for %s"), db_name_get(item));
	char *safe = jaro_contact_display_text((struct jaro_contact_text){title, strlen(title)});
	contact_menu(safe ? safe : _("Mailboxes"), addresses, NULL, _("Up/Down move  Enter/Esc back"));
	free(safe);
	free(title);
	abook_list_free(&addresses);
}

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
		if(jaro_editor_profile)
			wtimeout(stdscr, 100);
		do ch = getch();
		while(jaro_editor_profile && ch == ERR && !should_resize);
		if(jaro_editor_profile)
			wtimeout(stdscr, -1);
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
					int item = list_visible_item(event.y + list_get_firstitem() - LIST_TOP);
					if(jaro_editor_profile && (event.y < LIST_TOP || event.y >= LIST_BOTTOM ||
					   event.x >= list_browse_width() || item < 0))
						continue;
					list_set_curitem(item);
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

		if (ch > 0 && ch < 127)
			ch = remap_table[ch];
		if(jaro_editor_profile)
			ch = ui_emacs_key(ch);

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
			case KEY_ENTER:
			case '\n':
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

			case 'w': save_database(0);
				  if(jaro_editor_profile) refresh_screen();
				  break;
			case 'l': ui_read_database();	break;
			case 'i': import_database();	break;
			case 'e': export_database();	break;
			case 'C': ui_clear_database();	break;

			case 'o': ui_open_datafile();	break;

			case 's': sort_by_field("name");break;
			case 'S': sort_surname();	break;
			case 'F': sort_by_field(NULL);	break;

			case '/': if(jaro_editor_profile) filter_contacts();
				  else ui_find(0);
				  break;
			case '\t': if(jaro_editor_profile) inspect_contact_mailboxes();
				   break;
			case 7:
			case 27: if(jaro_editor_profile && *contact_query) {
					*contact_query = 0;
					set_contact_query();
				 }
				 break;
			case 'n':
			case '\\': if(jaro_editor_profile) scroll_down();
				   else ui_find(1);
				   break;

			case ' ': if(list_get_curitem() >= 0) {
				   list_invert_curitem_selection();
				   ui_contact_draft_changed();
				   ui_print_number_of_items();
				   refresh_list();
				  }
				break;
			case '+': select_all();
				  ui_contact_draft_changed();
				  refresh_list();
				break;
			case '-': select_none();
				  ui_contact_draft_changed();
				  refresh_list();
				break;
			case '*': invert_selection();
				  ui_contact_draft_changed();
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
	if(jaro_editor_profile) {
		int count = selected_items();
		if(!count && !is_valid_item(list_get_curitem()))
			return;
		int visible = 0;
		for(int row = 0; row < list_visible_count(); row++)
			visible += !!is_selected(list_visible_item(row));
		int hidden = count - visible;
		count = max(count, 1);
		char *question = hidden ? strdup_printf(_("Remove %d here (%d hidden by filter)?"), count, hidden) :
			strdup_printf(count == 1 ? _("Remove %d contact from this list?") :
				_("Remove %d contacts from this list?"), count);
		if(contact_prompt(question, _("y confirm  Esc cancel")) == 'y') {
			remove_selected_items();
			set_contact_feedback(_("Removal pending; save to commit"));
		}
		free(question);
		list_update_matches();
		refresh_list();
		return;
	}
	if(!list_visible_count())
		return;

	if(statusline_ask_boolean(_("Remove selected item(s)"), FALSE))
		remove_selected_items();

	clear_statusline();
	list_update_matches();
	refresh_list();
}

void
ui_merge_items()
{
	if(jaro_editor_profile) {
		int count = selected_items();
		if(count < 2)
			return;
		char *question = strdup_printf(_("Merge %d contacts here? Removes %d."), count, count - 1);
		if(contact_prompt(question, _("y confirm  Esc cancel")) == 'y') {
			merge_selected_items();
			set_contact_feedback(_("Merge pending; save to commit"));
		}
		free(question);
		list_update_matches();
		refresh_list();
		return;
	}
	if(statusline_ask_boolean(_("Merge selected items"), FALSE))
		merge_selected_items();

	clear_statusline();
	list_update_matches();
	refresh_list();
}

void		ui_remove_duplicates()
{
	if(jaro_editor_profile) {
		int count = count_duplicate_items();
		if(!count)
			return;
		char *question = strdup_printf(count == 1 ? _("Remove %d duplicate from this list?") :
			_("Remove %d duplicates from this list?"), count);
		if(contact_prompt(question, _("y confirm  Esc cancel")) == 'y') {
			remove_duplicates();
			select_none();
			set_contact_feedback(_("Removal pending; save to commit"));
		}
		free(question);
		list_update_matches();
		refresh_list();
		return;
	}
	if(statusline_ask_boolean(_("Remove duplicates"), FALSE))
		remove_duplicates();
	
	clear_statusline();
	list_update_matches();
	refresh_list();
}

void
ui_clear_database()
{
	if(jaro_editor_profile) {
		int count = db_n_items();
		if(!count)
			return;
		char *question = strdup_printf(_("Remove all %d contacts from this list?"), count);
		if(contact_prompt(question, _("y confirm  Esc cancel")) == 'y') {
			close_database();
			db_need_save = TRUE;
			set_contact_feedback(_("Removal pending; save to commit"));
		}
		free(question);
		list_update_matches();
		refresh_list();
		return;
	}
	if(statusline_ask_boolean(_("Clear WHOLE database"), FALSE)) {
		close_database();
		db_need_save = TRUE;
		list_update_matches();
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
	if(jaro_editor_profile)
		return;
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

	if(jaro_editor_profile) {
		ui_reload_contacts(TRUE);
		refresh_screen();
		return;
	}
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
	if (jaro_editor_profile) {
		abook_list *books;
		if(jaro_editor_books(&books)) {
			set_contact_feedback(jaro_editor_diagnostic());
			refresh_screen();
			return;
		}
		abook_list *choice = contact_menu(_("Contact lists"), books, jaro_editor_list,
			_("Up/Down move  Enter open  Esc back"));
		if(choice && strcmp(choice->data, jaro_editor_list) &&
		   ui_leave_contacts(FALSE) != UI_CONTACT_STAY) {
			if(jaro_editor_switch_book(choice->data))
				set_contact_feedback(jaro_editor_diagnostic());
			else {
				*contact_query = 0;
				list_set_query(contact_query);
				ui_contact_draft_changed();
			}
		}
		abook_list_free(&books);
		refresh_screen();
		return;
	}

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
