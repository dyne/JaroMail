
/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include "abook_curses.h"
#include "ui.h"
#include "abook.h"
#include "database.h"
#include "gettext.h"
#include "list.h"
#include "edit.h"
#include "misc.h"
#include "views.h"
#include "xmalloc.h"
#include "color.h"
#include "remap.h"
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#if defined(HAVE_LOCALE_H) && defined(HAVE_SETLOCALE)
#       include <locale.h>
#endif 


static void locale_date(char *str, size_t str_len, int year, int month, int day);

/*
 * some extern variables
 */

extern int views_count;
extern bool db_need_save;

WINDOW *editw;


static void
editor_tab(const int tab)
{
	int i, j;
	int x_pos = 2; /* current x pos */
	char *tab_name;

	wattrset(editw, COLOR_PAIR(CP_TAB_BORDER));
	mvwhline(editw, TABLINE + 1, 0, UI_HLINE_CHAR, EDITW_COLS);

	for(i = 0; i < views_count; i++) {
		view_info(i, &tab_name, NULL);
		int width = strwidth(tab_name) + 5;

		if(x_pos + width + 1 > EDITW_COLS) {
			statusline_addstr(_("Tab name too wide for screen"));
			break;
		}

		mvwaddch(editw,  TABLINE + 1, x_pos,  UI_TEE_CHAR);
		mvwaddch(editw,  TABLINE + 1, x_pos + width - 2, UI_TEE_CHAR);

		mvwaddch(editw,  TABLINE, x_pos,  UI_ULCORNER_CHAR);
		mvwaddch(editw,  TABLINE, x_pos + 1,  UI_LBOXLINE_CHAR);
		wattrset(editw, COLOR_PAIR(CP_TAB_LABEL));
		mvwaddstr(editw, TABLINE, x_pos + 2,  tab_name);
		wattrset(editw, COLOR_PAIR(CP_TAB_BORDER));
		mvwaddch(editw,  TABLINE, x_pos + width - 3, UI_RBOXLINE_CHAR);
		mvwaddch(editw,  TABLINE, x_pos + width - 2, UI_URCORNER_CHAR);

		if(i == tab) {
			mvwaddch(editw,  TABLINE + 1, x_pos, UI_LRCORNER_CHAR);
			for(j = 0; j < width - 3; j++)
				mvwaddstr(editw,
					TABLINE + 1, x_pos + j + 1, " ");
			mvwaddch(editw,  TABLINE + 1, x_pos + width - 2,
				UI_LLCORNER_CHAR);
		}
		x_pos += width;
	}
}

void
get_first_email(char *str, int item)
{
	char *tmp, *emails = db_email_get(item);

	if(!*emails) {
		*str = 0;
		return;
	}

	strncpy(str, emails, MAX_EMAIL_LEN);
	free(emails);
	if( (tmp = strchr(str, ',')) )
		*tmp = 0;
	else
		str[MAX_EMAIL_LEN - 1] = 0;
}

/* This only rolls emails from the 'email' field, not emails from any
 * field of type FIELD_EMAILS.
 * TODO: expand to ask for which field to roll if several are present? */
void
roll_emails(int item, enum rotate_dir dir)
{
	abook_list *emails = csv_to_abook_list(db_fget(item, EMAIL));

	if(!emails)
		return;

	free(db_fget(item, EMAIL));
	abook_list_rotate(&emails, dir);
	db_fput(item, EMAIL, abook_list_to_csv(emails));
	abook_list_free(&emails);
}

static void
init_editor()
{
	clear();
	editw = newwin(EDITW_LINES, EDITW_COLS, EDITW_TOP, EDITW_X);
	notimeout(editw, TRUE); /* handling of escape key */

	refresh_statusline();
}

enum {
	BACKUP_ITEM,
	RESTORE_ITEM,
	CLEAR_UNDO
};

static int
edit_undo(int item, int mode)
{
	static list_item backup = NULL;
	static int backed_up_item = -1;

	switch(mode) {
		case CLEAR_UNDO:
			if(backup) {
				item_empty(backup);
				item_free(&backup);
			}
			break;
		case BACKUP_ITEM:
			if(backup) {
				item_empty(backup);
				item_free(&backup);
			}
			backup = item_create();
			item_duplicate(backup, db_item_get(item));
			backed_up_item = item;
			break;
		case RESTORE_ITEM:
			if(backup) {
				item_empty(db_item_get(backed_up_item));
				item_copy(db_item_get(backed_up_item), backup);
				item_free(&backup);
				return backed_up_item;
			}
			break;
		default:
			assert(0);
	}
	return item;
}

