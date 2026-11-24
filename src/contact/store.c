#define _POSIX_C_SOURCE 200809L

#include "contact/store.h"
#include "contact/text.h"
#include "search-fold.h"
#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct jaro_contact_store {
	sqlite3 *db;
};

#define STRINGIFY_ARG(x) #x
#define STRINGIFY(x) STRINGIFY_ARG(x)

static const char schema[] =
	"PRAGMA application_id=" STRINGIFY(JARO_CONTACT_APPLICATION_ID) ";"
	"PRAGMA user_version=" STRINGIFY(JARO_CONTACT_SCHEMA_VERSION) ";"
	"CREATE TABLE metadata (singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
	" generation INTEGER NOT NULL CHECK(generation>=0)) STRICT;"
	"INSERT INTO metadata VALUES(1,0);"
	"CREATE TABLE books (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE) STRICT;"
	"CREATE TABLE contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, book_id INTEGER NOT NULL"
	" REFERENCES books(id), ordinal INTEGER NOT NULL CHECK(ordinal>=0),"
	" revision INTEGER NOT NULL CHECK(revision>0), name TEXT NOT NULL, nickname TEXT NOT NULL,"
	" notes TEXT NOT NULL, name_key TEXT NOT NULL, nickname_key TEXT NOT NULL,"
	" notes_key TEXT NOT NULL, UNIQUE(book_id,ordinal), UNIQUE(id,book_id)) STRICT;"
	"CREATE TABLE addresses (contact_id INTEGER NOT NULL, book_id INTEGER NOT NULL,"
	" ordinal INTEGER NOT NULL CHECK(ordinal>=0), spelling TEXT NOT NULL, mailbox_key TEXT NOT NULL,"
	" search_key TEXT NOT NULL, PRIMARY KEY(contact_id,ordinal),"
	" FOREIGN KEY(contact_id,book_id) REFERENCES contacts(id,book_id) ON DELETE CASCADE)"
	" WITHOUT ROWID, STRICT;"
	"CREATE INDEX addresses_book_mailbox ON addresses(book_id,mailbox_key);"
	"CREATE TABLE fields (contact_id INTEGER NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,"
	" ordinal INTEGER NOT NULL CHECK(ordinal>=0), key TEXT NOT NULL, value TEXT NOT NULL,"
	" PRIMARY KEY(contact_id,ordinal), UNIQUE(contact_id,key)) WITHOUT ROWID, STRICT;"
	"CREATE TRIGGER book_added AFTER INSERT ON books BEGIN"
	" UPDATE metadata SET generation=generation+1; END;"
	"CREATE TRIGGER contact_added AFTER INSERT ON contacts BEGIN"
	" UPDATE metadata SET generation=generation+1; END;"
	"CREATE TRIGGER contact_changed AFTER UPDATE ON contacts BEGIN"
	" UPDATE metadata SET generation=generation+1; END;"
	"CREATE TRIGGER contact_removed AFTER DELETE ON contacts BEGIN"
	" UPDATE metadata SET generation=generation+1; END;";

static const char has_mailbox_sql[] =
	"SELECT 1 FROM addresses INDEXED BY addresses_book_mailbox"
	" WHERE book_id=?1 AND mailbox_key=?2 LIMIT 1";

static int
failure(struct jaro_contact_error *error, int result, const char *reason,
	const char *field, size_t ordinal, int native_code)
{
	if (error)
		*error = (struct jaro_contact_error){reason, field, ordinal, native_code};
	return result;
}

static int
sql_failure(struct jaro_contact_error *error, int code, const char *reason)
{
	int primary = code & 0xff;
	int result = primary == SQLITE_BUSY || primary == SQLITE_LOCKED ?
		JARO_CONTACT_BUSY : JARO_CONTACT_STORAGE;
	return failure(error, result, reason, NULL, 0, code);
}

static int
execute(sqlite3 *db, const char *sql, struct jaro_contact_error *error, const char *reason)
{
	int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
	return rc == SQLITE_OK ? JARO_CONTACT_OK : sql_failure(error, rc, reason);
}

static int
read_integer(sqlite3 *db, const char *query, sqlite3_int64 *value)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db, query, -1, &statement, NULL);
	if (rc == SQLITE_OK) {
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
			*value = sqlite3_column_int64(statement, 0);
			rc = sqlite3_step(statement);
			if (rc == SQLITE_DONE)
				rc = SQLITE_OK;
		} else if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
			rc = SQLITE_CORRUPT;
		}
	}
	sqlite3_finalize(statement);
	return rc;
}

static int
check_setting(sqlite3 *db, const char *query, sqlite3_int64 expected,
	struct jaro_contact_error *error, const char *reason)
{
	sqlite3_int64 actual = 0;
	int rc = read_integer(db, query, &actual);
	if (rc == SQLITE_OK && actual != expected)
		rc = SQLITE_MISMATCH;
	return rc == SQLITE_OK ? JARO_CONTACT_OK : sql_failure(error, rc, reason);
}

static int
check_journal(sqlite3 *db, int create, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	const char *query = create ? "PRAGMA journal_mode=DELETE" : "PRAGMA journal_mode";
	int rc = sqlite3_prepare_v2(db, query, -1, &statement, NULL);
	if (rc == SQLITE_OK) {
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW) {
			const unsigned char *value = sqlite3_column_text(statement, 0);
			if (!value || strcmp((const char *)value, "delete"))
				rc = SQLITE_MISMATCH;
			else {
				rc = sqlite3_step(statement);
				if (rc == SQLITE_DONE)
					rc = SQLITE_OK;
			}
		}
	}
	sqlite3_finalize(statement);
	return rc == SQLITE_OK ? JARO_CONTACT_OK : sql_failure(error, rc, "unsupported journal mode");
}

