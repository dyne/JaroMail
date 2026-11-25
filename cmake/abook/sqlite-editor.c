#define _POSIX_C_SOURCE 200809L

#include "abook.h"
#include "database.h"
#include "list.h"
#include "ui.h"
#include "xmalloc.h"
#include "sqlite-editor.h"
#include "search-fold.h"
#include "contact/legacy-import.h"
#include "contact/migrate.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

const char *jaro_editor_profile, *jaro_editor_list;
extern int fields_count;
extern bool db_need_save;

struct entry {
	struct jaro_contact contact;
	int seen;
};

struct view {
	struct entry *entries;
	size_t count, capacity;
	struct jaro_contact_store *reader;
	struct jaro_contact_error error;
};

static struct view current;
static char diagnostic[256];
static char *chosen_list;
static int failed_item = -1;
static const char *failed_field;

const char *jaro_editor_diagnostic(void)
{
	return diagnostic;
}

static void set_diagnostic(const char *reason, const char *field)
{
	snprintf(diagnostic,sizeof diagnostic,"%s%s%s",field ? field : "",field ? ": " : "",reason);
}

int jaro_editor_failed_item(void)
{
	return failed_item;
}

const char *jaro_editor_failed_field(void)
{
	return failed_field;
}

static void save_diagnostic(int status, const char *reason, const char *field, int64_t id)
{
	failed_field = status == JARO_CONTACT_INVALID ? field : NULL;
	if (status == JARO_CONTACT_BUSY)
		snprintf(diagnostic, sizeof diagnostic, "Contact store is busy. Retry save.");
	else if (status == JARO_CONTACT_CONFLICT)
		snprintf(diagnostic, sizeof diagnostic, "Conflict #%" PRId64 ": changed elsewhere", id);
	else if (status == JARO_CONTACT_INVALID && field)
		snprintf(diagnostic, sizeof diagnostic, "Invalid %s: %s", field, reason);
	else
		set_diagnostic(reason ? reason : "cannot save contacts", field);
}

static struct jaro_contact_text span(const char *text)
{
	return (struct jaro_contact_text){text,text ? strlen(text) : 0};
}

static int equal(struct jaro_contact_text left,struct jaro_contact_text right)
{
	return left.size==right.size && (!left.size || !memcmp(left.data,right.data,left.size));
}

static int open_store(enum jaro_contact_profile_mode mode,struct jaro_contact_store **store)
{
	struct jaro_contact_migration_error error;
	int status=jaro_contact_profile_open(jaro_editor_profile,mode,store,&error);
	if (status) jaro_contact_migration_diagnostic(diagnostic, sizeof diagnostic, &error);
	return status;
}

static int keep_book(struct jaro_contact_text name, void *data)
{
	if (!equal(name, span("allow")) && !equal(name, span("block")) &&
	    !equal(name, span(jaro_editor_list)))
		abook_list_append(data, (char *)name.data);
	return 0;
}

int jaro_editor_books(abook_list **books)
{
	struct jaro_contact_store *store;
	struct jaro_contact_error error = {0};

	*books = NULL;
	if (open_store(JARO_CONTACT_PROFILE_READ, &store))
		return -1;
	abook_list_append(books, "allow");
	abook_list_append(books, "block");
	if (strcmp(jaro_editor_list, "allow") && strcmp(jaro_editor_list, "block"))
		abook_list_append(books, (char *)jaro_editor_list);
	int status = jaro_contact_visit_books(store, keep_book, books, &error);
	jaro_contact_close(store, NULL);
	if (status) {
		set_diagnostic(error.reason, error.field);
		abook_list_free(books);
		return -1;
	}
	return 0;
}

int jaro_editor_switch_book(const char *name)
{
	const char *previous = jaro_editor_list;
	char *next = xstrdup(name);

	jaro_editor_list = next;
	if (jaro_editor_load()) {
		jaro_editor_list = previous;
		free(next);
		return -1;
	}
	free(chosen_list);
	chosen_list = next;
	return 0;
}

/* These lowercase Latin letters survive canonical folding. Each ASCII form
 * fits in the two bytes of the original UTF-8 letter. */
