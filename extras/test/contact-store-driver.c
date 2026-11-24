#define _POSIX_C_SOURCE 200809L

#include "contact/legacy-import.h"
#include "contact/store.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct jaro_contact_error error;

#define TEXT(value) {value, sizeof(value) - 1}
#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "assertion at line %d: %s\n", __LINE__, #condition); return 99; } } while (0)
#define OK(expression) do { int result = (expression); if (result) { \
	fprintf(stderr, "failure at line %d: %s (%d/%d, %s)\n", __LINE__, #expression, result, \
		error.native_code, error.reason ? error.reason : "no diagnostic"); return 99; } } while (0)

static struct jaro_contact_text
text(const char *data)
{
	return (struct jaro_contact_text){data, strlen(data)};
}

static int
equal(struct jaro_contact_text value, const char *expected)
{
	return value.size == strlen(expected) && !memcmp(value.data, expected, value.size);
}

static const struct jaro_contact_text mailboxes[] = {
	TEXT("FIRST@Example.org"), TEXT("\"A,B O'Neil\"@Example.org"), TEXT("first@example.org")
};
static const struct jaro_contact_field fields[] = {
	{TEXT("custom_漢字"), TEXT("a'oe%_\t🧪\nline 2")},
	{TEXT("empty"), TEXT("")}
};
static const char original_name[] = "Jose\xcc\x81 O’Neil\t물";
static const char original_notes[] = "Workshop Straße\n照\x1b[0m\xc2\x9b\xe2\x80\xa8" "last";

struct visit_state {
	size_t count;
	int64_t first, second;
	int revised, display;
};

static int
verify_visit(const struct jaro_contact *contact, void *data)
{
	struct visit_state *state = data;
	state->count++;
	if (state->count > 1 && state->second) {
		CHECK(contact->id == state->second && contact->revision == 1);
		CHECK(equal(contact->name, "Duplicate Name") && contact->address_count == 1);
		return 0;
	}
	CHECK(contact->id == state->first && contact->revision == 1 + state->revised);
	CHECK(equal(contact->name, state->revised ? "Updated Name" : original_name));
	CHECK(equal(contact->nickname, "Étoile") && equal(contact->notes, original_notes));
	CHECK(contact->address_count == 3 && contact->field_count == 2);
	for (size_t i = 0; i < contact->address_count; i++)
		CHECK(equal(contact->addresses[i], mailboxes[i].data));
	for (size_t i = 0; i < contact->field_count; i++) {
		CHECK(equal(contact->fields[i].key, fields[i].key.data));
		CHECK(equal(contact->fields[i].value, fields[i].value.data));
	}
	if (state->display)
		for (size_t i = 0; i < contact->address_count; i++)
			CHECK(!jaro_contact_write_mutt_row(stdout, contact->addresses[i],
				contact->name, contact->notes));
	return 0;
}

static int
count_visit(const struct jaro_contact *contact, void *data)
{
	(void)contact;
	(*(size_t *)data)++;
	return 0;
}

static int
check_invalid_edits(struct jaro_contact_store *store, struct jaro_contact fixture)
{
	const struct jaro_contact_text malformed = TEXT("hi\xed\xa0\x80");
	const struct jaro_contact_text nul = TEXT("hi\0there");
	const char *labels[] = {"name", "nickname", "notes", "custom key", "custom value"};
	int64_t id;

	for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
		for (int case_id = 0; case_id < 2; case_id++) {
			struct jaro_contact edit = fixture;
			struct jaro_contact_field custom = fields[0];
			struct jaro_contact_text bad = case_id ? malformed : nul;
			if (i == 0)
				edit.name = bad;
			else if (i == 1)
				edit.nickname = bad;
			else if (i == 2)
				edit.notes = bad;
			else {
				if (i == 3)
					custom.key = bad;
				else
					custom.value = bad;
				edit.field_count = 1;
				edit.fields = &custom;
			}
			CHECK(jaro_contact_save(store, &edit, &id, &error) == JARO_CONTACT_INVALID);
			CHECK(error.field && !strcmp(error.field, labels[i]));
			CHECK(error.ordinal == (i >= 3 ? 1u : 0u));
		}
	static const struct jaro_contact_text invalid_mailboxes[] = {
		TEXT("Name <name@example.org>"), TEXT("a@example.org,b@example.org"),
		TEXT("\"a\tb\"@example.org"), TEXT("a\xc2\x9b@example.org"),
		TEXT("a\0@example.org"), TEXT("\xf4\x90\x80\x80@example.org")
	};
	for (size_t i = 0; i < sizeof invalid_mailboxes / sizeof invalid_mailboxes[0]; i++) {
		struct jaro_contact_text items[] = {mailboxes[0], invalid_mailboxes[i]};
		struct jaro_contact attempted = fixture;
		attempted.name = text("Rejected edit");
		attempted.address_count = 2;
		attempted.addresses = items;
		CHECK(jaro_contact_save(store, &attempted, &id, &error) == JARO_CONTACT_INVALID);
		CHECK(error.field && !strcmp(error.field, "email") && error.ordinal == 2);
	}
	return 0;
}