static int
configure(sqlite3 *db, int create, int memory, struct jaro_contact_error *error)
{
	int rc = sqlite3_extended_result_codes(db, 1);
	if (rc == SQLITE_OK)
		rc = sqlite3_busy_timeout(db, 1000);
	if (rc == SQLITE_OK)
		rc = sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "cannot configure contact database");
	int status = execute(db,
		"PRAGMA foreign_keys=ON; PRAGMA synchronous=EXTRA; PRAGMA trusted_schema=OFF;",
		error, "cannot configure contact database");
	if (!status)
		status = check_setting(db, "PRAGMA foreign_keys", 1, error, "foreign keys unavailable");
	if (!status)
		status = check_setting(db, "PRAGMA synchronous", 3, error, "durability setting unavailable");
	if (!status && !memory)
		status = check_journal(db, create, error);
	return status;
}

static int
check_schema(sqlite3 *db, struct jaro_contact_health *health, struct jaro_contact_error *error)
{
	sqlite3_int64 version = 0, generation = 0;
	sqlite3_stmt *statement = NULL;
	int status = check_setting(db, "PRAGMA application_id", JARO_CONTACT_APPLICATION_ID,
		error, "invalid contact database identity");
	if (status)
		return status;
	int rc = read_integer(db, "PRAGMA user_version", &version);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "cannot read contact schema version");
	if (health) {
		health->schema_known = 1;
		health->schema_version = version;
	}
	if (version != JARO_CONTACT_SCHEMA_VERSION)
		return sql_failure(error, SQLITE_MISMATCH, "unsupported contact schema version");
	rc = read_integer(db, "SELECT generation FROM metadata WHERE singleton=1", &generation);
	if (rc == SQLITE_OK && generation < 0)
		rc = SQLITE_CORRUPT;
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "invalid contact metadata");
	rc = sqlite3_prepare_v2(db,
		"SELECT b.id,b.name,c.id,c.book_id,c.ordinal,c.revision,c.name,c.nickname,c.notes,"
		" c.name_key,c.nickname_key,c.notes_key,a.contact_id,a.book_id,a.ordinal,a.spelling,"
		" a.mailbox_key,a.search_key,f.contact_id,f.ordinal,f.key,f.value FROM books b"
		" CROSS JOIN contacts c CROSS JOIN addresses a INDEXED BY addresses_book_mailbox"
		" CROSS JOIN fields f LIMIT 0", -1, &statement, NULL);
	sqlite3_finalize(statement);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "invalid contact database schema");
	return check_setting(db, "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND"
		" name IN ('book_added','contact_added','contact_changed','contact_removed')",
		4, error, "invalid contact change tracking");
}

static int
open_connection(const char *path, int flags, int create, int memory,
	struct jaro_contact_store **store, struct jaro_contact_error *error)
{
	struct jaro_contact_store *opened = calloc(1, sizeof *opened);
	if (!opened)
		return failure(error, JARO_CONTACT_STORAGE, "cannot allocate contact database",
			NULL, 0, ENOMEM);
	int rc = sqlite3_open_v2(path, &opened->db,
		flags | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE, NULL);
	if (rc == SQLITE_OK && flags & SQLITE_OPEN_READWRITE && sqlite3_db_readonly(opened->db, "main"))
		rc = SQLITE_READONLY;
	int status = rc == SQLITE_OK ? configure(opened->db, create, memory, error) :
		sql_failure(error, rc, "cannot open contact database");
	if (status) {
		sqlite3_close(opened->db);
		free(opened);
		return status;
	}
	*store = opened;
	return JARO_CONTACT_OK;
}

static int
open_database(const char *path, int flags, int create, int memory,
	struct jaro_contact_store **store, struct jaro_contact_error *error)
{
	struct jaro_contact_store *opened;
	int status = open_connection(path, flags, create, memory, &opened, error);
	if (status)
		return status;
	if (create)
		status = execute(opened->db, "BEGIN IMMEDIATE", error, "cannot begin contact schema");
	if (!status && create)
		status = execute(opened->db, schema, error, "cannot create contact schema");
	if (!status && create)
		status = execute(opened->db, "COMMIT", error, "cannot commit contact schema");
	if (!status)
		status = check_schema(opened->db, NULL, error);
	if (status) {
		jaro_contact_close(opened, NULL); /* SQLite rolls back unfinished transactions. */
		return status;
	}
	*store = opened;
	return JARO_CONTACT_OK;
}

int
jaro_contact_open(const char *path, enum jaro_contact_open_mode mode,
	struct jaro_contact_store **store, struct jaro_contact_error *error)
{
	struct stat st;
	int create = mode == JARO_CONTACT_CREATE_EXCLUSIVE;
	int status, fd;
	int flags = mode == JARO_CONTACT_READ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;

