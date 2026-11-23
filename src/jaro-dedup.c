#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

/* Private native helper; the shell caller selects the operation. */
#include "dedup/apply.h"
#include "dedup/profile.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(JARO_DEDUP_PROFILE) && defined(JARO_DEDUP_TESTING)
uint64_t jd_elapsed[JD_TIMERS], jd_message_opens;

static void
print_profile(void)
{
	uint64_t scan = jd_elapsed[JD_SCAN] - jd_elapsed[JD_INSERT];
	fprintf(stderr, "dedup profile: scan-ns=%" PRIu64 " index-ns=%" PRIu64
	    " sample-ns=%" PRIu64 " hash-ns=%" PRIu64 " compare-ns=%" PRIu64
	    " unlink-ns=%" PRIu64 " message-opens=%" PRIu64 " scratch-peak-bytes=%" PRIu64 "\n", scan,
	    jd_elapsed[JD_INVENTORY] - scan, jd_elapsed[JD_SAMPLE], jd_elapsed[JD_HASH],
	    jd_elapsed[JD_COMPARE], jd_elapsed[JD_UNLINK], jd_message_opens, jd_scratch_peak);
}
#endif

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
	struct jd_store store = {0};
	struct jd_locks locks = {0};
	struct jd_classify_stats stats = {0};
	struct jd_apply_stats applied = {0};
	const char *scratch = getenv("TMPDIR"), *mail_root = getenv("JAROMAILDIR");
	unsigned cache_kib = 131072, heap_kib = 393216;
	int offset = 1, inventory_mode = 0, apply = 0, quiet = 0, result = 1;
#ifdef JARO_DEDUP_TESTING
	int hold = 0, dump_rows = 0, candidates = 0, classify = 0;
	int allocation_failure = 0, temp_probe = 0, force_digest = 0, hold_reduction = 0;
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
		if (!strcmp(option, "--apply")) {
			apply = 1;
			continue;
		}
		if (!strcmp(option, "--preview")) {
			applied.preview = 1;
			continue;
		}
		if (!strcmp(option, "--quiet")) {
			quiet = 1;
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
		if (!strcmp(option, "--test-candidates")) { candidates = 1; continue; }
		if (!strcmp(option, "--test-classify")) { classify = 1; continue; }
		if (!strcmp(option, "--test-force-digest")) { force_digest = 1; continue; }
		if (!strcmp(option, "--test-fail-links")) { applied.fail_links = 1; continue; }
		if (!strcmp(option, "--test-apply-hold") && offset < argc) {
			applied.hold_at = argv[offset++];
			continue;
		}
		if (!strcmp(option, "--test-hold-after-reduction")) { hold_reduction = 1; continue; }
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
	if (inventory_mode + apply + applied.preview != 1 || offset == argc)
		goto usage;
#ifdef JARO_DEDUP_TESTING
	if (dump_rows + candidates + classify + apply + applied.preview > 1 ||
	    ((force_digest || hold_reduction) && !candidates && !classify && !apply) ||
	    ((applied.hold_at || applied.fail_links) && !apply))
		goto usage;
#endif
	if (mail_root && !*mail_root)
		mail_root = NULL;
	if (jd_preflight(&inventory, mail_root, (size_t)(argc - offset), argv + offset))
		goto cleanup;
	if ((apply || applied.preview) && jd_check_scratch_parent(&inventory, scratch))
		goto cleanup;
	if (applied.preview && signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("dedup preview: configure report pipe");
		goto cleanup;
	}
	if ((apply && jd_lock_inventory(&inventory, &locks)) ||
	    jd_store_open(&store, scratch, cache_kib, heap_kib))
		goto cleanup;
#ifdef JARO_DEDUP_TESTING
	store.fail_allocation = allocation_failure;
	store.force_digest = force_digest;
	if (max_pages) {
		char pragma[80];
		snprintf(pragma, sizeof(pragma), "PRAGMA max_page_count=%u", max_pages);
		if (sqlite3_exec(store.db, pragma, NULL, NULL, NULL) != SQLITE_OK)
			goto cleanup;
	}
#endif
	JD_START(inventory_start);
	if (jd_store_inventory(&store, &inventory))
		goto cleanup;
	JD_END(inventory_start, JD_INVENTORY);
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
	if (apply || applied.preview
#ifdef JARO_DEDUP_TESTING
	    || candidates || classify
#endif
	    ) {
		if (jd_reduce_candidates(&store, &inventory, &stats))
			goto cleanup;
#ifdef JARO_DEDUP_TESTING
		if (hold_reduction) {
			fputs("candidates-ready\n", stderr);
			if (fflush(stderr) || getchar() != 'x')
				goto cleanup;
		}
#endif
		if (apply || applied.preview) {
			if (jd_prove_hashes(&store, &inventory, &stats) ||
			    jd_apply_matches(&store, &inventory, &stats, &applied))
				goto cleanup;
			if (applied.preview && fflush(stdout)) {
				perror("dedup preview: flush report");
				goto cleanup;
			}
		}
#ifdef JARO_DEDUP_TESTING
		else if (jd_prove_pairs(&store, &inventory, &stats) ||
		    (classify ? (jd_prove_hashes(&store, &inventory, &stats) || jd_dump_survivors(&store)) :
		    jd_dump_candidates(&store)))
			goto cleanup;
		if (fflush(stdout))
			goto cleanup;
		jd_print_classify_stats(&stats);
#endif
	}
	result = 0;
cleanup:
	{
		sqlite3_int64 rows = store.rows, batches = store.batches, peak = sqlite3_memory_highwater(0);
		if (jd_store_close(&store))
			result = 1;
#if defined(JARO_DEDUP_PROFILE) && defined(JARO_DEDUP_TESTING)
		if (!result)
			print_profile();
#endif
		jd_unlock_inventory(&locks);
		jd_inventory_free(&inventory);
		if (apply || applied.preview) {
			applied.conflicts += store.inventory_conflict + stats.pair.conflicts +
			    stats.proof.conflicts + stats.sample.conflicts + stats.hash.conflicts;
			if (applied.preview && (!quiet || result))
				fprintf(stderr, "dedup preview: examined=%" PRId64 " groups=%ju proposed=%ju"
				    " removed=0 conflicts=%ju logical-bytes=%ju\n", (int64_t)rows,
				    applied.groups, applied.proposed, applied.conflicts, applied.logical_bytes);
			else if (apply && (!quiet || result))
				fprintf(stderr, "dedup apply: examined=%" PRId64 " groups=%ju removed=%ju"
			    " conflicts=%ju logical-bytes=%ju last-link-block-bytes=%ju\n",
			    (int64_t)rows, applied.groups, applied.removed, applied.conflicts,
			    applied.logical_bytes, applied.last_link_bytes);
		} else if (!result)
			fprintf(stderr, "inventory complete: rows=%" PRId64 " batches=%" PRId64
			    " sqlite-peak-bytes=%" PRId64 "\n", (int64_t)rows, (int64_t)batches, (int64_t)peak);
	}
	return result;
usage:
	fputs("private usage: jaro-dedup (--inventory|--preview|--apply) [--quiet] [--scratch-dir PATH] [--mail-root PATH] -- ROOT...\n", stderr);
	return 2;
}
