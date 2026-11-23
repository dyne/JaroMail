#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "apply.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct application {
	struct jd_store *store;
	struct jd_apply_stats *stats;
	struct jd_classify_stats *classified;
	sqlite3_stmt *lookup, *save;
	sqlite3_int64 previous_keeper;
	int changed_links;
};

static volatile sig_atomic_t interrupted;

static void
stop_apply(int number)
{
	interrupted = number;
}

#ifdef JARO_DEDUP_TESTING
static int
gate(struct jd_apply_stats *stats, const char *stage)
{
	if (stats->hold_at && !strcmp(stats->hold_at, stage)) {
		stats->hold_at = NULL;
		fprintf(stderr, "%s\n", stage);
		if (fflush(stderr) || getchar() != 'x')
			return -1;
	}
	return 0;
}
#else
#define gate(stats, stage) 0
#endif

/* An inventoried inode may have several names including the keeper. Only
 * relax its original snapshot for a link change we recorded on its open fd. */
static int
adjust_snapshot(struct application *app, struct jd_occurrence *entry)
{
	int code;
	if (!app->changed_links || entry->metadata.st_nlink == 1)
		return 0;
	code = sqlite3_bind_int64(app->lookup, 1, entry->id);
	if (code == SQLITE_OK)
		code = sqlite3_step(app->lookup);
	if (code == SQLITE_ROW) {
		entry->metadata.JD_CTIME.tv_sec = (time_t)sqlite3_column_int64(app->lookup, 0);
		entry->metadata.JD_CTIME.tv_nsec = (long)sqlite3_column_int64(app->lookup, 1);
		entry->metadata.st_nlink = (nlink_t)sqlite3_column_int64(app->lookup, 2);
	} else if (code != SQLITE_DONE) {
		return jd_store_error(app->store, "read own link change", code);
	}
	code = sqlite3_reset(app->lookup);
	return code == SQLITE_OK ? 0 : jd_store_error(app->store, "reset link lookup", code);
}

static int
record_link_change(struct application *app, const struct jd_message *loser, const struct stat *before)
{
	struct stat after;
	int code;
	if (fstat(loser->fd, &after) < 0) {
		perror("dedup apply: stat unlinked inode");
		return -1;
	}
	if (after.st_nlink != before->st_nlink - 1 || after.st_mode != before->st_mode ||
	    after.st_size != before->st_size || after.JD_MTIME.tv_sec != before->JD_MTIME.tv_sec ||
	    after.JD_MTIME.tv_nsec != before->JD_MTIME.tv_nsec) {
		fputs("dedup apply: inode changed during unlink\n", stderr);
		app->stats->conflicts++;
		return -1;
	}
	code = sqlite3_bind_int64(app->save, 1, after.JD_CTIME.tv_sec);
	if (code == SQLITE_OK)
		code = sqlite3_bind_int64(app->save, 2, after.JD_CTIME.tv_nsec);
	if (code == SQLITE_OK)
		code = sqlite3_bind_int64(app->save, 3, (sqlite3_int64)after.st_nlink);
	if (code == SQLITE_OK)
		code = sqlite3_bind_int64(app->save, 4, loser->entry->id);
	if (code == SQLITE_OK)
		code = sqlite3_step(app->save);
	if (code == SQLITE_DONE)
		code = sqlite3_reset(app->save);
	if (code != SQLITE_OK)
		return jd_store_error(app->store, "record own link change", code);
	app->changed_links = 1;
	return 0;
}

