#ifndef JARO_DEDUP_STORE_H
#define JARO_DEDUP_STORE_H

#include "inventory.h"
#include "sqlite3.h"

struct jd_store {
	sqlite3 *db;
	sqlite3_stmt *insert;
	char *directory;
	sqlite3_int64 rows, batches, old_heap_limit;
	int heap_set;
#ifdef JARO_DEDUP_TESTING
	int fail_allocation;
#endif
};

int jd_store_open(struct jd_store *, const char *, unsigned, unsigned);
int jd_store_inventory(struct jd_store *, const struct jd_inventory *);
/* Callback cursor, ordered by size then stable priority; does not load a bucket. */
int jd_store_cursor(struct jd_store *, jd_emit, void *, const struct jd_inventory *);
int jd_store_close(struct jd_store *);

#endif