	*store = NULL;
	if (error)
		*error = (struct jaro_contact_error){0};
	if (create) {
		fd = open(path, O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_RDWR, 0600);
		if (fd < 0)
			return failure(error, JARO_CONTACT_STORAGE, "cannot create private contact database",
				NULL, 0, errno);
		status = fchmod(fd, 0600);
		int saved = errno;
		if (close(fd) && !status) {
			status = -1;
			saved = errno;
		}
		if (status) {
			unlink(path);
			return failure(error, JARO_CONTACT_STORAGE, "cannot secure contact database",
				NULL, 0, saved);
		}
	}
	if (lstat(path, &st) < 0) {
		int saved = errno;
		if (create)
			unlink(path);
		return failure(error, saved == ENOENT ? JARO_CONTACT_MISSING : JARO_CONTACT_STORAGE,
			"cannot inspect contact database", NULL, 0, saved);
	}
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
		if (create)
			unlink(path);
		return failure(error, JARO_CONTACT_STORAGE, "contact database must be a private regular file",
			NULL, 0, 0);
	}
	status = open_database(path, flags, create, 0, store, error);
	if (status && create)
		unlink(path);
	return status;
}

int
jaro_contact_open_memory(struct jaro_contact_store **store, struct jaro_contact_error *error)
{
	*store = NULL;
	return open_database(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY,
		1, 1, store, error);
}

int
jaro_contact_close(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	int rc = sqlite3_close(store->db);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "cannot close contact database");
	free(store);
	return JARO_CONTACT_OK;
}

static int
verify_integrity(struct jaro_contact_store *store, const char *pragma,
	struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(store->db, pragma, -1, &statement, NULL);
	if (rc == SQLITE_OK) {
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW) {
			const unsigned char *value = sqlite3_column_text(statement, 0);
			rc = value && !strcmp((const char *)value, "ok") ? SQLITE_OK : SQLITE_CORRUPT;
		} else if (rc == SQLITE_DONE) {
			rc = SQLITE_CORRUPT;
		}
	}
	sqlite3_finalize(statement);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "contact database integrity check failed");
	rc = sqlite3_prepare_v2(store->db, "PRAGMA foreign_key_check", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (rc == SQLITE_ROW)
		rc = SQLITE_CONSTRAINT_FOREIGNKEY;
	return rc == SQLITE_DONE ? JARO_CONTACT_OK : sql_failure(error, rc, "invalid contact foreign keys");
}

int
jaro_contact_verify(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	return verify_integrity(store, "PRAGMA quick_check(1)", error);
}

static int
wal_header(const char *path)
{
	unsigned char header[20];
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return 0;
	ssize_t size = read(fd, header, sizeof header);
	close(fd);
	/* Bytes 18 and 19 are SQLite's database write and read format versions. */
	return size == (ssize_t)sizeof header && !memcmp(header, "SQLite format 3", 16) &&
		header[18] == 2 && header[19] == 2;
}

enum jaro_contact_diagnosis
jaro_contact_diagnose(const char *path, struct jaro_contact_health *health)
{
	struct jaro_contact_store *store;
	struct jaro_contact_error error = {0};
	struct stat st;
	sqlite3_int64 books = 0, contacts = 0;

	*health = (struct jaro_contact_health){.mode = -1};
	if (lstat(path, &st) < 0)
		return errno == ENOENT ? JARO_CONTACT_DIAG_ABSENT : JARO_CONTACT_DIAG_UNREADABLE;
	health->mode = st.st_mode & 07777;
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077))
		return JARO_CONTACT_DIAG_PERMISSIONS;
	if (!(st.st_mode & S_IRUSR))
		return JARO_CONTACT_DIAG_UNREADABLE;

	/* This build supports only rollback journaling. Omitting WAL also
	 * prevents SQLite from creating sidecars for an unsupported WAL file. */
	int status = open_connection(path, SQLITE_OPEN_READONLY, 0, 0, &store, &error);
	if (!status) {
		status = jaro_contact_begin_read(store, &error);
		if (!status)
			status = check_schema(store->db, health, &error);
		if (!status)
			status = verify_integrity(store, "PRAGMA integrity_check(1)", &error);
		if (!status) {
			int rc = read_integer(store->db, "SELECT count(*) FROM books", &books);
			if (rc == SQLITE_OK)
				rc = read_integer(store->db, "SELECT count(*) FROM contacts", &contacts);
			if (rc != SQLITE_OK)
				status = sql_failure(&error, rc, "cannot count contact records");
		}
		status = jaro_contact_finish(store, status, &error);
		int closed = jaro_contact_close(store, status ? NULL : &error);
		if (!status)
			status = closed;
	}
	if (!status) {
		health->books = books;
		health->contacts = contacts;
		return JARO_CONTACT_DIAG_OK;
	}
	if (status == JARO_CONTACT_BUSY)
		return JARO_CONTACT_DIAG_LOCKED;
	if (health->schema_known && health->schema_version != JARO_CONTACT_SCHEMA_VERSION)
		return JARO_CONTACT_DIAG_SCHEMA;
	if (error.native_code == SQLITE_READONLY_ROLLBACK)
		return JARO_CONTACT_DIAG_RECOVERY;
	int code = error.native_code & 0xff;
	if (code == SQLITE_NOTADB && wal_header(path))
		return JARO_CONTACT_DIAG_JOURNAL;
	switch (code) {
	case SQLITE_CORRUPT: case SQLITE_NOTADB: case SQLITE_CONSTRAINT:
	case SQLITE_MISMATCH: case SQLITE_ERROR:
		return JARO_CONTACT_DIAG_CORRUPT;
	default:
		return JARO_CONTACT_DIAG_UNREADABLE;
	}
}

int
jaro_contact_begin(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	return execute(store->db, "BEGIN IMMEDIATE", error, "cannot begin contact write");
}

int
jaro_contact_begin_read(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	return execute(store->db, "BEGIN", error, "cannot begin contact read");
}

int
jaro_contact_commit(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	return execute(store->db, "COMMIT", error, "cannot commit contact write");
}

