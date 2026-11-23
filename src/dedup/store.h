#ifndef JARO_DEDUP_STORE_H
#define JARO_DEDUP_STORE_H

#include "inventory.h"
#include "sqlite3.h"

struct jd_store {
	sqlite3 *db;
	sqlite3_stmt *insert;
	char *directory;
	sqlite3_int64 rows, batches, old_heap_limit;
	int heap_set, inventory_conflict;
#ifdef JARO_DEDUP_TESTING
	int fail_allocation, force_digest;
#endif
};

#ifdef __APPLE__
#define JD_MTIME st_mtimespec
#define JD_CTIME st_ctimespec
#else
#define JD_MTIME st_mtim
#define JD_CTIME st_ctim
#endif

/* SELECT columns for decoding a row of occurrences with alias o. */
#define JD_OCCURRENCE_COLUMNS "o.rank,o.subdir,o.name,o.size,o.device,o.inode," \
    "o.mtime_sec,o.mtime_nsec,o.ctime_sec,o.ctime_nsec,o.id,o.nlink,o.mode"
#define JD_OCCURRENCE_ID_COLUMN 10

struct jd_occurrence {
	const struct jd_root *root;
	const unsigned char *name; /* borrowed from the active SQLite row */
	size_t length;
	struct stat metadata;
	sqlite3_int64 id;
	unsigned subdir;
};

int jd_store_open(struct jd_store *, const char *, unsigned, unsigned);
int jd_store_inventory(struct jd_store *, const struct jd_inventory *);
int jd_store_require_inventory(struct jd_store *);
int jd_store_read_occurrence(struct jd_store *, sqlite3_stmt *,
    const struct jd_inventory *, struct jd_occurrence *);
int jd_store_error(struct jd_store *, const char *, int);
int jd_store_exec(struct jd_store *, const char *);
int jd_store_prepare(struct jd_store *, sqlite3_stmt **, const char *);
/* Callback cursor, ordered by size then stable priority; does not load a bucket. */
int jd_store_cursor(struct jd_store *, jd_emit, void *, const struct jd_inventory *);
int jd_store_close(struct jd_store *);

#endif
