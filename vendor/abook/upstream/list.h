#ifndef _LIST_H
#define _LIST_H

#include "ui.h"

#define INDEX_TEXT  1
#define INDEX_FIELD 2
#define INDEX_ALT_FIELD 3

struct index_elem {
	int type;
	union {
		char *text;
		struct {
			int id;
			int len;
			struct index_elem *next;
		} field;
	} d;
	struct index_elem *next;
};

struct list_field {
	char *data;
	int type;
};

void		init_index();
void		init_list();
int		init_extra_field(enum str_opts option);
void		close_list();
void            refresh_list();
void	get_list_field(int item, struct index_elem *e, struct list_field *res);
void		list_headerline();
void            scroll_up();
void            scroll_down();
void            scroll_list_up();
void            scroll_list_down();
void		page_up();
void		page_down();
void            select_none();
void            select_all();
void		set_selection(int item, int value);
void		list_invert_curitem_selection();
void            move_curitem(int direction);
void		goto_home();
void		goto_end();
int		selected_items();
void		invert_selection();
int		list_is_empty();
int		list_get_curitem();
int		list_get_firstitem();
void		list_set_curitem(int i);
int		duplicate_item();


enum {
	MOVE_ITEM_UP,
	MOVE_ITEM_DOWN
};

#define LIST_TOP        3
#define LIST_BOTTOM     (LINES - 2)

#define LIST_LINES	(LIST_BOTTOM - LIST_TOP)
#define LIST_COLS	COLS

#define LAST_LIST_ITEM	(first_list_item + LIST_LINES - 1)

#endif