static const char *editor_latin_alias(const char *letter)
{
	static const struct {
		char letter[3], ascii[3];
	} aliases[] = {
		{"æ", "ae"}, {"ð", "d"}, {"ø", "o"}, {"þ", "th"},
		{"đ", "d"}, {"ħ", "h"}, {"ı", "i"}, {"ł", "l"},
		{"œ", "oe"}, {"ŧ", "t"},
	};
	for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
		if (!strncmp(letter, aliases[i].letter, 2))
			return aliases[i].ascii;
	return NULL;
}

static char *editor_search_key(const char *text, size_t length)
{
	char *key = abook_search_key_alloc(text, length, xmalloc);
	if (!key)
		abort();
	const char *input = key;
	char *output = key;
	while (*input) {
		unsigned char lead = (unsigned char)*input;
		const char *alias = lead >= 0xc3 && lead <= 0xc5 ? editor_latin_alias(input) : NULL;
		if (alias) {
			size_t size = strlen(alias);
			memcpy(output, alias, size);
			output += size;
			input += 2;
		} else {
			*output++ = *input++;
		}
	}
	*output = '\0';
	return key;
}

struct jaro_editor_query {
	size_t count;
	bool anchor_start, anchor_end, has_wildcard;
	char *terms[];
};

struct jaro_editor_query *jaro_editor_query_new(const char *pattern)
{
	if (!*pattern)
		return NULL;

	const char *end = pattern + strlen(pattern);
	bool anchor_start = *pattern == '^', anchor_end = end[-1] == '$';
	size_t capacity = 1;
	for (const char *p = pattern; *p; p++)
		capacity += *p == '*';
	struct jaro_editor_query *query = xmalloc(sizeof *query + capacity * sizeof *query->terms);
	*query = (struct jaro_editor_query){0};

	const char *term = pattern + anchor_start;
	end -= anchor_end;
	bool after_wildcard = FALSE;
	for (;;) {
		const char *wildcard = memchr(term, '*', end - term);
		const char *stop = wildcard ? wildcard : end;
		char *folded = editor_search_key(term, stop - term);
		if (*folded) {
			if (!query->count)
				query->anchor_start = anchor_start && !after_wildcard;
			query->terms[query->count++] = folded;
			after_wildcard = FALSE;
		} else {
			free(folded);
		}
		if (!wildcard)
			break;
		query->has_wildcard = after_wildcard = TRUE;
		term = wildcard + 1;
	}
	query->anchor_end = anchor_end && !after_wildcard;
	return query;
}

void jaro_editor_query_free(struct jaro_editor_query *query)
{
	if (query)
		for (size_t i = 0; i < query->count; i++)
			free(query->terms[i]);
	free(query);
}

static int folded_matches(const char *value, const struct jaro_editor_query *query)
{
	size_t count = query->count;
	if (!count)
		return query->has_wildcard;

	const char *end = value + strlen(value);
	if (query->anchor_end) {
		const char *suffix = query->terms[--count];
		size_t length = strlen(suffix);
		if (length > (size_t)(end - value) || strcmp(end - length, suffix))
			return 0;
		end -= length;
		if (!count)
			return !query->anchor_start || value == end;
	}

	/* Earliest matches leave the most room for the remaining terms. */
	for (size_t i = 0; i < count; i++) {
		const char *term = query->terms[i];
		size_t length = strlen(term);
		if (i || !query->anchor_start)
			value = strstr(value, term);
		else if (strncmp(value, term, length))
			return 0;
		if (!value || value > end || length > (size_t)(end - value))
			return 0;
		value += length;
	}
	return 1;
}

static int field_matches(const char *value, const struct jaro_editor_query *query)
{
	if (!value || !*value)
		return 0;
	char *folded = editor_search_key(value, strlen(value));
	int matches = folded_matches(folded, query);
	free(folded);
	return matches;
}

