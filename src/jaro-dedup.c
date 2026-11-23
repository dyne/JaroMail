#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

/* Private read-only inventory entry point. Public/apply CLI belongs to L3. */
#include "dedup/store.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JARO_DEDUP_TESTING
static int
dump(void *context, const struct jd_root *root, unsigned sub,
    const unsigned char *name, size_t length, const struct stat *metadata)
{
	(void)context;
#ifdef __APPLE__
	struct timespec modified = metadata->st_mtimespec, changed = metadata->st_ctimespec;
#else
	struct timespec modified = metadata->st_mtim, changed = metadata->st_ctim;
#endif
	if (printf("%zu%c%u%c", root->rank, 0, sub, 0) < 0 ||
	    fwrite(name, 1, length, stdout) != length || putchar(0) == EOF ||
	    printf("%jd%c%ju%c%ju%c%jd%c%ld%c%jd%c%ld%c",
	    (intmax_t)metadata->st_size, 0, (uintmax_t)metadata->st_dev, 0,
	    (uintmax_t)metadata->st_ino, 0, (intmax_t)modified.tv_sec, 0,
	    modified.tv_nsec, 0, (intmax_t)changed.tv_sec, 0, changed.tv_nsec, 0) < 0)
		return -1;
	return 0;
}

static unsigned
number(const char *text)
{
	char *end;
	unsigned long value = strtoul(text, &end, 10);
	if (!*text || *end || value > 786432 || !value)
		return 0;
	return (unsigned)value;
}
#endif

int
main(int argc, char **argv)
{
	struct jd_inventory inventory;
	struct jd_store store;
	const char *scratch = getenv("TMPDIR"), *mail_root = getenv("JAROMAILDIR");
	unsigned cache_kib = 131072, heap_kib = 393216;
	int offset = 1, inventory_mode = 0, result = 1;
#ifdef JARO_DEDUP_TESTING
	int hold = 0, dump_rows = 0, allocation_failure = 0, temp_probe = 0;
	unsigned max_pages = 0;
#endif
	if (!scratch || !*scratch)
		scratch = "/tmp";
	while (offset < argc) {
		const char *option = argv[offset++];
		if (!strcmp(option, "--"))
			break;
		if (!strcmp(option, "--inventory")) {
			inventory_mode = 1;
			continue;
		}
		if ((!strcmp(option, "--scratch-dir") || !strcmp(option, "--mail-root")) && offset < argc) {
			if (!strcmp(option, "--scratch-dir"))
				scratch = argv[offset++];
			else
				mail_root = argv[offset++];
			continue;
		}
#ifdef JARO_DEDUP_TESTING
		if (!strcmp(option, "--test-hold")) { hold = 1; continue; }
		if (!strcmp(option, "--test-dump")) { dump_rows = 1; continue; }
		if (!strcmp(option, "--test-temp-probe")) { temp_probe = 1; continue; }
		if (!strcmp(option, "--test-allocation-failure")) { allocation_failure = 1; continue; }
		if (offset < argc && (!strcmp(option, "--test-cache-kib") ||
		    !strcmp(option, "--test-heap-kib") || !strcmp(option, "--test-max-pages"))) {
			unsigned value = number(argv[offset++]);
			if (!value)
				goto usage;
			if (!strcmp(option, "--test-cache-kib")) cache_kib = value;
			else if (!strcmp(option, "--test-heap-kib")) heap_kib = value;
			else max_pages = value;
			continue;
		}
#endif
		if (*option == '-')
			goto usage;
		offset--;
		break;
	}
	if (!inventory_mode || offset == argc)
		goto usage;
	if (mail_root && !*mail_root)
		mail_root = NULL;
	if (jd_preflight(&inventory, mail_root, (size_t)(argc - offset), argv + offset))
		return 1;
	if (jd_store_open(&store, scratch, cache_kib, heap_kib)) {
		jd_inventory_free(&inventory);
		return 1;
	}
#ifdef JARO_DEDUP_TESTING
	store.fail_allocation = allocation_failure;
	if (max_pages) {
		char pragma[80];
		snprintf(pragma, sizeof(pragma), "PRAGMA max_page_count=%u", max_pages);
		if (sqlite3_exec(store.db, pragma, NULL, NULL, NULL) != SQLITE_OK)
			goto cleanup;
	}
#endif
	if (jd_store_inventory(&store, &inventory))
		goto cleanup;
#ifdef JARO_DEDUP_TESTING
	if (temp_probe && sqlite3_exec(store.db, "CREATE TEMP TABLE probe AS SELECT * FROM occurrences;"
	    "CREATE INDEX temp.probe_size ON probe(size,name)", NULL, NULL, NULL) != SQLITE_OK)
		goto cleanup;
	if (hold) {
		fprintf(stderr, "scratch-ready: %s\n", store.directory);
		fflush(stderr);
		if (getchar() != 'x')
			goto cleanup;
	}
	if (dump_rows && (jd_store_cursor(&store, dump, NULL, &inventory) || fflush(stdout)))
		goto cleanup;
#endif
	result = 0;
cleanup:
	{
		sqlite3_int64 rows = store.rows, batches = store.batches, peak = sqlite3_memory_highwater(0);
		if (jd_store_close(&store))
			result = 1;
		jd_inventory_free(&inventory);
		if (!result)
			fprintf(stderr, "inventory complete: rows=%" PRId64 " batches=%" PRId64
			    " sqlite-peak-bytes=%" PRId64 "\n", (int64_t)rows, (int64_t)batches, (int64_t)peak);
	}
	return result;
usage:
	fputs("private inventory usage: jaro-dedup --inventory [--scratch-dir PATH] [--mail-root PATH] -- ROOT...\n", stderr);
	return 2;
}