static void
close_editor()
{
	edit_undo(-1, CLEAR_UNDO);
	delwin(editw);
	refresh_screen();
}

static void
print_editor_header(int item)
{
	char *header;
	char email[MAX_EMAIL_LEN];

	if((header = xmalloc(EDITW_COLS)) == NULL)
		return;

	get_first_email(email, item);

	if(*email)
		snprintf(header, EDITW_COLS, "%s <%s>",
				db_name_get(item),
				email);
	else
		snprintf(header, EDITW_COLS, "%s", db_name_get(item));

	wattrset(editw, COLOR_PAIR(CP_TAB_LABEL));
	mvwaddstr(editw, 0, (EDITW_COLS - strwidth(header)) / 2, header);

	free(header);
}

static void
editor_print_data(int tab, int item)
{
	int j = 1, nb;
	int y, x;
	abook_field_list *cur;
	char *str;

	view_info(tab, NULL, &cur);

	for(; cur; cur = cur->next) {

		if(j > 1) {
			getyx(editw, y, x);
			y++;
		} else
			y = FIELDS_START_Y;

		wattrset(editw, COLOR_PAIR(CP_FIELD_NAME));
		mvwprintw(editw, y, FIELDS_START_X, "%c - ",
				(j < 10) ? '0' + j : 'A' + j - 10);
		mvwaddnstr(editw, y, FIELDS_START_X + 4, cur->field->name,
				bytes2width(cur->field->name,
					FIELDNAME_MAX_WIDTH));
		mvwaddch(editw, y, TAB_COLON_POS, ':');

		wattrset(editw, COLOR_PAIR(CP_FIELD_VALUE));
		if((cur->field->type == FIELD_EMAILS) ||
				(cur->field->type == FIELD_LIST)) {
			abook_list *emails, *e;
			
			find_field_number(cur->field->key, &nb);
			emails = csv_to_abook_list(db_fget_byid(item, nb));

			for(e = emails; e; e = e->next) {
				getyx(editw, y, x);
				mvwaddnstr(editw, y + 1, TAB_COLON_POS + 2,
						e->data,
						bytes2width(e->data,
							FIELD_MAX_WIDTH));
				mvwaddch(editw, y + 1, TAB_COLON_POS,
						UI_VLINE_CHAR);
			}
			if(emails) {
				mvwaddch(editw, y + 2, TAB_COLON_POS,
						UI_LLCORNER_CHAR);
				mvwhline(editw, y + 2, TAB_COLON_POS + 1,
						UI_HLINE_CHAR,
						EDITW_COLS - TAB_COLON_POS - 2);
			}
			abook_list_free(&emails);
		} else if(cur->field->type == FIELD_DATE) {
			int day, month, year;
			char buf[64];

			find_field_number(cur->field->key, &nb);
			str = db_fget_byid(item, nb);
			
			if(parse_date_string(str, &day, &month, &year)) {
				/* put locale representation of date in buf */
				locale_date(buf, sizeof(buf), year, month, day);
				mvwaddnstr(editw, y, TAB_COLON_POS + 2, buf,
					bytes2width(buf, FIELD_MAX_WIDTH));
			}
		} else {
			find_field_number(cur->field->key, &nb);
			str = safe_str(db_fget_byid(item, nb));
			mvwaddnstr(editw, y, TAB_COLON_POS + 2, str,
				bytes2width(str, FIELD_MAX_WIDTH));
		}

		j++;
	}
}

/*
 * function: change_field
 *
 * parameters:
 *  (char *msg)
 *   message to display as a prompt
 *  (char **field)
 *   a pointer to a pointer which will point a new string. if the latter
 *   pointer != NULL it will be freed (if user doesn't cancel)
 *  (size_t max_len)
 *   maximum length of field to read from user
 *
 * returns (int)
 *  a nonzero value if user has cancelled and zero if user has typed a
 *  valid string
 */
static int
change_field(char *msg, char **field, size_t max_len)
{
	char *old;
	int ret = 0;

	old = *field;

	*field = ui_readline(msg, old, max_len - 1, 0);

	if(*field) {
		db_need_save = TRUE;
		xfree(old);
		if(!**field)
			xfree(*field);
	} else {
		*field = old;
		ret = 1;
	}

	clear_statusline();
	refresh_statusline();

	return ret;
}