int jaro_editor_matches(int item, const struct jaro_editor_query *query)
{
	/* Match the current editor rows, including drafts not yet in SQLite. */
	if (!query->count && !query->has_wildcard)
		return 0;
	if (field_matches(db_fget(item, NAME), query) ||
	    field_matches(db_fget(item, NICK), query) ||
	    field_matches(db_fget(item, NOTES), query))
		return 1;
	abook_list *mail = mailbox_list_from_csv(safe_str(db_fget(item, EMAIL)));
	int matches = 0;
	for (abook_list *address = mail; address && !matches; address = address->next)
		matches = field_matches(address->data, query);
	abook_list_free(&mail);
	return matches;
}

static void free_view(struct view *view)
{
	for (size_t i=0;i<view->count;i++) jaro_contact_clear(&view->entries[i].contact);
	free(view->entries);
	*view=(struct view){0};
}

static int keep_record(const struct jaro_contact *record,void *data)
{
	struct view *view=data;
	if (view->count==view->capacity) {
		size_t next=view->capacity ? view->capacity*2 : 32;
		struct entry *entries=NULL;
		if (next>view->capacity && next<=SIZE_MAX/sizeof *entries)
			entries=realloc(view->entries,next*sizeof *entries);
		if (!entries) {
			view->error=(struct jaro_contact_error){"cannot allocate editor records",NULL,0,ENOMEM};
			return 1;
		}
		view->entries=entries;
		view->capacity=next;
	}
	struct entry *entry=&view->entries[view->count];
	*entry=(struct entry){0};
	if (jaro_contact_get(view->reader,record->book_id,record->id,&entry->contact,&view->error)) return 1;
	view->count++;
	return 0;
}

static char *mailboxes(const struct jaro_contact *record)
{
	size_t size=record->address_count ? record->address_count-1 : 0;
	for (size_t i=0;i<record->address_count;i++) size+=record->addresses[i].size;
	char *joined=xmalloc(size+1), *pos=joined;
	for (size_t i=0;i<record->address_count;i++) {
		if (i) *pos++=',';
		memcpy(pos,record->addresses[i].data,record->addresses[i].size);
		pos+=record->addresses[i].size;
	}
	*pos=0;
	return joined;
}

static void show_record(struct jaro_contact *record)
{
	list_item item=item_create();
	item_fput(item,NAME,xstrdup(record->name.data));
	item_fput(item,NICK,xstrdup(record->nickname.data));
	item_fput(item,NOTES,xstrdup(record->notes.data));
	item_fput(item,EMAIL,mailboxes(record));
	for (size_t i=0;i<record->field_count;i++) {
		int field;
		find_field_number((char *)record->fields[i].key.data,&field);
		if (field>=0 && !item[field]) item[field]=xstrdup(record->fields[i].value.data);
	}
	item_set_contact_id(item,record->id);
	add_item2database(item);
	item_free(&item);
}

static int compare_ids(const void *left, const void *right)
{
	int64_t a=((const struct entry *)left)->contact.id;
	int64_t b=((const struct entry *)right)->contact.id;
	return (a>b)-(a<b);
}

static void install_view(struct view *view, int64_t selected_id);

int jaro_editor_load(void)
{
	struct view view={0};
	struct jaro_contact_error error={0};
	int64_t id;
	int status=open_store(JARO_CONTACT_PROFILE_READ,&view.reader);
	if (status) return -1;
	status=jaro_contact_begin_read(view.reader,&error);
	if (!status) {
		status=jaro_contact_book(view.reader,span(jaro_editor_list),0,&id,&error);
		if (status==JARO_CONTACT_MISSING) status=0;
		else if (!status) status=jaro_contact_visit(view.reader,id,span(NULL),keep_record,&view,&error);
		status=jaro_contact_finish(view.reader,status,&error);
	}
	jaro_contact_close(view.reader,NULL);
	view.reader=NULL;
	if (status) {
		if (view.error.reason) error=view.error;
		set_diagnostic(error.reason,error.field);
		free_view(&view);
		return -1;
	}
	install_view(&view, list_current_contact_id());
	return 0;
}