int
jaro_contact_rollback(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	if (sqlite3_get_autocommit(store->db))
		return JARO_CONTACT_OK;
	return execute(store->db, "ROLLBACK", error, "cannot roll back contact transaction");
}

int
jaro_contact_finish(struct jaro_contact_store *store, int status, struct jaro_contact_error *error)
{
	if (!status)
		status = jaro_contact_commit(store, error);
	if (status) {
		int rollback = jaro_contact_rollback(store, error);
		if (rollback)
			return rollback;
	}
	return status;
}

static int
bind_text(sqlite3_stmt *statement, int index, struct jaro_contact_text text)
{
	return sqlite3_bind_text64(statement, index, text.size ? text.data : "",
		text.size, SQLITE_TRANSIENT, SQLITE_UTF8);
}

static int
require_write(struct jaro_contact_store *store, struct jaro_contact_error *error)
{
	if (sqlite3_txn_state(store->db, "main") == SQLITE_TXN_WRITE)
		return JARO_CONTACT_OK;
	return failure(error, JARO_CONTACT_STORAGE, "contact write requires a write transaction",
		NULL, 0, SQLITE_MISUSE);
}

int
jaro_contact_book(struct jaro_contact_store *store, struct jaro_contact_text name,
	int create, int64_t *book_id, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int rc, status;

	if (!name.size || !jaro_contact_valid_text(name))
		return failure(error, JARO_CONTACT_INVALID, "invalid contact book name", "book", 0, 0);
	if (create) {
		status = require_write(store, error);
		if (status)
			return status;
		rc = sqlite3_prepare_v2(store->db,
			"INSERT INTO books(name) VALUES(?1) ON CONFLICT(name) DO NOTHING",
			-1, &statement, NULL);
		if (rc == SQLITE_OK)
			rc = bind_text(statement, 1, name);
		if (rc == SQLITE_OK)
			rc = sqlite3_step(statement);
		sqlite3_finalize(statement);
		statement = NULL;
		if (rc != SQLITE_DONE)
			return sql_failure(error, rc, "cannot create contact book");
	}
	rc = sqlite3_prepare_v2(store->db, "SELECT id FROM books WHERE name=?1", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = bind_text(statement, 1, name);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW)
		*book_id = sqlite3_column_int64(statement, 0);
	sqlite3_finalize(statement);
	if (rc == SQLITE_DONE)
		return failure(error, JARO_CONTACT_MISSING, "contact book does not exist", "book", 0, 0);
	return rc == SQLITE_ROW ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot find contact book");
}

int
jaro_contact_visit_books(struct jaro_contact_store *store,
	jaro_contact_book_visitor visit, void *data, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(store->db, "SELECT name FROM books ORDER BY id",
		-1, &statement, NULL);
	int status = JARO_CONTACT_OK;

	if (rc == SQLITE_OK) {
		while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(statement, 0);
			if (!name) {
				rc = SQLITE_NOMEM;
				break;
			}
			if (visit((struct jaro_contact_text){name, (size_t)sqlite3_column_bytes(statement, 0)}, data)) {
				status = failure(error, JARO_CONTACT_CONSUMER, "contact list visitor failed", NULL, 0, 0);
				break;
			}
		}
	}
	sqlite3_finalize(statement);
	if (status)
		return status;
	return rc == SQLITE_DONE ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot list contact books");
}

int
jaro_contact_generation(struct jaro_contact_store *store, int64_t *generation,
	struct jaro_contact_error *error)
{
	sqlite3_int64 value = 0;
	int rc = read_integer(store->db, "SELECT generation FROM metadata WHERE singleton=1", &value);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "cannot read contact change indicator");
	*generation = value;
	return JARO_CONTACT_OK;
}

int
jaro_contact_has_mailbox(struct jaro_contact_store *store, int64_t book_id,
	struct jaro_contact_text mailbox, int *present, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	char *key;
	int status = jaro_contact_mailbox_key(mailbox, &key);
	if (status)
		return failure(error, status, "cannot normalize mailbox", "email", 0, 0);
	int rc = sqlite3_prepare_v2(store->db, has_mailbox_sql, -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, book_id);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_text(statement, 2, key, -1, SQLITE_TRANSIENT);
	free(key);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (rc != SQLITE_DONE && rc != SQLITE_ROW)
		return sql_failure(error, rc, "cannot look up contact mailbox");
	*present = rc == SQLITE_ROW;
	return JARO_CONTACT_OK;
}

