#define _POSIX_C_SOURCE 200809L

#include "contact/migrate.h"
#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sqlite3_vfs *base_vfs;
static sqlite3_vfs fault_vfs;
static sqlite3_io_methods fault_methods;
static const sqlite3_io_methods *base_methods;
static int request_full, injected;
static size_t written;
static void writer_point(const char *phase);

static int
write_with_full(sqlite3_file *file, const void *bytes, int amount, sqlite3_int64 offset)
{
	if (request_full && !injected && written) {
		injected = 1;
		fputs("fault: SQLITE_FULL on database write after a successful write\n", stderr);
		return SQLITE_FULL;
	}
	int status = base_methods->xWrite(file, bytes, amount, offset);
	if (status == SQLITE_OK) {
		written++;
		if (written == 1)
			writer_point("inside_commit");
	}
	return status;
}

static int
open_with_full(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *actual)
{
	(void)vfs;
	int status = base_vfs->xOpen(base_vfs, name, file, flags, actual);
	if (status == SQLITE_OK && flags & SQLITE_OPEN_MAIN_DB) {
		if (!base_methods) {
			base_methods = file->pMethods;
			fault_methods = *base_methods;
			fault_methods.xWrite = write_with_full;
		}
		if (file->pMethods != base_methods) {
			file->pMethods->xClose(file);
			return SQLITE_IOERR;
		}
		file->pMethods = &fault_methods;
	}
	return status;
}

static int
enable_full(void)
{
	base_vfs = sqlite3_vfs_find(NULL);
	if (!base_vfs)
		return SQLITE_ERROR;
	fault_vfs = *base_vfs;
	fault_vfs.zName = "jaro-test-full";
	fault_vfs.xOpen = open_with_full;
	return sqlite3_vfs_register(&fault_vfs, 1);
}

static void
test_point(const char *phase, const char *hold, const char *ready, const char *release)
{
	char byte;
	if (hold && !strcmp(hold, phase) &&
			(!ready || !release || write(atoi(ready), "R", 1) != 1 ||
			 read(atoi(release), &byte, 1) != 1))
		exit(98);
}

static void
writer_point(const char *phase)
{
	test_point(phase, getenv("JARO_WRITER_HOLD"), getenv("JARO_WRITER_READY_FD"),
		getenv("JARO_WRITER_RELEASE_FD"));
}

void
jaro_contact_migration_test_point(const char *phase)
{
	if (getenv("JARO_MIGRATION_FULL") && !strcmp(phase, "before_commit"))
		request_full = 1;
	test_point(phase, getenv("JARO_MIGRATION_HOLD"), getenv("JARO_MIGRATION_READY_FD"),
		getenv("JARO_MIGRATION_RELEASE_FD"));
}

static void
notice(void *unused, size_t source, size_t records)
{
	(void)unused;
	printf("source %zu: %zu\n", source, records);
}

struct editor {
	struct jaro_contact_store *writer;
	struct jaro_contact_text name;
	struct jaro_contact_error error;
	int edited;
};

static int
edit_first(const struct jaro_contact *before, void *data)
{
	struct editor *editor = data;
	if (editor->edited)
		return 0;
	struct jaro_contact contact = *before;
	int64_t id;
	contact.name = editor->name;
	int status = jaro_contact_save(editor->writer, &contact, &id, &editor->error);
	editor->edited = status == JARO_CONTACT_OK && id == contact.id;
	return status;
}

static int
edit_book(const char *path, const char *book, const char *name)
{
	struct jaro_contact_store *reader = NULL;
	struct jaro_contact_error error = {0};
	struct jaro_contact_text list = {book, strlen(book)};
	struct editor editor = {.name = {name, strlen(name)}};
	int64_t book_id;

	int status = jaro_contact_open(path, JARO_CONTACT_WRITE, &editor.writer, &error);
	if (!status)
		status = jaro_contact_open(path, JARO_CONTACT_READ, &reader, &error);
	if (!status)
		status = jaro_contact_book(reader, list, 0, &book_id, &error);
	if (!status)
		status = jaro_contact_begin(editor.writer, &error);
	if (!status)
		status = jaro_contact_visit(reader, book_id, (struct jaro_contact_text){0},
			edit_first, &editor, &error);
	if (!status && editor.edited)
		status = jaro_contact_commit(editor.writer, &error);
	else if (!status)
		status = JARO_CONTACT_MISSING;
	if (reader)
		jaro_contact_close(reader, NULL);
	if (editor.writer)
		jaro_contact_close(editor.writer, NULL);
	if (status)
		fprintf(stderr, "edit failed: %s\n", editor.error.reason ? editor.error.reason :
			error.reason ? error.reason : "contact missing");
	return status;
}

static struct jaro_contact_text
text(const char *value)
{
	return (struct jaro_contact_text){value, strlen(value)};
}

static int
open_profile(const char *profile, enum jaro_contact_profile_mode mode,
	struct jaro_contact_store **store, struct jaro_contact_error *error)
{
	struct jaro_contact_migration_error migration;
	int status = jaro_contact_profile_open(profile, mode, store, &migration);
	if (status)
		*error = (struct jaro_contact_error){migration.reason, migration.field,
			migration.field_number, migration.native_code};
	return status;
}

