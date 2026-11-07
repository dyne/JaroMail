
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
#include <assert.h>
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include "abook.h"
#include "database.h"
#include "gettext.h"
#include "list.h"
#include "misc.h"
#include "xmalloc.h"

abook_field_list *fields_list = NULL;
int fields_count = 0;

list_item *database = NULL;
static int items = 0;

#define ITEM_SIZE (fields_count * sizeof(char *))
#define LAST_ITEM (items - 1)

#define INITIAL_LIST_CAPACITY	30
static int list_capacity = 0;

int standard_fields_indexed[ITEM_FIELDS];

/*
 * notes about adding predefined "standard" fields:
 * 	- leave alone "name" and "email"
 * 	- reorganize the field numbers in database.h
 */
abook_field standard_fields[] = {
	{"name",	N_("Name"),		FIELD_STRING}, /* NAME */
	{"email",	N_("E-mail addresses"),	FIELD_EMAILS}, /* EMAIL */
	{"address",	N_("Address"),		FIELD_STRING}, /* ADDRESS */
	{"address2",	N_("Address2"),		FIELD_STRING}, /* ADDRESS2 */
	{"city",	N_("City"),		FIELD_STRING}, /* CITY */
	{"state",	N_("State/Province"),	FIELD_STRING}, /* STATE */
	{"zip",		N_("ZIP/Postal Code"),	FIELD_STRING}, /* ZIP */
	{"country",	N_("Country"),		FIELD_STRING}, /* COUNTRY */
	{"phone",	N_("Home Phone"),	FIELD_STRING}, /* PHONE */
	{"workphone",	N_("Work Phone"),	FIELD_STRING}, /* WORKPHONE */
	{"fax",		N_("Fax"),		FIELD_STRING}, /* FAX */
	{"mobile",	N_("Mobile"),		FIELD_STRING}, /* MOBILEPHONE */
	{"nick",	N_("Nickname/Alias"),	FIELD_STRING}, /* NICK */
	{"url",		N_("URL"),		FIELD_STRING}, /* URL */
	{"notes",	N_("Notes"),		FIELD_STRING}, /* NOTES */
	{"anniversary",	N_("Anniversary day"),	FIELD_DATE},   /* ANNIVERSARY */
	{"groups",	N_("Groups"),		FIELD_LIST},   /* GROUPS */
	{0} /* ITEM_FIELDS */
};

bool db_need_save = FALSE;

extern int first_list_item;
extern int curitem;
extern char *selected;
extern char *datafile;



static abook_field *
declare_standard_field(int i)
{
	abook_field *f = xmalloc(sizeof(abook_field));

	f = memcpy(f, &standard_fields[i], sizeof(abook_field));
	f->name = xstrdup(gettext(f->name));

	add_field(&fields_list, f);

	assert(standard_fields_indexed[i] == -1);
	standard_fields_indexed[i] = fields_count++;

	return f;
}

abook_field *
find_standard_field(char *key, int do_declare)
{
	int i;

	for(i = 0; standard_fields[i].key; i++)
		if(0 == strcmp(standard_fields[i].key, key))
			goto found;

	return NULL;

found:
	return do_declare ? declare_standard_field(i) : &standard_fields[i];
}

/* Search for a field. Use the list of declared fields if no list specified. */
abook_field *
real_find_field(char *key, abook_field_list *list, int *number)
{
	abook_field_list *cur;
	int i;

	for(cur = (list ? list : fields_list), i = 0; cur; cur = cur->next, i++)
		if(0 == strcmp(cur->field->key, key)) {
			if(number)
				*number = i;
			return cur->field;
		}

	if(number)
		*number = -1;

	return NULL;
}

void
get_field_info(int i, char **key, char **name, int *type)
{
	abook_field_list *cur = fields_list;
	int j;

	assert(i < fields_count);

	for(j = 0; i >= 0 && j < i; j++, cur = cur->next)
		;

	if(key)
		*key = (i < 0) ? NULL : cur->field->key;
	if(name)
		*name = (i < 0) ? NULL : cur->field->name;
	if(type)
		*type = (i < 0) ? -1 : cur->field->type;
}