static void install_view(struct view *view, int64_t selected_id)
{
	close_database();
	free_view(&current);
	init_standard_fields();
	for (size_t i=0;i<view->count;i++)
		for (size_t j=0;j<view->entries[i].contact.field_count;j++) {
			char *key=(char *)view->entries[i].contact.fields[j].key.data;
			if (!find_declared_field(key)) declare_new_field(key,key,"string",1);
		}
	for (size_t i=0;i<view->count;i++) show_record(&view->entries[i].contact);
	if (view->count) qsort(view->entries,view->count,sizeof *view->entries,compare_ids);
	current=*view;
	*view=(struct view){0};
	db_need_save=FALSE;
	list_restore_contact_id(selected_id);
}

static int add_field_value(struct jaro_contact *draft,struct jaro_contact_field *fields,
	struct jaro_contact_text key,struct jaro_contact_text value)
{
	if (draft->field_count==JARO_CONTACT_FIELDS_MAX) return 1;
	fields[draft->field_count++]=(struct jaro_contact_field){key,value};
	return 0;
}

static int fill_fields(list_item item,const struct jaro_contact *original,
	struct jaro_contact *draft,struct jaro_contact_field *fields)
{
	/* Preserve existing field order. New fields follow the form order. */
	for (size_t i=0;original && i<original->field_count;i++) {
		const struct jaro_contact_field *old=&original->fields[i];
		int index;
		find_field_number((char *)old->key.data,&index);
		if (index==field_id(NAME) || index==field_id(EMAIL) ||
			index==field_id(NICK) || index==field_id(NOTES)) {
			if (add_field_value(draft,fields,old->key,old->value)) return 1;
		} else if (index>=0 && item[index] &&
			add_field_value(draft,fields,old->key,span(item[index]))) return 1;
	}
	for (int i=0;i<fields_count;i++) {
		if (i==field_id(NAME) || i==field_id(EMAIL) || i==field_id(NICK) || i==field_id(NOTES) || !item[i]) continue;
		char *key;
		get_field_info(i,&key,NULL,NULL);
		size_t j=0;
		while (j<draft->field_count && !equal(fields[j].key,span(key))) j++;
		if (j==draft->field_count && add_field_value(draft,fields,span(key),span(item[i]))) return 1;
	}
	return 0;
}

static int same_fields(const struct jaro_contact *left,const struct jaro_contact *right)
{
	if (!equal(left->name,right->name) || !equal(left->nickname,right->nickname) ||
		!equal(left->notes,right->notes) || left->field_count!=right->field_count) return 0;
	for (size_t i=0;i<left->field_count;i++)
		if (!equal(left->fields[i].key,right->fields[i].key) ||
			!equal(left->fields[i].value,right->fields[i].value)) return 0;
	return 1;
}

struct pending_edit {
	struct jaro_contact contact;
	struct jaro_contact_text *split;
	struct jaro_contact_field *fields;
	int prepared;
	int item;
};

static int prepare_item(list_item item,struct pending_edit *pending,struct jaro_contact_error *error)
{
	struct entry identity={.contact.id=item_contact_id(item)};
	struct entry *previous=current.count && identity.contact.id ?
		bsearch(&identity,current.entries,current.count,sizeof *current.entries,compare_ids) : NULL;
	const struct jaro_contact *original=previous ? &previous->contact : NULL;
	struct jaro_contact_field fields[JARO_CONTACT_FIELDS_MAX];
	struct jaro_contact draft={
		.id=identity.contact.id,.revision=original ? original->revision : 0,
		.name=span(item_fget(item,NAME)),.nickname=span(item_fget(item,NICK)),
		.notes=span(item_fget(item,NOTES)),.fields=fields
	};
	if (previous) previous->seen=1;
	if (fill_fields(item,original,&draft,fields)) {
		*error=(struct jaro_contact_error){"too many contact fields","custom",0,0};
		return JARO_CONTACT_INVALID;
	}
	struct jaro_contact_text email=span(item_fget(item,EMAIL));
	char *before=original ? mailboxes(original) : NULL;
	int unchanged=original && equal(email,span(before));
	free(before);
	if (unchanged && same_fields(&draft,original)) return 0;
	int status=0;
	if (unchanged) {
		draft.addresses=original->addresses;
		draft.address_count=original->address_count;
	} else {
		status=jaro_contact_split_mailboxes(email,&pending->split,&draft.address_count);
		draft.addresses=pending->split;
		if (status) {
			*error=(struct jaro_contact_error){"invalid mailbox list","email",0,0};
			return status;
		}
	}
	if (draft.field_count) {
		pending->fields=malloc(draft.field_count*sizeof *fields);
		if (!pending->fields) {
			*error=(struct jaro_contact_error){"cannot keep contact fields","custom",0,ENOMEM};
			return JARO_CONTACT_STORAGE;
		}
		memcpy(pending->fields,fields,draft.field_count*sizeof *fields);
	}
	draft.fields=pending->fields;
	pending->contact=draft;
	pending->prepared=1;
	return 0;
}