static int
write_rows(const struct jaro_contact *contact, void *unused)
{
	(void)unused;
	for (size_t i = 0; i < contact->address_count; i++)
		if (jaro_contact_write_mutt_row(stdout, contact->addresses[i], contact->name, contact->notes))
			return 1;
	return 0;
}

static int
profile_command(int argc, char **argv)
{
	struct jaro_contact_error error = {0};
	struct jaro_contact_store *store = NULL;
	struct jaro_contact_text book = text(argv[3]);
	int64_t book_id, id;
	int status, present, added;
	int preview = !strcmp(argv[1], "preview");
	int learn = preview || !strcmp(argv[1], "learn") || !strcmp(argv[1], "import");
	int lookup = !strcmp(argv[1], "member");
	int query = !strcmp(argv[1], "query");
	int save = !strcmp(argv[1], "save");
	int remove = !strcmp(argv[1], "delete");
	int cancel = !strcmp(argv[1], "cancel");
	int full = getenv("JARO_WRITER_FULL") != NULL;
	const char *hold = getenv("JARO_WRITER_HOLD");
	struct jaro_contact loaded = {0}, pending;

	if (!(argc == 5 && (lookup || query || remove)) &&
			!(argc == 6 && (learn || save || cancel)))
		return 99;
	if (full || (hold && !strcmp(hold, "inside_commit")))
		if (enable_full() != SQLITE_OK)
			return 97;
	status = open_profile(argv[2], preview ? JARO_CONTACT_PROFILE_PREVIEW :
		learn ? JARO_CONTACT_PROFILE_WRITE : JARO_CONTACT_PROFILE_READ, &store, &error);
	if (status)
		goto done;
	if (learn) {
		writer_point("before_learn");
		status = jaro_contact_learn(store, book, text(argv[4]), text(argv[5]), preview, &added, &error);
		if (!status)
			puts(added ? "new" : "present");
		goto done;
	}
	status = jaro_contact_book(store, book, 0, &book_id, &error);
	if (status == JARO_CONTACT_MISSING && (lookup || query)) {
		if (lookup)
			puts("absent");
		status = JARO_CONTACT_OK;
		goto done;
	}
	if (status)
		goto done;
	if (lookup) {
		status = jaro_contact_has_mailbox(store, book_id, text(argv[4]), &present, &error);
		if (!status)
			puts(present ? "present" : "absent");
		goto done;
	}
	if (query) {
		status = jaro_contact_visit(store, book_id, text(argv[4]), write_rows, NULL, &error);
		goto done;
	}
	id = strtoll(argv[4], NULL, 10);
	status = jaro_contact_get(store, book_id, id, &loaded, &error);
	if (status)
		goto done;
	status = jaro_contact_close(store, &error);
	if (status)
		goto done;
	store = NULL;
	pending = loaded;
	if (!remove)
		pending.name = text(argv[5]);
	writer_point("draft");
	if (cancel)
		goto done;
	status = open_profile(argv[2], JARO_CONTACT_PROFILE_WRITE, &store, &error);
	if (!status)
		status = jaro_contact_begin(store, &error);
	if (!status) {
		status = remove ? jaro_contact_delete(store, book_id, id, pending.revision, &error) :
			jaro_contact_save(store, &pending, &id, &error);
		if (!status) {
			writer_point("before_commit");
			if (full)
				request_full = 1;
		}
		status = jaro_contact_finish(store, status, &error);
		if (!status)
			writer_point("after_commit");
	}
	if (full && !injected)
		status = 96;
	if (status && save)
		printf("pending revision=%lld name=%.*s addresses=%zu\n", (long long)pending.revision,
			(int)pending.name.size, pending.name.data, pending.address_count);

done:
	jaro_contact_clear(&loaded);
	if (store)
		jaro_contact_close(store, NULL);
	if (status)
		fprintf(stderr, "%d native=%d %s%s%s\n", status, error.native_code,
			error.reason ? error.reason : "missing diagnostic",
			error.field ? ": " : "", error.field ? error.field : "");
	return status;
}

int
main(int argc, char **argv)
{
	struct jaro_contact_migration_error error;
	if (argc == 5 && !strcmp(argv[1], "edit"))
		return edit_book(argv[2], argv[3], argv[4]);
	if (argc >= 5)
		return profile_command(argc, argv);
	if (argc != 3 || strcmp(argv[1], "init"))
		return 99;
	if (getenv("JARO_MIGRATION_FULL") && enable_full() != SQLITE_OK)
		return 97;
	int status = jaro_contact_initialize(argv[2], notice, NULL, &error);
	if (getenv("JARO_MIGRATION_FULL") && !injected)
		return 96;
	if (status)
		fprintf(stderr, "%d source=%zu record=%zu line=%zu field=%zu native=%d %s%s%s\n",
			status, error.source, error.record, error.line, error.field_number,
			error.native_code, error.reason ? error.reason : "missing diagnostic",
			error.field ? ": " : "", error.field ? error.field : "");
	return status;
}