void
add_field(abook_field_list **list, abook_field *f)
{
	abook_field_list *tmp;

	for(tmp = *list; tmp && tmp->next; tmp = tmp->next)
		;

	if(tmp) {
		tmp->next = xmalloc(sizeof(abook_field_list));
		tmp = tmp->next;
	} else
		*list = tmp = xmalloc(sizeof(abook_field_list));

	tmp->field = f;
	tmp->next = NULL;
}

char *
declare_new_field(char *key, char *name, char *type, int accept_standard)
{
	abook_field *f;

	if(find_declared_field(key))
		return _("field already defined");

	if(find_standard_field(key, accept_standard))
		return accept_standard ? NULL /* ok, added */ :
			_("standard field does not need to be declared");

	f = xmalloc(sizeof(abook_field));
	f->key = xstrdup(key);
	f->name = xstrdup(name);

	if(!*type || (0 == strcasecmp("string", type)))
		f->type = FIELD_STRING;
	else if(0 == strcasecmp("emails", type))
		f->type = FIELD_EMAILS;
	else if(0 == strcasecmp("list", type))
		f->type = FIELD_LIST;
	else if(0 == strcasecmp("date", type))
		f->type = FIELD_DATE;
	else
		return _("unknown type");

	add_field(&fields_list, f);
	fields_count++;

	return NULL;
}

/*
 * Provide a list of custom fields. Handy for exporters.
 */
abook_field_list *
list_custom_fields()
{
	abook_field_list
		*cur = fields_list,
		*pos = NULL,
		*tmp = NULL;
	int is_standard, i;
	for(; cur; cur = cur->next) {
		is_standard = 0;
		for(i = 0; standard_fields[i].key; i++) {
			if(0 == strcmp(standard_fields[i].key, cur->field->key)) {
				is_standard = 1;
				break;
			}
		}

		if (is_standard) continue;

		if(tmp) {
			tmp->next = xmalloc(sizeof(abook_field_list));
			tmp = tmp->next;
		} else {
			pos = tmp = xmalloc(sizeof(abook_field_list));
		}

		tmp->field = cur->field;
		tmp->next = NULL;
	}

	return pos;
}

/*
 * Declare a new field while database is already loaded
 * making it grow accordingly
 */
static void
declare_unknown_field(char *key)
{
	int i;

	declare_new_field(key, key, "string",
			1 /* accept to declare "standard" fields */);

	if(!database)
		return;

	for(i = 0; i < items; i++)
		if(database[i]) {
			database[i] = xrealloc(database[i], ITEM_SIZE);
			database[i][fields_count - 1] = NULL;
		}
}

/*
 * Declare "standard" fields, thus preserving them while parsing a database,
 * even if they won't be displayed.
 */
void
init_standard_fields()
{
	int i;

	for(i = 0; standard_fields[i].key; i++)
		if(standard_fields_indexed[i] == -1)
			declare_standard_field(i);
}

/* Some initializations - Must be called _before_ load_opts() */
void
prepare_database_internals()
{
	int i;

	for(i = 0; i < ITEM_FIELDS; i++)
		standard_fields_indexed[i] = -1;

	/* the only two mandatory fields */
	declare_standard_field(NAME);
	declare_standard_field(EMAIL);
}

