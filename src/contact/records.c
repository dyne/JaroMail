/* Bounded noninteractive editor for the same store and revisions as abook. */
#include "migrate.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static struct jaro_contact_text text(const char *s)
{
	return (struct jaro_contact_text){s,strlen(s)};
}

static int number(const char *s, int64_t *out)
{
	char *end;
	if (!*s || strspn(s,"0123456789") != strlen(s)) return -1;
	errno = 0;
	*out = strtoimax(s,&end,10);
	return errno || *end ? -1 : 0;
}

static int field(struct jaro_contact_text value)
{
	return (value.size && fwrite(value.data,1,value.size,stdout) != value.size) || putchar(0) == EOF;
}

static int book_row(struct jaro_contact_text name, void *unused)
{
	(void)unused;
	return field(name);
}

static int record(const struct jaro_contact *row, void *unused)
{
	(void)unused;
	if (printf("%" PRId64 "%c%" PRId64 "%c",row->id,0,row->revision,0) < 0 ||
		field(row->name) || field(row->nickname) || field(row->notes) || field(row->insertion_date) ||
		printf("%zu%c",row->address_count,0) < 0) return 1;
	for (size_t i=0; i<row->address_count; i++) if (field(row->addresses[i])) return 1;
	if (printf("%zu%c",row->field_count,0) < 0) return 1;
	for (size_t i=0; i<row->field_count; i++)
		if (field(row->fields[i].key) || field(row->fields[i].value)) return 1;
	return 0;
}

struct draft {
	char *bytes;
	struct jaro_contact_text fields[3 + JARO_CONTACT_ADDRESSES_MAX];
	size_t count;
};

static int read_draft(struct draft *draft)
{
	size_t limit = JARO_CONTACT_RECORD_MAX + 3 + JARO_CONTACT_ADDRESSES_MAX;
	draft->bytes = malloc(limit + 1);
	if (!draft->bytes) return JARO_CONTACT_STORAGE;
	size_t size = fread(draft->bytes,1,limit+1,stdin);
	if (ferror(stdin)) return JARO_CONTACT_STORAGE;
	if (size > limit || !size || draft->bytes[size-1]) return JARO_CONTACT_INVALID;
	for (size_t pos=0; pos<size;) {
		struct jaro_contact_text value = text(draft->bytes+pos);
		if (draft->count >= 3 + JARO_CONTACT_ADDRESSES_MAX || value.size > JARO_CONTACT_TEXT_MAX)
			return JARO_CONTACT_INVALID;
		draft->fields[draft->count++] = value;
		pos += value.size+1;
	}
	return draft->count >= 3 ? 0 : JARO_CONTACT_INVALID;
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	const char *op = argv[2];
	int books = !strcmp(op,"books"), list = !strcmp(op,"list");
	int create = !strcmp(op,"create-book"), rename = !strcmp(op,"rename-book");
	int remove = !strcmp(op,"delete-book"), put = !strcmp(op,"put"), del = !strcmp(op,"delete");
	int required = books ? 3 : list ? 4 : create || remove ? 5 : rename || put || del ? 6 : 0;
	if (argc != required) return 2;
	int write = !books && !list;
	int64_t generation=0, expected=0, id=0, book=0;
	if (write && number(argv[argc-1],&expected)) return 2;
	if ((put || del) && number(argv[4],&id)) return 2;
	if (del && !id) return 2;
	struct draft draft = {0};
	struct jaro_contact old = {0};
	struct jaro_contact_text name = books ? text("") : text(argv[3]);
	struct jaro_contact_store *store = NULL;
	struct jaro_contact_error error = {0};
	struct jaro_contact_migration_error migration = {0};
	int rc = put ? read_draft(&draft) : 0;
	if (!rc) rc = jaro_contact_profile_open(argv[1], write ? JARO_CONTACT_PROFILE_WRITE :
			JARO_CONTACT_PROFILE_PREVIEW, &store, &migration);
	if (!rc) rc = write ? jaro_contact_begin(store,&error) : jaro_contact_begin_read(store,&error);
	if (!rc) rc = jaro_contact_generation(store,&generation,&error);
	if (!rc && write && (!id || create || rename || remove) && generation != expected)
		rc = JARO_CONTACT_CONFLICT;
	if (!rc && !books) {
		rc = jaro_contact_book(store,name,0,&book,&error);
		if (create && rc == JARO_CONTACT_MISSING) rc = jaro_contact_book(store,name,1,&book,&error);
		else if (create && !rc) rc = JARO_CONTACT_CONFLICT;
	}
	if (!rc && !write && printf("JCT1%c%" PRId64 "%c",0,generation,0)<0) rc=JARO_CONTACT_CONSUMER;
	if (!rc && books) rc = jaro_contact_visit_books(store,book_row,NULL,&error);
	if (!rc && list) rc = jaro_contact_visit(store,book,text(""),record,NULL,&error);
	if (!rc && (rename || remove))
		rc = jaro_contact_change_book(store,name,text(rename ? argv[4] : ""),&error);
	if (!rc && del) rc = jaro_contact_delete(store,book,id,expected,&error);
	if (!rc && put) {
		if (id) rc = jaro_contact_get(store,book,id,&old,&error);
		struct jaro_contact edited = old;
		edited.book_id = book; edited.id = id; edited.revision = id ? expected : 0;
		edited.name=draft.fields[0]; edited.nickname=draft.fields[1]; edited.notes=draft.fields[2];
		edited.addresses=draft.fields+3; edited.address_count=draft.count-3;
		if (!rc) rc = jaro_contact_save(store,&edited,&id,&error);
	}
	if (!rc && write) rc = jaro_contact_generation(store,&generation,&error);
	if (store) {
		rc = jaro_contact_finish(store,rc,&error);
		jaro_contact_close(store,NULL);
	}
	jaro_contact_clear(&old);
	free(draft.bytes);
	if (rc) {
		fprintf(stderr,"contacts: %s\n",rc==JARO_CONTACT_CONFLICT ? "stale revision or duplicate" :
			rc==JARO_CONTACT_BUSY ? "busy" : rc==JARO_CONTACT_MISSING ? "missing" :
			rc==JARO_CONTACT_INVALID ? "invalid record" : "storage or output error");
		return rc==JARO_CONTACT_CONFLICT || rc==JARO_CONTACT_BUSY ? 6 :
			rc==JARO_CONTACT_MISSING ? 1 : rc==JARO_CONTACT_INVALID ? 4 : 3;
	}
	if (write && printf("JCT1%c%" PRId64 "%c%" PRId64 "%c",0,generation,0,id,0)<0) return 3;
	return fflush(stdout) || ferror(stdout) ? 3 : 0;
}
