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

static int
write_with_full(sqlite3_file *file, const void *bytes, int amount, sqlite3_int64 offset)
{
	if (request_full && !injected && written) {
		injected = 1;
		fputs("fault: SQLITE_FULL on database write after a successful write\n", stderr);
		return SQLITE_FULL;
	}
	int status = base_methods->xWrite(file, bytes, amount, offset);
	if (status == SQLITE_OK)
		written++;
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

void
jaro_contact_migration_test_point(const char *phase)
{
	const char *hold = getenv("JARO_MIGRATION_HOLD");
	if (getenv("JARO_MIGRATION_FULL") && !strcmp(phase, "before_commit"))
		request_full = 1;
	if (hold && !strcmp(hold, phase)) {
		const char *ready = getenv("JARO_MIGRATION_READY_FD");
		const char *release = getenv("JARO_MIGRATION_RELEASE_FD");
		char signal;
		if (!ready || !release || write(atoi(ready), "R", 1) != 1 ||
				read(atoi(release), &signal, 1) != 1)
			exit(98);
	}
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

int
main(int argc, char **argv)
{
	struct jaro_contact_migration_error error;
	if (argc == 5 && !strcmp(argv[1], "edit"))
		return edit_book(argv[2], argv[3], argv[4]);
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
