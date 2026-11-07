#ifndef _EDIT_H
#define _EDIT_H

#include "misc.h"	/* for rotate_dir enum definition */


void		edit_item(int item);
void		get_first_email(char *str, int item);
void		roll_emails(int item, enum rotate_dir dir);
void		add_item();
int		parse_date_string(char *s, int *day, int *month, int *year);

#define EDITW_COLS	(COLS - 6)
#define EDITW_LINES	(LINES - 5)
#define EDITW_TOP	2
#define EDITW_X		3

#define EDITOR_HELPLINE	N_("?:help q:quit editor")

#define TABLINE		1

#define TAB_COLON_POS	28
#define FIELDNAME_MAX_WIDTH	20
#define FIELD_MAX_WIDTH	(EDITW_COLS - TAB_COLON_POS - FIELDS_START_X - 2)

#define FIELDS_START_Y	4
#define FIELDS_START_X	4

#endif
