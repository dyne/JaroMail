
/*
 * $Id$
 *
 * by JH <jheinonen@users.sourceforge.net>
 *
 * Copyright (C) Jaakko Heinonen
 */

#include <stdio.h>
#include <string.h>
#include "abook.h"
#include <assert.h>
#include "ui.h"
#include "database.h"
#include "edit.h"
#include "gettext.h"
#include "list.h"
#include "misc.h"
#include "options.h"
#include "xmalloc.h"
#include "color.h"
#include "contact/text.h"
#include "sqlite-editor.h"
#include "search-fold.h"


int curitem = -1;
int first_list_item = -1;
int scroll_speed = 2;
char *selected = NULL;

extern abook_field_list *fields_list;
struct index_elem *index_elements = NULL;

static WINDOW *list = NULL;
static char *query_key;
static int *matches;
static int matches_count;

int
list_visible_count(void)
{
	return query_key ? matches_count : db_n_items();
}

int
list_visible_item(int row)
{
	if(row < 0 || row >= list_visible_count())
		return -1;
	return query_key ? matches[row] : row;
}

static int
visible_row(int item)
{
	if(!query_key)
		return is_valid_item(item) ? item : -1;
	/* Results are in database order. */
	int lo = 0, hi = matches_count;
	while(lo < hi) {
		int mid = lo + (hi - lo) / 2;
		if(matches[mid] < item)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < matches_count && matches[lo] == item ? lo : -1;
}

int
list_adjacent_item(int item, int direction)
{
	return list_visible_item(visible_row(item) + direction);
}

void
list_update_matches(void)
{
	free(matches);
	matches = NULL;
	matches_count = 0;
	if(query_key && db_n_items() > 0) {
		matches = xmalloc((size_t)db_n_items() * sizeof *matches);
		for(int item = 0; item < db_n_items(); item++)
			if(jaro_editor_matches(item, query_key))
				matches[matches_count++] = item;
	}
}

void
list_set_query(const char *query)
{
	free(query_key);
	query_key = *query ? abook_search_key(query) : NULL;
	list_update_matches();
}

int64_t
list_current_contact_id(void)
{
	return is_valid_item(curitem) ? item_contact_id(db_item_get(curitem)) : 0;
}

void
list_restore_contact_id(int64_t id)
{
	if(id)
		for(int i = 0; i < db_n_items(); i++)
			if(item_contact_id(db_item_get(i)) == id) {
				curitem = i;
				return;
			}
}

static int
mailbox_column(void)
{
	return min(26, COLS / 3 + 2);
}


static void
index_elem_add(int type, char *a, char *b)
{
	struct index_elem *tmp = NULL, *cur, *cur2;
	int field, len = 0;

	if(!a || !*a)
		return;

	switch(type) {
		case INDEX_TEXT:
			tmp = xmalloc(sizeof(struct index_elem));
			tmp->d.text = xstrdup(a);
			break;
		case INDEX_FIELD: /* fall through */
		case INDEX_ALT_FIELD:
			find_field_number(a, &field);
			if(field == -1)
				return;
			len = (b && *b && is_number(b)) ? atoi(b) : 0;
			tmp = xmalloc(sizeof(struct index_elem));
			tmp->d.field.id = field;
			tmp->d.field.len = len;
			break;
		default:
			assert(0);
	}
	tmp->type = type;
	tmp->next = NULL;
	tmp->d.field.next = NULL;

	if(!index_elements) { /* first element */
		index_elements = tmp;
		return;
	}

	for(cur = index_elements; cur->next; cur = cur->next)
		;
	if(type != INDEX_ALT_FIELD)
		cur->next = tmp;
	else { /* add as an alternate field */
		tmp->d.field.len = cur->d.field.len;
		for(cur2 = cur; cur2->d.field.next; cur2 = cur2->d.field.next)
			;
		cur2->d.field.next = tmp;
	}
}

static void
parse_index_format(char *s)
{
	char *p, *start, *lstart = NULL;
	int in_field = 0, in_alternate = 0, in_length = 0, type;

	p = start = s;

	while(*p) {
		if(*p == '{' && !in_field) {
			*p = 0;
			index_elem_add(INDEX_TEXT, start, NULL);
			start = ++p;
			in_field = 1;
		} else if(*p == ':' && in_field && !in_alternate) {
			*p = 0;
			lstart = ++p;
			in_length = 1;
		} else if(*p == '|' && in_field) {
			*p = 0;
			type = in_alternate ? INDEX_ALT_FIELD : INDEX_FIELD;
			index_elem_add(type, start, in_length ? lstart : NULL);
			start = ++p;
			in_length = 0;
			in_alternate = 1;
		} else if(*p == '}' && in_field) {
			*p = 0;
			type = in_alternate ? INDEX_ALT_FIELD : INDEX_FIELD;
			index_elem_add(type, start, in_length ? lstart : NULL);
			start = ++p;
			in_field = in_alternate = in_length = 0;
		} else
			p++;
	}
	if(!in_field)
		index_elem_add(INDEX_TEXT, start, NULL);
}

void
init_index()
{
	assert(!index_elements);
	parse_index_format(opt_get_str(STR_INDEX_FORMAT));
}

void
init_list()
{
	list = newwin(LIST_LINES, LIST_COLS, LIST_TOP, 0);
	scrollok(list, TRUE);
	scroll_speed = abs(opt_get_int(INT_SCROLL_SPEED));
}

void
close_list()
{
	delwin(list);
	list = NULL;
}

void
get_list_field(int item, struct index_elem *e, struct list_field *res)
{
	char *s;

	res->data = s = NULL;

	do { /* find first non-empty field data in the alternate fields list */
		s = db_fget_byid(item, e->d.field.id);
	} while(!(s && *s) && ((e = e->d.field.next) != NULL));

	if(!e || !s || !*s)
		return;

	res->data = s;
	get_field_info(e->d.field.id, NULL, NULL, &res->type);
}

static void
print_list_field(int item, int line, int *x_pos, struct index_elem *e)
{
	char *s, *p;
	int width, x_start, mustfree = FALSE, len = abs(e->d.field.len);
	struct list_field f;

	get_list_field(item, e, &f);
	s = f.data;

	if(!s || !*s) {
		*x_pos += len;
		return;
	}
	
	if(f.type == FIELD_EMAILS && !opt_get_bool(BOOL_SHOW_ALL_EMAILS)) {
		if (jaro_editor_profile) {
			abook_list *addresses=mailbox_list_from_csv(s);
			s=xstrdup(addresses ? addresses->data : "");
			mustfree=TRUE;
			abook_list_free(&addresses);
		} else if((p = strchr(s, ',')) != NULL) {
			s = xstrndup(s, p - s);
			mustfree = TRUE;
		}
	}
	if (jaro_editor_profile && f.type!=FIELD_EMAILS) {
		char *safe=jaro_contact_display_text((struct jaro_contact_text){s,strlen(s)});
		if (mustfree) free(s);
		s=safe ? safe : "";
		mustfree=safe!=NULL;
	}

	width = len ? bytes2width(s, len) : strwidth(s);
	x_start = *x_pos + ((e->d.field.len < 0) ? len - width : 0);
	if(width + x_start >= COLS)
		width = bytes2width(s, COLS - x_start);

	if(width)
		mvwaddnstr(list, line, x_start, s, width);

	if(mustfree)
		free(s);
		
	*x_pos += len ? len : width;
}

static void
highlight_line(WINDOW *win, int line)
{
	wattrset(win, COLOR_PAIR(CP_LIST_HIGHLIGHT));
	if(!opt_get_bool(BOOL_USE_COLORS)) {
		wstandout(win);
	}

	/*
	 * this is a tricky one
	 */
#if 0
/*#ifdef mvwchgat*/
	mvwchgat(win, line, 0, -1,  A_STANDOUT, 0, NULL);
#else
	/*
	 * buggy function: FIXME
	 */
	scrollok(win, FALSE);
	{
		int i;
		wmove(win, line, 0);
		for(i = 0; i < COLS; i++)
			waddch(win, ' ');
	/*wattrset(win, 0);*/
	}
	scrollok(win, TRUE);
#endif
}

static void
print_list_line(int item, int line, int highlight)
{
	struct index_elem *cur;
	int x_pos = 1;

	if(item % 2 == 0)
		wattrset(list, COLOR_PAIR(CP_LIST_EVEN));
	else
		wattrset(list, COLOR_PAIR(CP_LIST_ODD));
	scrollok(list, FALSE);
	if(highlight)
		highlight_line(list, line);

	if(selected[item])
		mvwaddch(list, line, 0, '*' );
	else if(jaro_editor_profile && highlight)
		mvwaddch(list, line, 0, '>');

	if(jaro_editor_profile) {
		int col = mailbox_column();
		const char *name = db_name_get(item);
		char *safe = jaro_contact_display_text((struct jaro_contact_text){name, strlen(name)});
		const char *csv = safe_str(db_fget(item, EMAIL));
		abook_list *addresses = mailbox_list_from_csv(csv);
		if(safe) {
			mvwaddnstr(list, line, 1, safe, bytes2width(safe, col - 2));
			free(safe);
		}
		if(addresses)
			mvwaddnstr(list, line, col, addresses->data, bytes2width(addresses->data, COLS - col - 1));
		abook_list_free(&addresses);
	} else for(cur = index_elements; cur; cur = cur->next)
		switch(cur->type) {
			case INDEX_TEXT:
				mvwaddstr(list, line, x_pos, cur->d.text);
				x_pos += strwidth(cur->d.text);
				break;
			case INDEX_FIELD:
				print_list_field(item, line, &x_pos, cur);
				break;
			default:
				assert(0);
		}

	scrollok(list, TRUE);
	if(highlight)
		wstandend(list);
}

static void
draw_list(void)
{
	if(jaro_editor_profile) {
		wnoutrefresh(stdscr);
		wnoutrefresh(list);
		ui_browse_footer();
		doupdate();
	} else {
		refresh();
		wrefresh(list);
	}
}

void
refresh_list()
{
	int i, line, row = visible_row(curitem);

	werase(list);

	ui_print_number_of_items();

	if(!list_visible_count()) {
		curitem = first_list_item = -1;
		if(jaro_editor_profile) {
			const char *empty = query_key ? _("No matching contacts") : _("No contacts in this list");
			mvwaddstr(list, 0, 1, empty);
		}
		draw_list();
		return;
	}

	if(row < 0) {
		row = 0;
		curitem = list_visible_item(0);
	}

	if(first_list_item < 0)
		first_list_item = 0;

	if(row < first_list_item)
		first_list_item = row;
	else if(row > LAST_LIST_ITEM)
		first_list_item = max(row - LIST_LINES + 1, 0);

        for(line = 0, i = first_list_item;
			i <= LAST_LIST_ITEM && i < list_visible_count();
			line++, i++) {

		print_list_line(list_visible_item(i), line, i == row);
        }

	if(opt_get_bool(BOOL_SHOW_CURSOR))
		wmove(list, row - first_list_item, 0);
	draw_list();
}

void
list_headerline()
{
	struct index_elem *e;
	int x_pos = 1, width;
	char *str = NULL;

#if defined(A_BOLD) && defined(A_NORMAL)
	attrset(A_BOLD);
#endif
	attrset(COLOR_PAIR(CP_LIST_HEADER));
	mvhline(2, 0, ' ', COLS);

	if(jaro_editor_profile) {
		mvaddstr(2, 1, _("Name"));
		mvaddstr(2, mailbox_column(), _("Mailbox"));
		attrset(A_NORMAL);
		return;
	}

	for(e = index_elements; e; e = e->next)
		if(e->type == INDEX_TEXT)
			x_pos += strwidth(e->d.text);
		else if(e->type == INDEX_FIELD) {
			get_field_info(e->d.field.id, NULL, &str, NULL);
			width = e->d.field.len ?
				abs(e->d.field.len) : strwidth(str);
			if(width + x_pos > COLS)
				width = bytes2width(str, COLS - x_pos);
			mvaddnstr(2, x_pos, str, width);
			x_pos += width;
		} else
			assert(0);

#if defined(A_BOLD) && defined(A_NORMAL)
	attrset(A_NORMAL);
#endif
}

void
scroll_up()
{
	int item = list_adjacent_item(curitem, -1);
	if(item < 0)
		return;

	curitem = item;

	refresh_list();
}

void
scroll_down()
{
	int item = list_adjacent_item(curitem, 1);
	if(item < 0)
		return;

	curitem = item;

	refresh_list();
}

void
scroll_list_up()
{
	if(query_key) {
		scroll_up();
		return;
	}
	if(first_list_item <= 0) {
		if(curitem != 0) {
			curitem--;
			refresh_list();
		}
		return;
	}

	first_list_item -= scroll_speed;
	if(first_list_item < 0) {
		first_list_item = 0;
	}
	if(curitem > LAST_LIST_ITEM) {
		curitem = LAST_LIST_ITEM;
	}

	refresh_list();
}

void
scroll_list_down()
{
	if(query_key) {
		scroll_down();
		return;
	}
	if(LAST_LIST_ITEM > db_n_items() - 2) {
		if(curitem < LAST_LIST_ITEM) {
			curitem++;
			refresh_list();
		}
		return;
	}

	first_list_item += scroll_speed;
	if(LAST_LIST_ITEM > db_n_items() - 1) {
		first_list_item = db_n_items() - LIST_LINES;
	}
	if(curitem < first_list_item) {
		curitem = first_list_item;
	}

	refresh_list();
}

void
page_up()
{
	int row = visible_row(curitem);
	if(row < 1)
		return;

	curitem = list_visible_item(row == first_list_item ?
		max(row - LIST_LINES, 0) : first_list_item);

	refresh_list();
}

void
page_down()
{
	int row = visible_row(curitem), last = list_visible_count() - 1;
	if(row < 0 || row >= last)
		return;

	curitem = list_visible_item(row == LAST_LIST_ITEM ?
		min(row + LIST_LINES, last) : min(LAST_LIST_ITEM, last));

	refresh_list();
}

void
select_none()
{
        memset(selected, 0, db_n_items());
}

void
select_all()
{
	for(int i = 0; i < list_visible_count(); i++)
		selected[list_visible_item(i)] = 1;
}

void
list_set_selection(int item, int value)
{
	assert(is_valid_item(item));

	selected[item] = !!value;
}

void
list_invert_curitem_selection()
{
	assert(is_valid_item(curitem));

	selected[curitem] = !selected[curitem];
}

void
move_curitem(int direction)
{
        list_item tmp;

        if(curitem < 0 || curitem > last_item())
                return;

	tmp = item_create();
	item_copy(tmp, db_item_get(curitem));

	switch(direction) {
		case MOVE_ITEM_UP:
			if( curitem < 1 )
				goto out_move;
			item_copy(db_item_get(curitem),
					db_item_get(curitem - 1));
			item_copy(db_item_get(curitem-1), tmp);
			curitem--;
			break;

		case MOVE_ITEM_DOWN:
			if(curitem >= last_item())
				goto out_move;
			item_copy(db_item_get(curitem),
					db_item_get(curitem + 1));
			item_copy(db_item_get(curitem + 1), tmp);
			curitem++;
			break;
	}
	list_update_matches();
	refresh_list();

out_move:
	item_free(&tmp);
}

void
goto_home()
{
	curitem = list_visible_item(0);

	refresh_list();
}

void
goto_end()
{
	curitem = list_visible_item(list_visible_count() - 1);

	refresh_list();
}

int
selected_items()
{
	int i, n = 0;

	for(i = 0; i < db_n_items(); i++)
		if(selected[i])
			n++;

	return n;
}

void
invert_selection()
{
	int i;

	if(list_is_empty())
		return;

	for(i = 0; i < list_visible_count(); i++) {
		int item = list_visible_item(i);
		selected[item] = !selected[item];
	}
}

int
list_is_empty()
{
	return db_n_items() < 1;
}

int
list_get_curitem()
{
	return curitem;
}

int
list_get_firstitem()
{
	return first_list_item;
}

void
list_set_curitem(int i)
{
	curitem = i;
}

int
duplicate_item()
{
	list_item item;
	
	if(curitem < 0)
		return 1;

	item = item_create();
	item_duplicate(item, db_item_get(curitem));
	if(add_item2database(item)) {
		item_free(&item);
		return 1;
	}
	item_free(&item);

	curitem = last_item();
	list_update_matches();
	refresh_list();

	return 0;
}

