#ifndef _VIEWS_H
#define _VIEWS_H

#include "database.h"

#define MAX_VIEW_FIELDS 35 /* keybindings for modifying a field: 1-9A-Z */

typedef struct abook_view_t {
	char *name;
	abook_field_list *fields;
	struct abook_view_t *next;
} abook_view;

char *add_field_to_view(char *tabname, char *field);
void view_info(int number, char **name, abook_field_list **fields);
void init_default_views();

#endif /* _VIEWS_H */