static int
change_name_field(char *msg, char **field, size_t max_len)
{
	char *tmp;
	int ret;

	tmp = xstrdup(*field);
	ret = change_field(msg, field, max_len);

	if(*field == NULL || ! **field) {
		xfree(*field);
		*field = xstrdup(tmp);
	}

	xfree(tmp);

	return ret;
}

static void
fix_email_str(char *str)
{
	for(; *str; str++)
		*str = *str == ',' ? '_' : *str;
}

static void
edit_list(int item, int nb, int isemail)
{
	char *field, *msg, *keys;
	abook_list *list, *e;
	int choice = 1, elem_count;

	list = csv_to_abook_list(db_fget_byid(item, nb));

	for(e = list, elem_count = 0; e; e = e->next, elem_count++)
		;

	if(elem_count) {
		keys = xstrndup(S_("keybindings_new_123456789|n123456789"),
				elem_count + 1);
		msg = strdup_printf(_("Choose %s to modify (<1>%s%c%s%s."),
				isemail ? _("email") : _("item"),
				(elem_count > 1) ? "-<" : "",
				(elem_count > 1) ?  '0' + elem_count : ')',
				(elem_count > 1) ? ">)" : "",
				(elem_count < MAX_LIST_ITEMS) ?
					_(" or <n>ew") : ""
				);
		choice = statusline_askchoice(
				msg,
				keys,
				(elem_count < MAX_LIST_ITEMS) ? 1 : 2
				);
		free(keys);
		free(msg);
	}

	if(choice == 0)
		return;

	field = (choice > 1) ?
		xstrdup(abook_list_get(list, choice - 2)->data) :
		NULL;

	if(change_field(isemail ? _("E-mail: ") : _("Item: "),
				&field, MAX_EMAIL_LEN))
		return; /* user cancelled ( C-g ) */

	/* TODO if list item contains commas, should use quotes instead */
	if(field)
		fix_email_str(field);

	if(choice == 1)
		abook_list_append(&list, field);
	else
		abook_list_replace(&list, choice - 2, field);

	if(field)
		xfree(field);

	field = abook_list_to_csv(list);
	db_fput_byid(item, nb, field ? field : xstrdup(""));
	abook_list_free(&list);
}

/*
 * available %-sequences:
 *   - %y, %Y, %m, %M, %d, %D represent year, month, and day
 *     (the uppercase version telling to fill with leading zeros
 *     if necessary)
 *   - %I for ISO 8601 representation
 */
static size_t
format_date(char *str, size_t str_len, char *fmt, int year, int month, int day)
{
	char *s = str;
	size_t len;

	while(*fmt && (s - str + 1 < str_len)) {
		if(*fmt != '%') {
			*s++ = *fmt++;
			continue;
		}

		len = str_len - (str - s);
		switch(*++fmt) {
			case 'y': s += snprintf(s, len, "%d", year); break;
			case 'Y': s += snprintf(s, len, "%04d", year); break;
			case 'm': s += snprintf(s, len, "%d", month); break;
			case 'M': s += snprintf(s, len, "%02d", month); break;
			case 'd': s += snprintf(s, len, "%d", day); break;
			case 'D': s += snprintf(s, len, "%02d", day); break;
			case 'I': s += format_date(s, len,
						  year ? "%Y-%M-%D" : "--%M-%D",
						  year, month, day);
				  break;
			case '%': *s++ = '%'; break;
			default: *s++ = '%'; *s++ = *fmt; break;
		}
		fmt++;
	}
	*s = 0;
	return s - str;
}

/*
 * str is a buffer of max length str_len, which, after calling, will
 * contain a representation of the given [y, m, d] date using the
 * current locale (as defined by LC_TIME).
 *
 * In the absence of any localization, use an ISO 8601 representation.
 */
static void
locale_date(char *str, size_t str_len, int year, int month, int day)
{
	char *fmt;

#if defined(HAVE_LOCALE_H) && defined(HAVE_SETLOCALE)
	fmt = year ?	dcgettext(PACKAGE, "%Y-%M-%D", LC_TIME) :
			dcgettext(PACKAGE, "--%M-%D", LC_TIME);
#else
	fmt = "%I";
#endif
	format_date(str, str_len, fmt, year, month, day);
}