static int
validate_contact(const struct jaro_contact *contact, struct jaro_contact_error *error)
{
	const char *names[] = {"name", "nickname", "notes"};
	const struct jaro_contact_text values[] = {contact->name, contact->nickname, contact->notes};
	size_t bytes = 0;

	if (contact->book_id < 1 || contact->id < 0 || (contact->id && contact->revision < 1))
		return failure(error, JARO_CONTACT_INVALID, "invalid contact identity", NULL, 0, 0);
	if (!contact->name.size)
		return failure(error, JARO_CONTACT_INVALID, "a name is required", "name", 0, 0);
	for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
		if (!jaro_contact_valid_text(values[i]))
			return failure(error, JARO_CONTACT_INVALID, "invalid UTF-8 text, embedded NUL or size",
				names[i], 0, 0);
		bytes += values[i].size;
	}
	if (contact->address_count > JARO_CONTACT_ADDRESSES_MAX ||
			(contact->address_count && !contact->addresses))
		return failure(error, JARO_CONTACT_INVALID, "invalid address count", "email", 0, 0);
	if (contact->field_count > JARO_CONTACT_FIELDS_MAX || (contact->field_count && !contact->fields))
		return failure(error, JARO_CONTACT_INVALID, "invalid custom field count", "custom", 0, 0);
	for (size_t i = 0; i < contact->field_count; i++) {
		const struct jaro_contact_field *field = &contact->fields[i];
		if (!field->key.size || !jaro_contact_valid_text(field->key))
			return failure(error, JARO_CONTACT_INVALID, "invalid UTF-8 text, embedded NUL or size",
				"custom key", i + 1, 0);
		if (!jaro_contact_valid_text(field->value))
			return failure(error, JARO_CONTACT_INVALID, "invalid UTF-8 text, embedded NUL or size",
				"custom value", i + 1, 0);
		bytes += field->key.size + field->value.size;
		if (bytes > JARO_CONTACT_RECORD_MAX)
			return failure(error, JARO_CONTACT_INVALID, "contact size limit exceeded",
				"custom value", i + 1, 0);
		for (size_t j = 0; j < i; j++)
			if (field->key.size == contact->fields[j].key.size &&
					!memcmp(field->key.data, contact->fields[j].key.data, field->key.size))
				return failure(error, JARO_CONTACT_INVALID, "duplicate custom key",
					"custom key", i + 1, 0);
	}
	for (size_t i = 0; i < contact->address_count; i++) {
		if (contact->addresses[i].size > JARO_CONTACT_RECORD_MAX - bytes)
			return failure(error, JARO_CONTACT_INVALID, "contact size limit exceeded", "email", i + 1, 0);
		bytes += contact->addresses[i].size;
	}
	return JARO_CONTACT_OK;
}

static int
bind_search_key(sqlite3_stmt *statement, int position, struct jaro_contact_text text)
{
	char *key = abook_search_key_bytes(text.data, text.size);
	if (!key)
		return SQLITE_NOMEM;
	return sqlite3_bind_text64(statement, position, key, strlen(key), free, SQLITE_UTF8);
}

static int
save_contact_row(sqlite3 *db, const struct jaro_contact *contact,
	sqlite3_int64 *saved, struct jaro_contact_error *error)
{
	const struct jaro_contact_text text[] = {contact->name, contact->nickname, contact->notes};
	const char *sql = contact->id ?
		"UPDATE contacts SET name=?2,nickname=?3,notes=?4,name_key=?5,nickname_key=?6,"
		"notes_key=?7,revision=revision+1 WHERE book_id=?1 AND id=?8 AND revision=?9" :
		"INSERT INTO contacts(book_id,name,nickname,notes,name_key,nickname_key,notes_key,ordinal,revision)"
		" VALUES(?1,?2,?3,?4,?5,?6,?7,"
		"(SELECT COALESCE(MAX(ordinal)+1,0) FROM contacts WHERE book_id=?1),1)";
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);

	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, contact->book_id);
	for (int i = 0; rc == SQLITE_OK && i < 3; i++)
		rc = bind_text(statement, i + 2, text[i]);
	for (int i = 0; rc == SQLITE_OK && i < 3; i++)
		rc = bind_search_key(statement, i + 5, text[i]);
	if (rc == SQLITE_OK && contact->id)
		rc = sqlite3_bind_int64(statement, 8, contact->id);
	if (rc == SQLITE_OK && contact->id)
		rc = sqlite3_bind_int64(statement, 9, contact->revision);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (rc != SQLITE_DONE)
		return sql_failure(error, rc, "cannot save contact row");
	if (contact->id && sqlite3_changes(db) != 1)
		return failure(error, JARO_CONTACT_CONFLICT, "contact revision has changed", NULL, 0, 0);
	*saved = contact->id ? contact->id : sqlite3_last_insert_rowid(db);
	return JARO_CONTACT_OK;
}

static int
delete_children(sqlite3 *db, const char *sql, sqlite3_int64 id)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int
save_addresses(sqlite3 *db, const struct jaro_contact *contact, sqlite3_int64 id,
	struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int status = JARO_CONTACT_OK;
	int rc = sqlite3_prepare_v2(db, "INSERT INTO addresses"
		"(contact_id,book_id,ordinal,spelling,mailbox_key,search_key) VALUES(?1,?2,?3,?4,?5,?6)",
		-1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 2, contact->book_id);
	for (size_t i = 0; rc == SQLITE_OK && i < contact->address_count; i++) {
		struct jaro_contact_text address = contact->addresses[i];
		char *key;
		status = jaro_contact_mailbox_key(address, &key);
		if (status) {
			failure(error, status, status == JARO_CONTACT_INVALID ?
				"invalid deliverable mailbox" : "cannot allocate mailbox key",
				"email", i + 1, status == JARO_CONTACT_STORAGE ? ENOMEM : 0);
			break;
		}
		rc = sqlite3_bind_int64(statement, 3, (sqlite3_int64)i);
		if (rc == SQLITE_OK)
			rc = bind_text(statement, 4, address);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_text(statement, 5, key, -1, SQLITE_TRANSIENT);
		free(key);
		if (rc == SQLITE_OK)
			rc = bind_search_key(statement, 6, address);
		if (rc == SQLITE_OK) {
			rc = sqlite3_step(statement);
			if (rc == SQLITE_DONE)
				rc = sqlite3_reset(statement);
		}
	}
	sqlite3_finalize(statement);
	if (status)
		return status;
	return rc == SQLITE_OK ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot save contact addresses");
}