int
parse_database(FILE *in)
{
        char *line = NULL;
	char *tmp;
	int sec=0, field;
	list_item item;

	item = item_create();

	for(;;) {
		line = getaline(in);
		if(feof(in)) {
			if(item[field_id(NAME)] && sec) {
				add_item2database(item);
			} else {
				item_empty(item);
			}
			break;
		}

		if(!*line || *line == '\n' || *line == '#') {
			goto next;
		} else if(*line == '[') {
			if(item[field_id(NAME)] && sec ) {
				add_item2database(item);
			} else {
				item_empty(item);
			}
			sec = 1;
			memset(item, 0, ITEM_SIZE);
			if(!(tmp = strchr(line, ']')))
				sec = 0; /*incorrect section lines are skipped*/
		} else if((tmp = strchr(line, '=') ) && sec) {
			*tmp++ = '\0';
			find_field_number(line, &field);
			if(field != -1) {
				item[field] = xstrdup(tmp);
				goto next;
			} else if(!strcasecmp(opt_get_str(STR_PRESERVE_FIELDS),
						"all")){
				declare_unknown_field(line);
				item = xrealloc(item, ITEM_SIZE);
				item[fields_count - 1] = xstrdup(tmp);
				goto next;
			}
		}
next:
		xfree(line);
	}

	xfree(line);
	item_free(&item);
	db_need_save = FALSE;

	return 0;
}

int
load_database(char *filename)
{
	FILE *in;

	if(database != NULL)
		close_database();

	if ((in = abook_fopen(filename, "r")) == NULL)
		return -1;

	parse_database(in);

	return (items == 0) ? 2 : 0;
}

int
write_database(FILE *out, struct db_enumerator e)
{
	int j;
	int i = 0;
	abook_field_list *cur;

	fprintf(out,
		"# abook addressbook file\n\n"
		"[format]\n"
		"program=" PACKAGE "\n"
		"version=" VERSION "\n"
		"\n\n"
	);

	db_enumerate_items(e) {
		fprintf(out, "[%d]\n", i);

		for(cur = fields_list, j = 0; cur; cur = cur->next, j++) {
			if( database[e.item][j] != NULL &&
					*database[e.item][j] )
				fprintf(out, "%s=%s\n",
					cur->field->key,
					database[e.item][j]
					);
		}

		fputc('\n', out);
		i++;
	}

	return 0;
}

int
save_database(int force_save)
{

	// officialize this the day we are sure that db_need_save cover all possible
	// modifications of the database using the UI
#ifdef DEBUG
	if(! db_need_save && force_save != 1) {
		fprintf(stderr, "[debug] database unmodified, request to write on-disk ignored\n");
		return 0;
	}
#endif

	FILE *out;
	int ret = 0;
	struct db_enumerator e = init_db_enumerator(ENUM_ALL);
	char *datafile_new = strconcat(datafile, ".new", NULL);
	char *datafile_old = strconcat(datafile, "~", NULL);

	if( (out = abook_fopen(datafile_new, "w")) == NULL ) {
		ret = -1;
		goto out;
	}

	if(!list_is_empty())
		/*
		 * Possibly should check if write_database failed.
		 * Currently it returns always zero.
		 */
		write_database(out, e);

	fclose(out);

	if(access(datafile, F_OK) == 0 &&
			(rename(datafile, datafile_old)) == -1)
		ret = -1;
	
	if((rename(datafile_new, datafile)) == -1)
		ret = -1;

	if(ret == 0) {
		db_need_save = FALSE;
		refresh_screen();
	}

out:
	free(datafile_new);
	free(datafile_old);
	return ret;
}

static void
db_free_item(int item)
{
	item_empty(database[item]);
}

void
close_database()
{
	int i;

	for(i=0; i <= LAST_ITEM; i++)
		db_free_item(i);

	xfree(database);
	free(selected);

	database = NULL;
	selected = NULL;

	items = 0;
	first_list_item = curitem = -1;
	list_capacity = 0;
}