static int is_valid_date(const int day, const int month, const int year)
{
	int valid = 1;
	int month_length[13] =
		{ 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	/*
	 * leap year
	 */
	if ((!(year % 4)) && ((year % 100) || !(year % 400)))
		month_length[2] = 29;

	if (month < 1 || month > 12)
		valid = 0;
	else if (day < 1 || day > month_length[month])
		valid = 0;
	else if (year < 0) /* we don't accept negative year numbers */
		valid = 0;

	return valid;
}

int
parse_date_string(char *str, int *day, int *month, int *year)
{
	int i = 0;
	char buf[12], *s, *p;

	assert(day && month && year);

	if(!str || !*str)
		return FALSE;

	p = s = strncpy(buf, str, sizeof(buf));

	if(*s == '-' && *s++ == '-') { /* omitted year */
		*year = 0;
		p = ++s;
		i++;
	}

	while(*s) {
		if(isdigit(*s)) {
			s++;
			continue;
		} else if(*s == '-') {
			if(++i > 3)
				return FALSE;
			*s++ = '\0';
			switch(i) {
				case 1: *year = safe_atoi(p); break;
				case 2: *month = safe_atoi(p); break;
			}
			p = s;
		} else
			return FALSE;
	}

	if (i != 2 || !*p)
		return FALSE;

	*day = atoi(p);

	return is_valid_date(*day, *month, *year);
}

static void
edit_date(int item, int nb)
{
	int i, date[3], old;
	char *s = db_fget_byid(item, nb);
	char *field[] = { N_("Day: "), N_("Month: "), N_("Year (optional): ") };

	old = parse_date_string(s, &date[0], &date[1], &date[2]);

	for(i = 0; i < 3; i++) {
		s = (old && date[i]) ? strdup_printf("%d", date[i]) : NULL;
		if(change_field(gettext(field[i]), &s, 5))
			return; /* user aborted with ^G */

		date[i] = (s && is_number(s)) ? atoi(s) : 0;

		if(!s) {
			switch(i) {
				case 0:	db_fput_byid(item, nb, NULL); /*delete*/
				case 1: /* fall through */ return;
			}
		} else
			xfree(s);
	}

	/* ISO 8601 date, of the YYYY-MM-DD or --MM-DD format */
	if(is_valid_date(date[0], date[1], date[2])) {
		if(date[2])
			s = strdup_printf("%04d-%02d-%02d",
				date[2], date[1], date[0]);
		else
			s = strdup_printf("--%02d-%02d", date[1], date[0]);

		db_fput_byid(item, nb, xstrdup(s));
	} else
		statusline_msg(_("Invalid date"));
}

/* input range: 1-9A-Z
 * output range: 0-34 */
static int
key_to_field_number(char c)
{
	int n = c - '1';
	if(n >= 0 && n < 9)
		return n;

	n = c - 'A' + 9;
	if(n > 8 && n < 35)
		return n;

	return -1;
}

static void
edit_field(int tab, char c, int item_number)
{
	ui_enable_mouse(FALSE);
	int i = 0, number, idx;
	char *msg;
	abook_field_list *f;
	list_item item;

	if((number = key_to_field_number(c)) < 0)
		goto detachfield;

	edit_undo(item_number, BACKUP_ITEM);

	view_info(tab, NULL, &f);

	while(1) {
		if(!f)
			goto detachfield;

		if(i == number)
			break;

		f = f->next;
		i++;
	}

	find_field_number(f->field->key, &idx);
	
	switch(f->field->type) {
		case FIELD_STRING:
			msg = strdup_printf("%s: ", f->field->name);
			item = db_item_get(item_number);
			if(strcmp(f->field->key, "name") == 0)
				change_name_field(msg,&item[idx],MAX_FIELD_LEN);
			else
				change_field(msg,&item[idx],MAX_FIELD_LEN);
			free(msg);
			break;
		case FIELD_LIST:
			edit_list(item_number, idx, 0);
			break;
		case FIELD_EMAILS:
			edit_list(item_number, idx, 1);
			break;
		case FIELD_DATE:
			edit_date(item_number, idx);
			goto detachfield;
		default:
			assert(0);
	}

 detachfield:
	if(opt_get_bool(BOOL_USE_MOUSE))
	  ui_enable_mouse(TRUE);
}

static int
edit_loop(int item)
{
	static int tab = 0; /* first tab */
	int c;

	werase(editw);
	headerline(gettext(EDITOR_HELPLINE));
	refresh_statusline();
	print_editor_header(item);
	editor_tab(tab);
	editor_print_data(tab, item);
	wmove(editw, EDITW_LINES - 1, EDITW_COLS - 1);

	refresh();
	wrefresh(editw);

	c = getch();
	if(c == '\033') {
		statusline_addstr("ESC-");
		c = getch();
		clear_statusline();

		/* Escaped bindings */
		switch(c) {
			case 'r': roll_emails(item, ROTATE_RIGHT); break;
			default: break;
		}

		return item;
	}
	if(c == KEY_MOUSE) {
		MEVENT event;
		if(getmouse(&event) == OK) {
			if(event.bstate & BUTTON1_CLICKED
			   || event.bstate & BUTTON1_DOUBLE_CLICKED) {
				int window_y, window_x;
				getbegyx(editw, window_y, window_x);
				if(event.y == 0) {
					/* if first row is selected, then go back to list */
					return -1;
				} else if(event.y == window_y + TABLINE
				   || event.y == window_y + TABLINE + 1) {
					char* tab_name;
					int mouse_x = event.x;
					int xpos = 2 + 1; /* look at editor_tab() and try out */
					int clicked_tab = 0;
					while(clicked_tab < views_count) {
						view_info(clicked_tab, &tab_name, NULL);
						xpos += strwidth(tab_name) + 5;
						/* fprintf(stderr, "trying tab %d\n", clicked_tab); */
						if(xpos >= mouse_x) {
							break; /* clicked tab was found */
						} else {
							/* try next tab */
							clicked_tab++;
						}
					}
					if(clicked_tab < views_count) {
						tab = clicked_tab;
					}
				} else if(event.y >= window_y + FIELDS_START_Y) {
					/* is mouse in field area? */
					int j = 1 + event.y - window_y - FIELDS_START_Y;
					/* field numbers start with 1, but if j='0', then char='0' */
					/* so fix this, by adding 1 to j */
					int field_char = (j < 10) ? '0' + j : 'A' + j - 10;
					edit_field(tab, field_char, item);
				}
			} else if(event.bstate & BUTTON4_PRESSED) {
				tab = tab == 0 ? views_count - 1 : tab - 1;
			}
			else if(event.bstate & BUTTON5_PRESSED) {
				tab = tab == views_count - 1 ? 0 : tab + 1;
			}
			return item;
		}
	}

	const char *remap_table = get_remap_table();
	if (c > 0 && c <= 127)
		c = remap_table[c];

	/* No uppercase nor numeric key should be used in this menu,
	 * as they are reserved for field selection */
	switch(c) {
		case 'h':
		case KEY_LEFT: tab = tab == 0 ? views_count - 1 : tab - 1;
			       break;
		case 'l':
		case KEY_RIGHT: tab = tab == views_count - 1 ? 0 : tab + 1;
				break;
		case KEY_UP:
		case '<':
		case 'k': if(is_valid_item(item - 1)) item--; break;
		case KEY_DOWN:
		case '>':
		case 'j': if(is_valid_item(item + 1)) item++; break;
		case 'r': roll_emails(item, ROTATE_LEFT); break;
		case '?': display_help(HELP_EDITOR); break;
		case 'u': item = edit_undo(item, RESTORE_ITEM); break;
		case 'm': launch_mutt(item); clearok(stdscr, 1); break;
		case 'v': launch_wwwbrowser(item); clearok(stdscr, 1); break;
		case 12 : clearok(stdscr, 1); break; /* ^L (refresh screen) */
		case 'q': return -1;
		default: edit_field(tab, c, item);
	}

	return item;
}

void
edit_item(int item)
{
	if(item < 0) {
		if(list_get_curitem() < 0)
			return;
		else
			item = list_get_curitem();
	}

	init_editor();

	while((item = edit_loop(item)) >= 0)
		list_set_curitem(item); /* this is not very clean way to go */

	close_editor();
}

void
add_item()
{
	char *field = NULL;
	list_item item = item_create();

	change_field(_("Name: "), &field, MAX_FIELD_LEN);

	if( field == NULL )
		return;

	item_fput(item, NAME, field);

	add_item2database(item);
	item_free(&item);

	list_set_curitem(last_item());

	edit_item(last_item());
}