static int
save_fields(sqlite3 *db, const struct jaro_contact *contact, sqlite3_int64 id,
	struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db, "INSERT INTO fields(contact_id,ordinal,key,value) VALUES(?1,?2,?3,?4)",
		-1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	for (size_t i = 0; rc == SQLITE_OK && i < contact->field_count; i++) {
		rc = sqlite3_bind_int64(statement, 2, (sqlite3_int64)i);
		if (rc == SQLITE_OK)
			rc = bind_text(statement, 3, contact->fields[i].key);
		if (rc == SQLITE_OK)
			rc = bind_text(statement, 4, contact->fields[i].value);
		if (rc == SQLITE_OK) {
			rc = sqlite3_step(statement);
			if (rc == SQLITE_DONE)
				rc = sqlite3_reset(statement);
		}
	}
	sqlite3_finalize(statement);
	return rc == SQLITE_OK ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot save custom fields");
}

int
jaro_contact_save(struct jaro_contact_store *store, const struct jaro_contact *contact,
	int64_t *id, struct jaro_contact_error *error)
{
	sqlite3 *db = store->db;
	sqlite3_int64 saved = 0;
	int status = require_write(store, error);
	if (!status)
		status = validate_contact(contact, error);
	if (!status)
		status = execute(db, "SAVEPOINT contact_edit", error, "cannot begin contact edit");
	if (status)
		return status;

	status = save_contact_row(db, contact, &saved, error);
	if (!status && contact->id) {
		int rc = delete_children(db, "DELETE FROM addresses WHERE contact_id=?1", saved);
		if (rc == SQLITE_OK)
			rc = delete_children(db, "DELETE FROM fields WHERE contact_id=?1", saved);
		if (rc != SQLITE_OK)
			status = sql_failure(error, rc, "cannot replace contact fields");
	}
	if (!status && contact->address_count)
		status = save_addresses(db, contact, saved, error);
	if (!status && contact->field_count)
		status = save_fields(db, contact, saved, error);
	if (!status)
		status = execute(db, "RELEASE contact_edit", error, "cannot finish contact edit");
	if (status) {
		if (sqlite3_get_autocommit(db))
			return status;
		int rc = sqlite3_exec(db, "ROLLBACK TO contact_edit; RELEASE contact_edit", NULL, NULL, NULL);
		if (rc != SQLITE_OK) {
			sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
			return sql_failure(error, rc, "cannot restore contact edit");
		}
		return status;
	}
	*id = saved;
	return JARO_CONTACT_OK;
}

int
jaro_contact_delete(struct jaro_contact_store *store, int64_t book_id, int64_t id,
	int64_t revision, struct jaro_contact_error *error)
{
	int status = require_write(store, error);
	if (status)
		return status;
	if (book_id < 1 || id < 1 || revision < 1)
		return failure(error, JARO_CONTACT_INVALID, "invalid contact identity", NULL, 0, 0);

	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(store->db,
		"DELETE FROM contacts WHERE book_id=?1 AND id=?2 AND revision=?3", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, book_id);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 2, id);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 3, revision);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (rc != SQLITE_DONE)
		return sql_failure(error, rc, "cannot remove contact");
	if (sqlite3_changes(store->db) != 1)
		return failure(error, JARO_CONTACT_CONFLICT, "contact revision has changed", NULL, 0, 0);
	return JARO_CONTACT_OK;
}

int
jaro_contact_learn(struct jaro_contact_store *store, struct jaro_contact_text book,
	struct jaro_contact_text mailbox, struct jaro_contact_text name, int preview,
	int *added, struct jaro_contact_error *error)
{
	struct jaro_contact contact = {
		.name = name.size ? name : mailbox, .addresses = &mailbox, .address_count = 1
	};
	int present = 0, owned = sqlite3_get_autocommit(store->db);

	*added = 0;
	if (!jaro_contact_valid_text(name))
		return failure(error, JARO_CONTACT_INVALID, "invalid UTF-8 text, embedded NUL or size",
			"name", 0, 0);
	int status = preview ? jaro_contact_begin_read(store, error) :
		owned ? jaro_contact_begin(store, error) : require_write(store, error);
	if (status)
		return status;
	status = jaro_contact_book(store, book, 0, &contact.book_id, error);
	if (status == JARO_CONTACT_MISSING) {
		if (error)
			*error = (struct jaro_contact_error){0};
		status = JARO_CONTACT_OK; /* A zero book ID cannot match an address. */
	}
	if (!status)
		status = jaro_contact_has_mailbox(store, contact.book_id, mailbox, &present, error);
	if (!status && !present && !preview) {
		int64_t id;
		if (!contact.book_id)
			status = jaro_contact_book(store, book, 1, &contact.book_id, error);
		if (!status)
			status = jaro_contact_save(store, &contact, &id, error);
	}
	if (owned)
		status = jaro_contact_finish(store, status, error);
	if (!status)
		*added = !present;
	return status;
}

int
jaro_contact_record_import(struct jaro_contact_store *store, char token[33],
	struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int status = require_write(store, error);
	if (!status)
		status = execute(store->db, "CREATE TABLE IF NOT EXISTS filter_import("
			"singleton INTEGER PRIMARY KEY CHECK(singleton=1),token TEXT NOT NULL) STRICT",
			error, "cannot prepare filter import receipt");
	if (status)
		return status;
	int rc = sqlite3_prepare_v2(store->db,
		"INSERT OR REPLACE INTO filter_import VALUES (1,lower(hex(randomblob(16)))) RETURNING token",
		-1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW && sqlite3_column_bytes(statement, 0) == 32) {
		memcpy(token, sqlite3_column_text(statement, 0), 32);
		token[32] = 0;
		rc = sqlite3_step(statement);
	} else if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_CORRUPT;
	sqlite3_finalize(statement);
	return rc == SQLITE_DONE ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot record filter import");
}