static void
validate_item(list_item item)
{
	abook_field_list *f;
	int i, max_field_len;
	char *tmp;

	for(f = fields_list, i = 0; f; f = f->next, i++) {
		max_field_len = 0;

		switch(f->field->type) {
			case FIELD_EMAILS:
				max_field_len = MAX_EMAILSTR_LEN;
				if(item[i] == NULL)
					item[i] = xstrdup("");
				break;
			case FIELD_LIST:
				/* TODO quote string if it contains commas */
				break;
			case FIELD_STRING:
				max_field_len = MAX_FIELD_LEN;
				break;
			case FIELD_DATE:
				break;
			default:
				assert(0);
		}

		if(max_field_len && item[i] &&
				((int)strlen(item[i]) > max_field_len)) {
			/* truncate field */
			tmp = item[i];
			item[i][max_field_len - 1] = 0;
			item[i] = xstrdup(item[i]);
			free(tmp);
		}
	}
}

static void
adjust_list_capacity()
{
	if(list_capacity < 1)
		list_capacity = INITIAL_LIST_CAPACITY;
	else if(items >= list_capacity)
		list_capacity *= 2;
	else if(list_capacity / 2 > items)
		list_capacity /= 2;
	else
		return;

	if(database)
		database = xrealloc(database,sizeof(list_item) * list_capacity);
	else /* allocate memory _and_ initialize pointers to NULL */
		database = xmalloc0(sizeof(list_item) * list_capacity);

	selected = xrealloc(selected, list_capacity);
}

int
add_item2database(list_item item)
{
	/* 'name' field is mandatory */
	if((item[field_id(NAME)] == NULL) || ! *item[field_id(NAME)]) {
		item_empty(item);
		return 1;
	}

	if(++items > list_capacity)
		adjust_list_capacity();

	validate_item(item);

	selected[LAST_ITEM] = 0;

	database[LAST_ITEM] = item_create();
        item_copy(database[LAST_ITEM], item);
	db_need_save = TRUE;

	return 0;
}
	

void
remove_selected_items()
{
	int i, j;

	if(list_is_empty())
		return;

	if(!selected_items())
		selected[curitem] = 1;

	for(j = LAST_ITEM; j >= 0; j--) {
		if(selected[j]) {
			db_free_item(j); /* added for .4 data_s_ */
			for(i = j; i < LAST_ITEM; i++) {
				item_copy(database[i], database[i + 1]);
				selected[i] = selected[i + 1];
			}
			item_free(&database[LAST_ITEM]);
			items--;
			db_need_save = TRUE;
		}
	}

	if(curitem > LAST_ITEM && items > 0)
		curitem = LAST_ITEM;

	adjust_list_capacity();

	select_none();
}

void merge_selected_items()
{
	int i, j;
	int destitem = -1;

	if((list_is_empty()) || (selected_items() < 2))
		return;

	/* Find the top item */
	for(j=0; destitem < 0; j++)
		if(selected[j])
			destitem = j;

	/* Merge pairwise */
	for(j = LAST_ITEM; j > destitem; j--) {
		if(selected[j]) {
			item_merge(database[destitem],database[j]);
			for(i = j; i < LAST_ITEM; i++) {
				/* TODO: this can be done by moving pointers */
				item_copy(database[i], database[i + 1]);
				selected[i] = selected[i + 1];
			}
			item_free(&database[LAST_ITEM]);
			items--;
			db_need_save = TRUE;
		}
	}

	if(curitem > LAST_ITEM && items > 0)
		curitem = LAST_ITEM;

	adjust_list_capacity();

	select_none();
}

void remove_duplicates()
{
	int i,j,k;
	char *tmpj;
	if(list_is_empty())
		return;

	/* Scan from the last one */
	for(j = LAST_ITEM - 1; j >= 0; j--) {
		tmpj = db_name_get(j);
		for(i = LAST_ITEM; i > j; i--)
			/* Check name and merge if dups */
			if (0 == strcmp(tmpj,db_name_get(i))) {
				item_merge(database[j],database[i]);
				if (curitem == i) curitem--;
				for(k = i; k < LAST_ITEM; k++) {
					item_copy(database[k], database[k + 1]);
				}
				item_free(&database[LAST_ITEM]);
				items--;
				db_need_save = TRUE;
			}
	}

	adjust_list_capacity();
}


