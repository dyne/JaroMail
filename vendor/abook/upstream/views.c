/*
 * $Id$
 *
 * by Cedric Duval <cedricduval@free.fr>
 *
 * Copyright (C) Cedric Duval
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include "gettext.h"
#include "misc.h"
#include "options.h"
#include "views.h"
#include "xmalloc.h"


abook_view *abook_views = NULL;
int views_count = 0;


extern abook_field standard_fields[];


static abook_view *
find_view(char *name)
{
	abook_view *cur = abook_views;

	for(; cur; cur = cur->next)
		if(0 == strcasecmp(cur->name, name))
			return cur;

	return NULL;
}

static abook_view *
create_view(char *name) {
	abook_view *v;

	for(v = abook_views; v && v->next; v = v->next)
		;

	if(v) {
		v->next = xmalloc(sizeof(abook_view));
		v = v->next;		
	} else
		abook_views = v = xmalloc(sizeof(abook_view));

	v->name = xstrdup(name);
	v->fields = NULL;
	v->next = NULL;

	views_count++;

	return v;
}

static int
fields_in_view(abook_view *view)
{
	int nb;
	abook_field_list *f;

	for(nb = 0, f = view->fields; f; f = f->next, nb++)
		;

	return nb;
}

char *
add_field_to_view(char *viewname, char *field)
{
	abook_view *v;
	abook_field *f;

	if(
			!(f = find_declared_field(field)) &&
			!(f = find_standard_field(field, 1 /*do_declare*/))
		)
		return _("undeclared field");
	
	if((v = find_view(viewname)) == NULL)
		v = create_view(viewname);
	else if(fields_in_view(v) == MAX_VIEW_FIELDS)
		return _("maximal number of fields per view reached");

	if(v->fields && (find_field(field, v->fields)))
		return _("field already in this view");

	add_field(&v->fields, f);
	
	return NULL;
}

void
view_info(int number, char **name, abook_field_list **fields)
{	int i = 0;
	abook_view *cur = abook_views;

	assert((number < views_count) && (number >= 0));
	
	while(i++ != number)
		cur = cur->next;

	if(fields)
		*fields = cur->fields;

	if(name)
		*name = cur->name;
}

#define MAX_DEFAULT_FIELDS_PER_VIEW 6

void
init_default_views()
{
	char *str;
	int i, j, add_custom_fields, add_custom_view = 0;
	
	add_custom_fields =
		!strcasecmp(opt_get_str(STR_PRESERVE_FIELDS), "standard");

	/* if the user has configured views, no need to provide defaults */
	if(abook_views)
		goto out;
	add_custom_view = 1;

	struct {
		char *name;
		int fields[MAX_DEFAULT_FIELDS_PER_VIEW + 1];
	} default_views[] = {
		{ N_("CONTACT"), {NAME, EMAIL, -1} },
		{ N_("ADDRESS"),
			{ ADDRESS, ADDRESS2, CITY, STATE, ZIP, COUNTRY, -1 } },
		{ N_("PHONE"), { PHONE, WORKPHONE, FAX, MOBILEPHONE, -1 } },
		{ N_("OTHER"), { NICK, URL, NOTES, ANNIVERSARY, GROUPS, -1 } },
		{ 0 }
	};

	for(i = 0; default_views[i].name; i++) {
		for(j = 0; j < MAX_DEFAULT_FIELDS_PER_VIEW; j++) {
			if(default_views[i].fields[j] == -1)
				break;
			str = standard_fields[default_views[i].fields[j]].key;
			add_field_to_view(gettext(default_views[i].name), str);
		}
	}
out:

#define init_view(view, key, name) do { \
	if(add_custom_fields || add_custom_view) \
		declare_new_field(key, name, "string", \
				0 /*"standard" field already declared above*/);\
	if(add_custom_view) \
		add_field_to_view(view, key); \
} while(0);

	init_view(_("CUSTOM"), "custom1", _("Custom1"));
	init_view(_("CUSTOM"), "custom2", _("Custom2"));
	init_view(_("CUSTOM"), "custom3", _("Custom3"));
	init_view(_("CUSTOM"), "custom4", _("Custom4"));
	init_view(_("CUSTOM"), "custom5", _("Custom5"));
}