int
jaro_contact_has_import(struct jaro_contact_store *store, const char *token,
	int *present, struct jaro_contact_error *error)
{
	sqlite3_int64 exists = 0;
	sqlite3_stmt *statement = NULL;
	*present = 0;
	int rc = read_integer(store->db, "SELECT count(*) FROM sqlite_schema WHERE type='table'"
		" AND name='filter_import'", &exists);
	if (rc != SQLITE_OK)
		return sql_failure(error, rc, "cannot inspect filter import receipts");
	if (!exists)
		return JARO_CONTACT_OK;
	rc = sqlite3_prepare_v2(store->db, "SELECT 1 FROM filter_import WHERE token=?1", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_text(statement, 1, token, -1, SQLITE_TRANSIENT);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	*present = rc == SQLITE_ROW;
	return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? JARO_CONTACT_OK :
		sql_failure(error, rc, "cannot read filter import receipt");
}

static int
copy_text(sqlite3_stmt *statement, int column, struct jaro_contact_text *output, size_t *remaining)
{
	if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
		return SQLITE_CORRUPT;
	const unsigned char *value = sqlite3_column_text(statement, column);
	size_t length = (size_t)sqlite3_column_bytes(statement, column);
	struct jaro_contact_text text = {(const char *)value, length};
	char *copy;

	if (!value || !jaro_contact_valid_text(text) || length > *remaining)
		return SQLITE_CORRUPT;
	copy = malloc(length + 1);
	if (!copy)
		return SQLITE_NOMEM;
	memcpy(copy, value, length + 1);
	*output = (struct jaro_contact_text){copy, length};
	*remaining -= length;
	return SQLITE_OK;
}

static int
read_addresses(sqlite3 *db, int64_t id, struct jaro_contact_text *addresses,
	size_t count, size_t *remaining)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db,
		"SELECT spelling FROM addresses WHERE contact_id=?1 ORDER BY ordinal", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	for (size_t i = 0; rc == SQLITE_OK && i < count; i++) {
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW)
			rc = copy_text(statement, 0, &addresses[i], remaining);
		else if (rc == SQLITE_DONE)
			rc = SQLITE_CORRUPT;
	}
	if (rc == SQLITE_OK) {
		rc = sqlite3_step(statement);
		rc = rc == SQLITE_DONE ? SQLITE_OK : rc == SQLITE_ROW ? SQLITE_CORRUPT : rc;
	}
	sqlite3_finalize(statement);
	return rc;
}

static int
read_fields(sqlite3 *db, int64_t id, struct jaro_contact_field *fields,
	size_t count, size_t *remaining)
{
	sqlite3_stmt *statement = NULL;
	int rc = sqlite3_prepare_v2(db,
		"SELECT key,value FROM fields WHERE contact_id=?1 ORDER BY ordinal", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	for (size_t i = 0; rc == SQLITE_OK && i < count; i++) {
		rc = sqlite3_step(statement);
		if (rc == SQLITE_ROW) {
			rc = copy_text(statement, 0, &fields[i].key, remaining);
			if (rc == SQLITE_OK)
				rc = copy_text(statement, 1, &fields[i].value, remaining);
		} else if (rc == SQLITE_DONE) {
			rc = SQLITE_CORRUPT;
		}
	}
	if (rc == SQLITE_OK) {
		rc = sqlite3_step(statement);
		rc = rc == SQLITE_DONE ? SQLITE_OK : rc == SQLITE_ROW ? SQLITE_CORRUPT : rc;
	}
	sqlite3_finalize(statement);
	return rc;
}

void
jaro_contact_clear(struct jaro_contact *contact)
{
	free((void *)contact->name.data);
	free((void *)contact->nickname.data);
	free((void *)contact->notes.data);
	if (contact->addresses)
		for (size_t i = 0; i < contact->address_count; i++)
			free((void *)contact->addresses[i].data);
	if (contact->fields)
		for (size_t i = 0; i < contact->field_count; i++) {
			free((void *)contact->fields[i].key.data);
			free((void *)contact->fields[i].value.data);
		}
	free((void *)contact->addresses);
	free((void *)contact->fields);
	*contact = (struct jaro_contact){0};
}

static int
read_record(sqlite3 *db, sqlite3_stmt *statement, int64_t book,
	struct jaro_contact *contact, struct jaro_contact_error *error)
{
	*contact = (struct jaro_contact){
		.id = sqlite3_column_int64(statement, 0), .book_id = book,
		.revision = sqlite3_column_int64(statement, 1)
	};
	sqlite3_int64 addresses = sqlite3_column_int64(statement, 5);
	sqlite3_int64 fields = sqlite3_column_int64(statement, 6);
	size_t remaining = JARO_CONTACT_RECORD_MAX;
	if (addresses < 0 || addresses > JARO_CONTACT_ADDRESSES_MAX ||
			fields < 0 || fields > JARO_CONTACT_FIELDS_MAX)
		return sql_failure(error, SQLITE_CORRUPT, "invalid contact field counts");
	contact->address_count = (size_t)addresses;
	contact->field_count = (size_t)fields;
	int rc = copy_text(statement, 2, &contact->name, &remaining);
	if (rc == SQLITE_OK)
		rc = copy_text(statement, 3, &contact->nickname, &remaining);
	if (rc == SQLITE_OK)
		rc = copy_text(statement, 4, &contact->notes, &remaining);
	if (rc == SQLITE_OK && addresses) {
		struct jaro_contact_text *items = calloc(contact->address_count, sizeof *items);
		contact->addresses = items;
		rc = items ? read_addresses(db, contact->id, items, contact->address_count, &remaining) : SQLITE_NOMEM;
	}
	if (rc == SQLITE_OK && fields) {
		struct jaro_contact_field *items = calloc(contact->field_count, sizeof *items);
		contact->fields = items;
		rc = items ? read_fields(db, contact->id, items, contact->field_count, &remaining) : SQLITE_NOMEM;
	}
	if (rc == SQLITE_OK)
		return JARO_CONTACT_OK;
	jaro_contact_clear(contact);
	return sql_failure(error, rc, "cannot read contact fields");
}

#define CONTACT_READ_COLUMNS "SELECT c.id,c.revision,c.name,c.nickname,c.notes," \
	" (SELECT count(*) FROM addresses WHERE contact_id=c.id)," \
	" (SELECT count(*) FROM fields WHERE contact_id=c.id) FROM contacts c"

int
jaro_contact_get(struct jaro_contact_store *store, int64_t book, int64_t id,
	struct jaro_contact *contact, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int status;

	*contact = (struct jaro_contact){0};
	int rc = sqlite3_prepare_v2(store->db,
		CONTACT_READ_COLUMNS " WHERE c.book_id=?1 AND c.id=?2", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, book);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 2, id);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	if (rc == SQLITE_ROW)
		status = read_record(store->db, statement, book, contact, error);
	else if (rc == SQLITE_DONE)
		status = failure(error, JARO_CONTACT_MISSING, "contact does not exist", NULL, 0, 0);
	else
		status = sql_failure(error, rc, "cannot get contact");
	sqlite3_finalize(statement);
	return status;
}