static int
apply_pair(struct application *app, const struct jd_occurrence *winner,
    const struct jd_occurrence *duplicate, int proved)
{
	struct jd_apply_stats *stats = app->stats;
	struct jd_occurrence a = *winner, b = *duplicate;
	struct jd_message keeper, loser;
	struct jd_read_stats validation = {0}, *reads = proved ? &validation : &app->classified->pair;
	struct stat before;
	int result = -1, conflict = 0;
	if (interrupted)
		return -1;
	if (adjust_snapshot(app, &a) || adjust_snapshot(app, &b))
		return -1;
	if (jd_message_open(&keeper, &a, reads)) {
		conflict = jd_entry_conflict(errno);
		goto done;
	}
	if (jd_message_open(&loser, &b, reads)) {
		conflict = jd_entry_conflict(errno);
		goto close_keeper;
	}
	if (fstat(loser.fd, &before) < 0) {
		perror("dedup apply: stat duplicate");
		goto close_loser;
	}
	if (gate(stats, "names-pinned"))
		goto close_loser;
	if (!proved) {
		int equal = jd_compare_opened(&keeper, &loser, reads);
		if (equal < 0) {
			conflict = jd_entry_conflict(errno);
			goto close_loser;
		}
		app->classified->compared_pairs++;
		if (!equal) {
			result = 0;
			goto close_loser;
		}
		app->classified->matching_pairs++;
	}
	if (winner->id != app->previous_keeper) {
		stats->groups++;
		app->previous_keeper = winner->id;
	}
	if (gate(stats, "before-unlink"))
		goto close_loser;
	/* Each byte proof refers to these exact snapshots. Directory
	 * locks serialize cooperating runs; external movers must be quiescent
	 * through these last name checks and the following unlinkat. */
	if (jd_message_verify(&keeper) || jd_message_verify(&loser)) {
		conflict = jd_entry_conflict(errno);
		goto close_loser;
	}
	if (interrupted)
		goto close_loser;
	if (stats->preview) {
		if (fputs("keep ", stdout) == EOF || jd_write_path(stdout, winner) ||
		    fputs(" remove ", stdout) == EOF || jd_write_path(stdout, duplicate) ||
		    putchar('\n') == EOF) {
			perror("dedup preview: write report");
			goto close_loser;
		}
		stats->proposed++;
		stats->logical_bytes += (uintmax_t)b.metadata.st_size;
		result = 0;
		goto close_loser;
	}
	if (unlinkat(loser.dir, loser.name, 0) < 0) {
		conflict = jd_entry_conflict(errno);
		perror("dedup apply: unlink duplicate");
		goto close_loser;
	}
	/* Account immediately: a later stat, scratch write or signal cannot
	 * undo this unlink. Last-link blocks exclude links retained elsewhere. */
	stats->removed++;
	stats->logical_bytes += (uintmax_t)b.metadata.st_size;
	if (before.st_nlink == 1)
		stats->last_link_bytes += (uintmax_t)before.st_blocks * 512;
	else if (record_link_change(app, &loser, &before))
		goto close_loser;
	result = gate(stats, "name-removed");
close_loser:
	jd_message_close(&loser);
close_keeper:
	jd_message_close(&keeper);
done:
	if (conflict) {
		fputs("dedup apply: conflict, kept available names ", stderr);
		jd_write_path(stderr, winner);
		fputc(' ', stderr);
		jd_write_path(stderr, duplicate);
		fputc('\n', stderr);
		stats->conflicts++;
		return 0; /* A different pair may still be safely removable. */
	}
	return result;
}

static int
apply_provisional(void *context, const struct jd_occurrence *a, const struct jd_occurrence *b)
{
	return apply_pair(context, a, b, 0);
}

static int
apply_proved(void *context, const struct jd_occurrence *a, const struct jd_occurrence *b)
{
	return apply_pair(context, a, b, 1);
}

int
jd_apply_matches(struct jd_store *store, const struct jd_inventory *inventory,
    struct jd_classify_stats *classified, struct jd_apply_stats *stats)
{
	struct application app = {.store = store, .stats = stats, .classified = classified};
	struct sigaction action = {.sa_handler = stop_apply}, old_int, old_term;
	int result = -1;
	/* Key updates against the original ctime/link count as well as inode
	 * identity. Never change the occurrences being streamed by a cursor. */
	if (!stats->preview && (jd_store_exec(store, "CREATE TEMP TABLE link_changes(device TEXT,inode TEXT,"
	    "original_sec INTEGER,original_nsec INTEGER,original_nlink INTEGER,"
	    "sec INTEGER,nsec INTEGER,nlink INTEGER,"
	    "PRIMARY KEY(device,inode,original_sec,original_nsec,original_nlink)) WITHOUT ROWID") ||
	    jd_store_prepare(store, &app.lookup, "SELECT l.sec,l.nsec,l.nlink"
	    " FROM occurrences o JOIN link_changes l ON l.device=o.device AND l.inode=o.inode"
	    " AND l.original_sec=o.ctime_sec AND l.original_nsec=o.ctime_nsec"
	    " AND l.original_nlink=o.nlink WHERE o.id=?") ||
	    jd_store_prepare(store, &app.save, "INSERT OR REPLACE INTO link_changes"
	    " SELECT device,inode,ctime_sec,ctime_nsec,nlink,?,?,? FROM occurrences WHERE id=?")))
		goto done;
#ifdef JARO_DEDUP_TESTING
	if (stats->fail_links && jd_store_exec(store, "CREATE TEMP TRIGGER fail_link"
	    " BEFORE INSERT ON link_changes BEGIN SELECT RAISE(ABORT,'test link write failed'); END"))
		goto done;
#endif
	interrupted = 0;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &old_int) < 0) {
		perror("dedup apply: install SIGINT handler");
		goto done;
	}
	if (sigaction(SIGTERM, &action, &old_term) < 0) {
		perror("dedup apply: install SIGTERM handler");
	} else {
		if (!gate(stats, "apply-ready"))
			result = jd_apply_cursor(store, inventory, apply_provisional, apply_proved, &app);
		sigaction(SIGTERM, &old_term, NULL);
	}
	sigaction(SIGINT, &old_int, NULL);
	if (interrupted) {
		fprintf(stderr, "dedup apply: interrupted by signal %d\n", (int)interrupted);
		result = -1;
	}
done:
	sqlite3_finalize(app.save);
	sqlite3_finalize(app.lookup);
	return result || stats->conflicts ? -1 : 0;
}