static int
roundtrip(const char *path)
{
	struct jaro_contact_store *store, *reader;
	int64_t allow, block, custom, id, second, temporary, generation, old;
	int present;
	struct jaro_contact fixture = {
		.name = TEXT(original_name), .nickname = TEXT("Étoile"), .notes = TEXT(original_notes),
		.addresses = mailboxes, .address_count = 3, .fields = fields, .field_count = 2
	};

	OK(jaro_contact_open(path, JARO_CONTACT_CREATE_EXCLUSIVE, &store, &error));
	OK(jaro_contact_open(path, JARO_CONTACT_READ, &reader, &error));
	OK(jaro_contact_begin(store, &error));
	OK(jaro_contact_book(store, text("allow"), 1, &allow, &error));
	OK(jaro_contact_book(store, text("block"), 1, &block, &error));
	OK(jaro_contact_book(store, text("allow%'_;--漢字"), 1, &custom, &error));
	CHECK(allow != block && custom != allow && custom != block);
	fixture.book_id = allow;
	OK(jaro_contact_save(store, &fixture, &id, &error));
	fixture.book_id = 987654321;
	CHECK(jaro_contact_save(store, &fixture, &temporary, &error) == JARO_CONTACT_STORAGE);
	fixture.book_id = allow;
	struct jaro_contact copy = {.book_id = allow, .name = TEXT("Duplicate Name"),
		.addresses = mailboxes, .address_count = 1};
	OK(jaro_contact_save(store, &copy, &second, &error));
	copy.book_id = block;
	OK(jaro_contact_save(store, &copy, &temporary, &error));
	CHECK(id != second && second != temporary);
	OK(jaro_contact_generation(reader, &generation, &error));
	CHECK(generation == 0);
	OK(jaro_contact_commit(store, &error));
	OK(jaro_contact_generation(reader, &generation, &error));
	CHECK(generation == 6);
	old = generation;

	OK(jaro_contact_has_mailbox(reader, allow, text("\"first\"@example.org"), &present, &error));
	CHECK(present);
	OK(jaro_contact_has_mailbox(reader, block, text("FIRST@EXAMPLE.ORG"), &present, &error));
	CHECK(present);
	OK(jaro_contact_has_mailbox(reader, custom, text("FIRST@EXAMPLE.ORG"), &present, &error));
	CHECK(!present);
	OK(jaro_contact_has_mailbox(reader, allow, text("\"a,b o'neil\"@example.org"), &present, &error));
	CHECK(present);
	OK(jaro_contact_has_mailbox(reader, block, text("\"a,b o'neil\"@example.org"), &present, &error));
	CHECK(!present);
	OK(jaro_contact_has_mailbox(reader, allow, text("strasse@example.org"), &present, &error));
	CHECK(!present);
	CHECK(jaro_contact_book(reader, text("Allow"), 0, &temporary, &error) == JARO_CONTACT_MISSING);

	const char *hits[] = {"José", "oneil", "étoil", "strasse", "ORKSHOP", "b o'ne", "물"};
	for (size_t i = 0; i < sizeof hits / sizeof hits[0]; i++) {
		size_t count = 0;
		OK(jaro_contact_visit(reader, allow, text(hits[i]), count_visit, &count, &error));
		CHECK(count == 1);
		OK(jaro_contact_visit(reader, block, text(hits[i]), count_visit, &count, &error));
		CHECK(count == 1);
	}
	const char *misses[] = {"%_", "joneil", "💌", "josefirst", "o'e"};
	for (size_t i = 0; i < sizeof misses / sizeof misses[0]; i++) {
		size_t count = 0;
		OK(jaro_contact_visit(reader, allow, text(misses[i]), count_visit, &count, &error));
		CHECK(!count);
	}
	struct visit_state state = {.first = id, .second = second, .display = 1};
	OK(jaro_contact_visit(reader, allow, text(""), verify_visit, &state, &error));
	CHECK(state.count == 2);
	OK(jaro_contact_begin(store, &error));
	fixture.id = id;
	fixture.revision = 1;
	CHECK(!check_invalid_edits(store, fixture));
	OK(jaro_contact_generation(store, &generation, &error));
	CHECK(generation == old);
	OK(jaro_contact_commit(store, &error));
	OK(jaro_contact_close(store, &error));
	OK(jaro_contact_close(reader, &error));

	OK(jaro_contact_open(path, JARO_CONTACT_WRITE, &store, &error));
	state.count = 0;
	state.display = 0;
	OK(jaro_contact_visit(store, allow, text(""), verify_visit, &state, &error));
	CHECK(state.count == 2);
	OK(jaro_contact_begin(store, &error));
	fixture.name = text("Updated Name");
	OK(jaro_contact_save(store, &fixture, &temporary, &error));
	CHECK(temporary == id);
	CHECK(jaro_contact_save(store, &fixture, &temporary, &error) == JARO_CONTACT_CONFLICT);
	OK(jaro_contact_commit(store, &error));
	OK(jaro_contact_generation(store, &generation, &error));
	CHECK(generation == old + 1);
	state.count = 0;
	state.revised = 1;
	OK(jaro_contact_visit(store, allow, text(""), verify_visit, &state, &error));
	CHECK(state.count == 2);
	OK(jaro_contact_close(store, &error));
	return 0;
}