int
jaro_contact_visit(struct jaro_contact_store *store, int64_t book_id,
	struct jaro_contact_text query, jaro_contact_visitor visit, void *data,
	struct jaro_contact_error *error)
{
	static const char sql[] =
		"SELECT c.id,c.revision,c.name,c.nickname,c.notes,"
		" (SELECT count(*) FROM addresses WHERE contact_id=c.id),"
		" (SELECT count(*) FROM fields WHERE contact_id=c.id),c.book_id"
		" FROM contacts c JOIN books b ON b.id=c.book_id"
		" WHERE (c.book_id=?1 OR (?1=0 AND b.name<>'block')) AND (?2='' OR instr(c.name_key,?2)>0"
		" OR instr(c.nickname_key,?2)>0 OR instr(c.notes_key,?2)>0"
		" OR EXISTS(SELECT 1 FROM addresses a WHERE a.contact_id=c.id AND instr(a.search_key,?2)>0))"
		" ORDER BY b.name='allow' DESC,c.book_id,c.ordinal";
	sqlite3 *db = store->db;
	sqlite3_stmt *statement = NULL;
	int rc, status = JARO_CONTACT_OK;
	char *key;

	if (!jaro_contact_valid_text(query))
		return failure(error, JARO_CONTACT_INVALID, "invalid search text", "query", 0, 0);
	key = abook_search_key_bytes(query.data, query.size);
	if (!key)
		return sql_failure(error, SQLITE_NOMEM, "cannot allocate search key");
	if (query.size && !*key) {
		free(key);
		return JARO_CONTACT_OK;
	}
	rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, book_id);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_text(statement, 2, key, -1, SQLITE_TRANSIENT);
	free(key);
	if (rc == SQLITE_OK) {
		while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
			struct jaro_contact contact;
			status = read_record(db, statement, sqlite3_column_int64(statement, 7), &contact, error);
			if (!status && visit(&contact, data))
				status = failure(error, JARO_CONTACT_CONSUMER, "contact visitor failed", NULL, 0, 0);
			jaro_contact_clear(&contact);
			if (status)
				break;
		}
	}
	sqlite3_finalize(statement);
	if (status)
		return status;
	return rc == SQLITE_DONE ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot query contacts");
}

int
jaro_contact_visit_keys(struct jaro_contact_store *store, struct jaro_contact_text book,
	jaro_contact_key_visitor visit, void *data, struct jaro_contact_error *error)
{
	sqlite3_stmt *statement = NULL;
	int64_t id;
	int status = jaro_contact_book(store, book, 0, &id, error);
	if (status == JARO_CONTACT_MISSING)
		return JARO_CONTACT_OK;
	if (status)
		return status;
	int rc = sqlite3_prepare_v2(store->db,
		"SELECT DISTINCT mailbox_key FROM addresses INDEXED BY addresses_book_mailbox"
		" WHERE book_id=?1 ORDER BY mailbox_key", -1, &statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int64(statement, 1, id);
	if (rc == SQLITE_OK)
		while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
			struct jaro_contact_text key = {
				(const char *)sqlite3_column_text(statement, 0),
				(size_t)sqlite3_column_bytes(statement, 0)
			};
			char *canonical;
			int valid = !jaro_contact_mailbox_key(key, &canonical);
			if (!valid || strcmp(key.data, canonical))
				status = sql_failure(error, SQLITE_CORRUPT, "invalid stored mailbox key");
			free(canonical);
			if (status)
				break;
			if (visit(key, data)) {
				status = failure(error, JARO_CONTACT_CONSUMER, "mailbox visitor failed", NULL, 0, 0);
				break;
			}
		}
	sqlite3_finalize(statement);
	if (status)
		return status;
	return rc == SQLITE_DONE ? JARO_CONTACT_OK : sql_failure(error, rc, "cannot read contact mailbox keys");
}