char *
get_surname(char *s)
{
	char *p = s + strlen(s);

	assert(s != NULL);

	while(p > s && *(p - 1) != ' ')
		p--;

	return xstrdup(p);
}

static int
surnamecmp(const void *i1, const void *i2)
{
	int ret, idx = field_id(NAME);
	char *n1, *n2, *s1, *s2;

	n1 = (*(list_item *)i1)[idx];
	n2 = (*(list_item *)i2)[idx];
	
	s1 = get_surname(n1);
	s2 = get_surname(n2);

	if( !(ret = safe_strcoll(s1, s2)) )
		ret = safe_strcoll(n1, n2);

	free(s1);
	free(s2);

	return ret;
}

static int sort_field = -1;

static int
namecmp(const void *i1, const void *i2)
{
	char *n1, *n2;

	assert(sort_field >= 0 && sort_field < fields_count);

	n1 = (*(list_item *)i1)[sort_field];
	n2 = (*(list_item *)i2)[sort_field];

	return safe_strcoll(n1, n2);
}

void
sort_by_field(char *name)
{
	int field;

	select_none();

	name = (name == NULL) ? opt_get_str(STR_SORT_FIELD) : name;
	find_field_number(name, &field);

	if(field < 0) {
		if(name == opt_get_str(STR_SORT_FIELD))
			statusline_msg(_("Invalid field value defined "
				"in configuration"));
		else
			statusline_msg(_("Invalid field value for sorting"));

		return;
	}

	sort_field = field;

	qsort((void *)database, items, sizeof(list_item), namecmp);
	db_need_save = TRUE;

	refresh_screen();
}

void
sort_surname()
{
	select_none();

	qsort((void *)database, items, sizeof(list_item), surnamecmp);
	db_need_save = TRUE;

	refresh_screen();
}

/* TODO implement a search based on more sophisticated patterns */

void
compact_string (char* str)
{
	char* p = str;
	while (*str) {
		if (isalnum (*str)) *(p++) = *str;
		++str;
	}
	*p = 0;
}

int
find_item(char *str, int start, int search_fields[])
{
	int i, id;
	char *findstr = NULL;
	char *tmp = NULL;
	int ret = -1; /* not found */
	struct db_enumerator e = init_db_enumerator(ENUM_ALL);

	if(list_is_empty() || !is_valid_item(start))
		return -2; /* error */

	findstr = xstrdup(str);
	findstr = strlower(findstr);
	compact_string (findstr);

	e.item = start - 1; /* must be "real start" - 1 */
	db_enumerate_items(e) {
		for(i = 0; search_fields[i] >= 0; i++) {
			if((id = field_id(search_fields[i])) == -1)
				continue;
			if(database[e.item][id] == NULL)
				continue;
			tmp = xstrdup(database[e.item][id]);
			compact_string (tmp);
			if( tmp && strstr(strlower(tmp), findstr) ) {
				ret = e.item;
				goto out;
			}
			xfree(tmp);
		}
	}

out:
	free(findstr);
	free(tmp);
	return ret;
}

int
is_selected(int item)
{
	return selected[item];
}

int
is_valid_item(int item)
{
	return item <= LAST_ITEM && item >= 0;
}

int
last_item()
{
	return LAST_ITEM;
}

int
db_n_items()
{
	return items;
}

int
real_db_enumerate_items(struct db_enumerator e)
{
	int item = max(0, e.item + 1);
	int i;

	switch(e.mode) {
#ifdef DEBUG
		case ENUM_ALL:
			break;
#endif
		case ENUM_SELECTED:
			for(i = item; i <= LAST_ITEM; i++) {
				if(is_selected(i)) {
					item = i;
					goto out;
				}
			}
			return -1;
#ifdef DEBUG
		default:
			fprintf(stderr, "real_db_enumerate_items() "
					"BUG: unknown db_enumerator mode: %d\n",
					e.mode);
			break;
#endif
	}
out:
	return (item > LAST_ITEM || item < 0) ? -1 : item;
}