struct import_context {
	struct jaro_contact_store *store;
	int64_t book;
	struct jaro_contact_import_error error;
};

static int
accept_legacy(const struct jaro_legacy_record *rec, void *data)
{
	struct import_context *context = data;
	return jaro_contact_import_record(context->store, context->book, rec, NULL, NULL, &context->error);
}

static int
import_book(const char *path, const char *name, const char *source)
{
	struct import_context context = {0};
	struct jaro_legacy_error legacy = {0};
	int64_t generation;
	OK(jaro_contact_open(path, JARO_CONTACT_CREATE_EXCLUSIVE, &context.store, &error));
	OK(jaro_contact_begin(context.store, &error));
	OK(jaro_contact_book(context.store, text(name), 1, &context.book, &error));
	int status = jaro_legacy_read_at(AT_FDCWD, source, accept_legacy, &context, &legacy);
	if (status) {
		if (context.error.detail.reason)
			fprintf(stderr, "%zu %zu %zu %s\n", context.error.record, context.error.line,
				context.error.field, context.error.detail.field ? context.error.detail.field :
				context.error.detail.reason);
		else
			fprintf(stderr, "%zu %zu %zu %s\n", legacy.record, legacy.line, legacy.field, legacy.reason);
		OK(jaro_contact_rollback(context.store, &error));
		OK(jaro_contact_generation(context.store, &generation, &error));
		CHECK(generation == 0);
		OK(jaro_contact_close(context.store, &error));
		CHECK(!unlink(path));
		return 1;
	}
	OK(jaro_contact_commit(context.store, &error));
	OK(jaro_contact_close(context.store, &error));
	return 0;
}

static int
permissions(const char *path, const char *journal)
{
	struct jaro_contact_store *store, *reader;
	struct stat st;
	int64_t id;
	(void)umask(0);
	OK(jaro_contact_open(path, JARO_CONTACT_CREATE_EXCLUSIVE, &store, &error));
	OK(jaro_contact_begin(store, &error));
	OK(jaro_contact_book(store, text("permissions"), 1, &id, &error));
	CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
	CHECK(!stat(journal, &st) && (st.st_mode & 0777) == 0600);
	OK(jaro_contact_rollback(store, &error));
	CHECK(access(journal, F_OK) < 0);
	OK(jaro_contact_close(store, &error));
	OK(jaro_contact_open(path, JARO_CONTACT_READ, &reader, &error));
	CHECK(jaro_contact_begin(reader, &error) || jaro_contact_book(reader, text("no"), 1, &id, &error));
	OK(jaro_contact_rollback(reader, &error));
	OK(jaro_contact_close(reader, &error));
	CHECK(access(journal, F_OK) < 0);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "roundtrip"))
		return roundtrip(argv[2]);
	if (argc == 5 && !strcmp(argv[1], "import"))
		return import_book(argv[2], argv[3], argv[4]);
	if (argc == 4 && !strcmp(argv[1], "permissions"))
		return permissions(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "probe")) {
		struct jaro_contact_store *store;
		int result = jaro_contact_open(argv[2], JARO_CONTACT_READ, &store, &error);
		if (result) {
			fprintf(stderr, "%d %s\n", result, error.reason);
			return result;
		}
		OK(jaro_contact_close(store, &error));
		return 0;
	}
	return 99;
}