static void free_edits(struct pending_edit *edits,size_t count)
{
	for (size_t i=0;i<count;i++) {
		free(edits[i].split);
		free(edits[i].fields);
	}
	free(edits);
}

int jaro_editor_save(void)
{
	struct jaro_contact_store *store=NULL;
	struct jaro_contact_error error={0};
	struct view committed={0};
	int64_t selected_id=list_current_contact_id(), affected_id=0;
	failed_item=-1;
	failed_field=NULL;
	if (!db_need_save) return 0;

	int items=db_n_items(),status=0;
	size_t edits_count=0,deletes=0;
	struct pending_edit *edits=items ? calloc((size_t)items,sizeof *edits) : NULL;
	if (items && !edits) {
		set_diagnostic("cannot prepare contact edits",NULL);
		return -1;
	}
	for (size_t i=0;i<current.count;i++) current.entries[i].seen=0;
	for (int i=0;!status && i<items;i++) {
		struct pending_edit *edit=&edits[edits_count];
		list_item item=db_item_get(i);
		edit->item=i;
		status=prepare_item(item,edit,&error);
		if (status) {
			failed_item=i;
			affected_id=item_contact_id(item);
		}
		if (status || edit->prepared) edits_count++;
	}
	if (status) goto done;
	for (size_t i=0;i<current.count;i++) if (!current.entries[i].seen) deletes++;
	if (!edits_count && !deletes) {
		db_need_save=FALSE;
		free(edits);
		return 0;
	}
	status=open_store(JARO_CONTACT_PROFILE_WRITE,&store);
	if (status) {
		if (status==JARO_CONTACT_BUSY)
			save_diagnostic(status,NULL,NULL,0);
		free_edits(edits,edits_count);
		return -1;
	}
	status=jaro_contact_begin(store,&error);
	if (!status) {
		int64_t book,id;
		status=jaro_contact_book(store,span(jaro_editor_list),1,&book,&error);
		for (size_t i=0;!status && i<edits_count;i++) {
			edits[i].contact.book_id=book;
			status=jaro_contact_save(store,&edits[i].contact,&id,&error);
			if (status) {
				failed_item=edits[i].item;
				affected_id=edits[i].contact.id;
			} else if (edits[i].item==list_get_curitem())
				selected_id=id;
		}
		for (size_t i=0;!status && i<current.count;i++) {
			struct entry *entry=&current.entries[i];
			if (!entry->seen) {
				status=jaro_contact_delete(store,book,entry->contact.id,
					entry->contact.revision,&error);
				if (status) affected_id=entry->contact.id;
			}
		}
		/* Prepare the new editor snapshot before commit. A failed read or
		 * commit leaves the original revisions and every draft in place. */
		committed.reader=store;
		if (!status)
			status=jaro_contact_visit(store,book,span(NULL),keep_record,&committed,&error);
		if (committed.error.reason) error=committed.error;
		status=jaro_contact_finish(store,status,&error);
		committed.reader=NULL;
	}
	jaro_contact_close(store,NULL);
done:
	free_edits(edits,edits_count);
	if (status) {
		free_view(&committed);
		save_diagnostic(status,error.reason,error.field,affected_id);
		return -1;
	}
	install_view(&committed,selected_id);
	return 1;
}