struct db_enumerator
init_db_enumerator(int mode)
{
	struct db_enumerator e;

	e.item = -1; /* important - means "start from beginning" */
	e.mode = mode;

	return e;
}


list_item
item_create()
{
	return xmalloc0(ITEM_SIZE);
}

void
item_free(list_item *item)
{
	assert(item);

	xfree(*item);
}

void
item_empty(list_item item)
{	int i;

	assert(item);

	for(i = 0; i < fields_count; i++)
		if(item[i])
			xfree(item[i]);

}

void
item_copy(list_item dest, list_item src)
{
	memmove(dest, src, ITEM_SIZE);
	// called by parse_database(), that's why
	// parse_database() re-init db_need_save to FALSE
	db_need_save = TRUE;
}

void
item_duplicate(list_item dest, list_item src)
{
	int i;

	for(i = 0; i < fields_count; i++)
		dest[i] = src[i] ? xstrdup(src[i]) : NULL;
}
 
/*
 * Merging works as follows:
 * - fields present only in source are copied over to dest
 * - multi-fields (email, groups) are checked for dupes ad merged
 * */
void
item_merge(list_item dest, list_item src)
{
	int i, found = 0;
	abook_list *dfield, *sfield, *ed, *es;

	for(i = 0; i < fields_count; i++)
		if (src[i]) {
			if (!dest[i])
			       dest[i] = xstrdup(src[i]);
			else if((i == field_id(EMAIL)) || (i == field_id(GROUPS))) {
				dfield = csv_to_abook_list(dest[i]);
				sfield = csv_to_abook_list(src[i]);
				for(es = sfield; es; es = es->next) {
					for(found=0, ed = dfield; (!found) && ed; ed = ed->next)
						found = (0 == strcmp(es->data,ed->data));
					if (!found)
						abook_list_append(&dfield, es->data);
				}
				xfree(dest[i]);
				dest[i] = abook_list_to_csv(dfield);
				abook_list_free(&dfield);
				abook_list_free(&sfield);
			}
		}

	item_empty(src);
}

/* 
 * Things like item[field_id(NICK)] should never be used, since besides NAME
 * and EMAIL, none of the standard fields can be assumed to be existing.
 *
 * Prefer the functions item_fput(), item_fget(), db_fput() and db_fget()
 * to access fields in items and database.
 */

/* quick lookup by "standard" field number */
inline int
field_id(int i)
{
	assert((i >= 0) && (i < ITEM_FIELDS));
	return standard_fields_indexed[i];
}

int
item_fput(list_item item, int i, char *val)
{
	int id = field_id(i);

	if(id != -1) {
		item[id] = val;
		db_need_save = TRUE;
		return 1;
	}

	return 0;
}

char *
item_fget(list_item item, int i)
{
	int id = field_id(i);

	if(id != -1)
		return item[id];
	else
		return NULL;
}

int
real_db_field_put(int item, int i, int std, char *val)
{
	int id;

	assert(database[item]);

	id = std ? field_id(i) : i;

	if(id != -1) {
		database[item][id] = val;
		db_need_save = TRUE;
		return 1;
	}

	return 0;
}

char *
real_db_field_get(int item, int i, int std)
{
	int id;

	assert(database[item]);

	id = std ? field_id(i) : i;

	if(id != -1)
		return database[item][id];
	else
		return NULL;
}

list_item
db_item_get(int i)
{
	return database[i];
}

/* Fetch addresses from all fields of FIELD_EMAILS type */
/* Memory has to be freed by the caller */
char *
db_email_get(int item)
{
	int i;
	char *res;
	abook_field_list *cur;
	abook_list *emails = NULL;

	for(cur = fields_list, i = 0; cur; cur = cur->next, i++)
		if(cur->field->type == FIELD_EMAILS && *database[item][i])
			abook_list_append(&emails, database[item][i]);

	res = abook_list_to_csv(emails);
	abook_list_free(&emails);
	return res ? res : xstrdup("");
}

