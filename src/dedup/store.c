#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "store.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JD_BATCH_ROWS 1024

int
jd_store_error(struct jd_store *store, const char *operation, int code)
{
	fprintf(stderr, "dedup scratch: %s: %s (%d)\n", operation,
	    store->db ? sqlite3_errmsg(store->db) : sqlite3_errstr(code), code);
	return -1;
}

int
jd_store_exec(struct jd_store *store, const char *command)
{
	int code = sqlite3_exec(store->db, command, NULL, NULL, NULL);
	return code == SQLITE_OK ? 0 : jd_store_error(store, command, code);
}

static int
scalar(struct jd_store *store, const char *query, sqlite3_int64 expected)
{
	sqlite3_stmt *statement = NULL;
	int code = sqlite3_prepare_v2(store->db, query, -1, &statement, NULL);
	if (code == SQLITE_OK)
		code = sqlite3_step(statement);
	if (code != SQLITE_ROW || sqlite3_column_int64(statement, 0) != expected) {
		sqlite3_finalize(statement);
		return jd_store_error(store, query, code);
	}
	code = sqlite3_finalize(statement);
	return code == SQLITE_OK ? 0 : jd_store_error(store, query, code);
}

int
jd_store_open(struct jd_store *store, const char *parent, unsigned cache_kib,
    unsigned heap_kib)
{
	char *resolved = NULL, *path = NULL;
	char settings[256];
	int fd, code;
	memset(store, 0, sizeof(*store));
	if (!cache_kib || heap_kib < cache_kib || heap_kib > 786432) {
		fprintf(stderr, "dedup scratch: invalid memory bounds\n");
		return -1;
	}
	/* One helper process owns this connection and SQLite's global temp path. */
	if (sqlite3_temp_directory) {
		fprintf(stderr, "dedup scratch: SQLite temporary directory already owned\n");
		return -1;
	}
	resolved = realpath(parent, NULL);
	if (!resolved)
		goto system_error;
	store->directory = malloc(strlen(resolved) + sizeof("/jaro-dedup-XXXXXX"));
	if (!store->directory)
		goto system_error;
	sprintf(store->directory, "%s/jaro-dedup-XXXXXX", resolved);
	free(resolved);
	resolved = NULL;
	if (!mkdtemp(store->directory)) {
		free(store->directory);
		store->directory = NULL;
		goto system_error;
	}
	store->old_heap_limit = sqlite3_hard_heap_limit64((sqlite3_int64)heap_kib * 1024);
	store->heap_set = 1;
	if (store->old_heap_limit < 0 ||
	    sqlite3_hard_heap_limit64(-1) != (sqlite3_int64)heap_kib * 1024) {
		fprintf(stderr, "dedup scratch: cannot enforce SQLite hard heap bound\n");
		goto error;
	}
	sqlite3_temp_directory = sqlite3_mprintf("%s", store->directory);
	path = sqlite3_mprintf("%s/index.sqlite", store->directory);
	if (!sqlite3_temp_directory || !path) {
		jd_store_error(store, "allocate scratch paths", SQLITE_NOMEM);
		goto error;
	}
	fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto system_error;
	if (close(fd) < 0)
		goto system_error;
	code = sqlite3_open_v2(path, &store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
	sqlite3_free(path);
	path = NULL;
	if (code != SQLITE_OK) {
		jd_store_error(store, "open scratch database", code);
		goto error;
	}
	snprintf(settings, sizeof(settings), "PRAGMA page_size=4096; PRAGMA cache_size=-%u; PRAGMA temp.cache_size=-%u;"
	    "PRAGMA mmap_size=0; PRAGMA temp_store=FILE; PRAGMA synchronous=OFF;"
	    "PRAGMA journal_mode=DELETE;", cache_kib, cache_kib > 8192 ? 8192 : cache_kib);
	if (jd_store_exec(store, settings) || scalar(store, "PRAGMA cache_size", -(sqlite3_int64)cache_kib) ||
	    scalar(store, "PRAGMA mmap_size", 0) || scalar(store, "PRAGMA temp_store", 1) ||
	    scalar(store, "PRAGMA temp.cache_size", -(sqlite3_int64)(cache_kib > 8192 ? 8192 : cache_kib)) ||
	    scalar(store, "PRAGMA page_size", 4096) ||
	    jd_store_exec(store, "CREATE TABLE roots(rank INTEGER PRIMARY KEY, path BLOB NOT NULL);"
	    "CREATE TABLE occurrences(id INTEGER PRIMARY KEY, rank INTEGER NOT NULL,"
	    "subdir INTEGER NOT NULL, name BLOB NOT NULL, size INTEGER NOT NULL,"
	    "device TEXT NOT NULL, inode TEXT NOT NULL, mtime_sec INTEGER NOT NULL,"
	    "mtime_nsec INTEGER NOT NULL, ctime_sec INTEGER NOT NULL, ctime_nsec INTEGER NOT NULL);"
	    "CREATE TABLE status(complete INTEGER NOT NULL); INSERT INTO status VALUES(0);"))
		goto error;
	return 0;
system_error:
	fprintf(stderr, "dedup scratch: %s\n", strerror(errno));
error:
	free(resolved);
	sqlite3_free(path);
	jd_store_close(store);
	return -1;
}

static int
insert_occurrence(void *context, const struct jd_root *root, unsigned subdir,
    const unsigned char *name, size_t length, const struct stat *metadata)
{
	struct jd_store *store = context;
	int code = SQLITE_OK;
	char device[32], inode[32];
	struct timespec modified = metadata->JD_MTIME, changed = metadata->JD_CTIME;
#ifdef JARO_DEDUP_TESTING
	if (store->fail_allocation) {
		/* Force a real SQLite allocation refusal, not a simulated return code. */
		sqlite3_hard_heap_limit64(sqlite3_memory_used() + 1);
		store->fail_allocation = 0;
	}
#endif
	snprintf(device, sizeof(device), "%ju", (uintmax_t)metadata->st_dev);
	snprintf(inode, sizeof(inode), "%ju", (uintmax_t)metadata->st_ino);
#define BIND(call) do { if (code == SQLITE_OK) code = (call); } while (0)
	BIND(sqlite3_bind_int64(store->insert, 1, (sqlite3_int64)root->rank));
	BIND(sqlite3_bind_int(store->insert, 2, (int)subdir));
	BIND(sqlite3_bind_blob64(store->insert, 3, name, length, SQLITE_TRANSIENT));
	BIND(sqlite3_bind_int64(store->insert, 4, metadata->st_size));
	BIND(sqlite3_bind_text(store->insert, 5, device, -1, SQLITE_TRANSIENT));
	BIND(sqlite3_bind_text(store->insert, 6, inode, -1, SQLITE_TRANSIENT));
	BIND(sqlite3_bind_int64(store->insert, 7, modified.tv_sec));
	BIND(sqlite3_bind_int64(store->insert, 8, modified.tv_nsec));
	BIND(sqlite3_bind_int64(store->insert, 9, changed.tv_sec));
	BIND(sqlite3_bind_int64(store->insert, 10, changed.tv_nsec));
#undef BIND
	if (code == SQLITE_OK)
		code = sqlite3_step(store->insert);
	if (code != SQLITE_DONE)
		return jd_store_error(store, "insert occurrence", code);
	if (sqlite3_reset(store->insert) != SQLITE_OK || sqlite3_clear_bindings(store->insert) != SQLITE_OK)
		return jd_store_error(store, "reset occurrence insert", sqlite3_errcode(store->db));
	store->rows++;
	if (store->rows % JD_BATCH_ROWS == 0) {
		if (jd_store_exec(store, "COMMIT; BEGIN"))
			return -1;
		store->batches++;
	}
	return 0;
}

int
jd_store_inventory(struct jd_store *store, const struct jd_inventory *inventory)
{
	size_t i;
	sqlite3_stmt *roots = NULL;
	int code = sqlite3_prepare_v2(store->db, "INSERT INTO roots VALUES(?,?)", -1, &roots, NULL);
	if (code != SQLITE_OK)
		return jd_store_error(store, "prepare roots", code);
	for (i = 0; i < inventory->count; i++) {
		const struct jd_root *root = &inventory->roots[i];
		if (root->alias)
			continue;
		if (root->rank > INT64_MAX ||
		    sqlite3_bind_int64(roots, 1, (sqlite3_int64)root->rank) != SQLITE_OK ||
		    sqlite3_bind_blob64(roots, 2, root->path, strlen(root->path), SQLITE_TRANSIENT) != SQLITE_OK ||
		    sqlite3_step(roots) != SQLITE_DONE || sqlite3_reset(roots) != SQLITE_OK) {
			sqlite3_finalize(roots);
			return jd_store_error(store, "insert root", sqlite3_errcode(store->db));
		}
	}
	if (sqlite3_finalize(roots) != SQLITE_OK)
		return jd_store_error(store, "finalize roots", sqlite3_errcode(store->db));
	code = sqlite3_prepare_v2(store->db, "INSERT INTO occurrences(rank,subdir,name,size,device,inode,"
	    "mtime_sec,mtime_nsec,ctime_sec,ctime_nsec) VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &store->insert, NULL);
	if (code != SQLITE_OK)
		return jd_store_error(store, "prepare occurrences", code);
	if (jd_store_exec(store, "BEGIN") || jd_scan(inventory, insert_occurrence, store))
		return -1;
	code = sqlite3_finalize(store->insert);
	store->insert = NULL;
	if (code != SQLITE_OK)
		return jd_store_error(store, "finalize occurrences", code);
	if (jd_store_exec(store, "COMMIT; CREATE INDEX occurrence_size ON occurrences(size,rank,subdir,name);"
	    "UPDATE status SET complete=1"))
		return -1;
	if (store->rows % JD_BATCH_ROWS)
		store->batches++;
	return 0;
}

int
jd_store_read_occurrence(struct jd_store *store, sqlite3_stmt *cursor,
    const struct jd_inventory *inventory, struct jd_occurrence *entry)
{
	sqlite3_int64 rank = sqlite3_column_int64(cursor, 0);
	int subdir = sqlite3_column_int(cursor, 1);
	const unsigned char *device = sqlite3_column_text(cursor, 4);
	const unsigned char *inode = sqlite3_column_text(cursor, 5);
	struct stat metadata = {0};

	entry->name = sqlite3_column_blob(cursor, 2);
	entry->length = (size_t)sqlite3_column_bytes(cursor, 2);
	if (!device || !inode || !entry->name)
		return jd_store_error(store, "read occurrence fields", SQLITE_NOMEM);
	if (rank < 0 || (uintmax_t)rank >= inventory->count ||
	    inventory->roots[rank].alias || subdir < 0 || subdir > 1)
		return jd_store_error(store, "invalid occurrence binding", SQLITE_CORRUPT);
	entry->root = &inventory->roots[rank];
	entry->subdir = (unsigned)subdir;
	entry->id = sqlite3_column_int64(cursor, JD_OCCURRENCE_ID_COLUMN);
	metadata.st_mode = S_IFREG;
	metadata.st_size = (off_t)sqlite3_column_int64(cursor, 3);
	metadata.st_dev = (dev_t)strtoumax((const char *)device, NULL, 10);
	metadata.st_ino = (ino_t)strtoumax((const char *)inode, NULL, 10);
	metadata.JD_MTIME.tv_sec = (time_t)sqlite3_column_int64(cursor, 6);
	metadata.JD_MTIME.tv_nsec = (long)sqlite3_column_int64(cursor, 7);
	metadata.JD_CTIME.tv_sec = (time_t)sqlite3_column_int64(cursor, 8);
	metadata.JD_CTIME.tv_nsec = (long)sqlite3_column_int64(cursor, 9);
	entry->metadata = metadata;
	return 0;
}

int
jd_store_require_inventory(struct jd_store *store)
{
	return scalar(store, "SELECT complete FROM status", 1);
}

int
jd_store_cursor(struct jd_store *store, jd_emit emit, void *context,
    const struct jd_inventory *inventory)
{
	sqlite3_stmt *cursor = NULL;
	int code;
	if (jd_store_require_inventory(store))
		return -1;
	code = sqlite3_prepare_v2(store->db, "SELECT " JD_OCCURRENCE_COLUMNS " FROM occurrences AS o "
	    "INDEXED BY occurrence_size ORDER BY size,rank,subdir,name", -1, &cursor, NULL);
	if (code != SQLITE_OK)
		return jd_store_error(store, "prepare inventory cursor", code);
	while ((code = sqlite3_step(cursor)) == SQLITE_ROW) {
		struct jd_occurrence entry;
		if (jd_store_read_occurrence(store, cursor, inventory, &entry) ||
		    emit(context, entry.root, entry.subdir, entry.name, entry.length, &entry.metadata)) {
			sqlite3_finalize(cursor);
			return -1;
		}
	}
	sqlite3_finalize(cursor);
	return code == SQLITE_DONE ? 0 : jd_store_error(store, "read inventory cursor", code);
}

int
jd_store_close(struct jd_store *store)
{
	int result = 0;
	sqlite3_finalize(store->insert);
	store->insert = NULL;
	if (store->db && sqlite3_close(store->db) != SQLITE_OK) {
		fprintf(stderr, "dedup scratch: database close failed\n");
		result = -1;
	}
	store->db = NULL;
	if (store->heap_set) {
		sqlite3_free(sqlite3_temp_directory);
		sqlite3_temp_directory = NULL;
		sqlite3_hard_heap_limit64(store->old_heap_limit);
		store->heap_set = 0;
	}
	if (store->directory) {
		char *path = malloc(strlen(store->directory) + sizeof("/index.sqlite-journal"));
		if (!path) {
			result = -1;
		} else {
			/* Only this invocation's known scratch files; never message paths. */
			const char *files[] = {"index.sqlite", "index.sqlite-journal"};
			size_t i;
			for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
				sprintf(path, "%s/%s", store->directory, files[i]);
				if (unlink(path) && errno != ENOENT)
					result = -1;
			}
			free(path);
			if (rmdir(store->directory))
				result = -1;
		}
		if (result)
			fprintf(stderr, "dedup scratch: cleanup incomplete: %s\n", store->directory);
		free(store->directory);
		store->directory = NULL;
	}
	return result;
}
