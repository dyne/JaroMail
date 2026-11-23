#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "classify.h"
#include "profile.h"
#include "filter/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAMPLE_WINDOW (16 * 1024)
#define READ_BLOCK (64 * 1024)

static int
write_escaped(FILE *stream, const unsigned char *bytes, size_t length)
{
	size_t i;
	for (i = 0; i < length; i++) {
		unsigned c = bytes[i];
		if (c < 32 || c > 126 || c == '"' || c == '\\') {
			if (fprintf(stream, "\\x%02x", c) < 0)
				return -1;
		} else if (fputc((int)c, stream) == EOF) {
			return -1;
		}
	}
	return 0;
}

int
jd_write_path(FILE *stream, const struct jd_occurrence *entry)
{
	const char *root = entry->root->path;
	return fputc('"', stream) == EOF ||
	    write_escaped(stream, (const unsigned char *)root, strlen(root)) ||
	    fputs(entry->subdir ? "/new/" : "/cur/", stream) == EOF ||
	    write_escaped(stream, entry->name, entry->length) || fputc('"', stream) == EOF ? -1 : 0;
}

static int
content_error(const struct jd_occurrence *entry, const char *operation)
{
	int saved = errno;
	fprintf(stderr, "dedup content: %s ", operation);
	jd_write_path(stderr, entry);
	fprintf(stderr, ": %s\n", strerror(saved));
	errno = saved;
	return -1;
}

static int
matches_snapshot(const struct stat *observed, const struct stat *expected)
{
	return S_ISREG(observed->st_mode) && observed->st_size == expected->st_size &&
	    observed->st_nlink == expected->st_nlink && observed->st_mode == expected->st_mode &&
	    observed->st_dev == expected->st_dev && observed->st_ino == expected->st_ino &&
	    observed->JD_MTIME.tv_sec == expected->JD_MTIME.tv_sec &&
	    observed->JD_MTIME.tv_nsec == expected->JD_MTIME.tv_nsec &&
	    observed->JD_CTIME.tv_sec == expected->JD_CTIME.tv_sec &&
	    observed->JD_CTIME.tv_nsec == expected->JD_CTIME.tv_nsec;
}

int
jd_message_open(struct jd_message *message, const struct jd_occurrence *entry,
    struct jd_read_stats *reads)
{
	struct stat observed;
	*message = (struct jd_message){.root = -1, .dir = -1, .fd = -1, .entry = entry};
	message->name = malloc(entry->length + 1);
	if (!message->name)
		return content_error(entry, "copy basename");
	memcpy(message->name, entry->name, entry->length);
	message->name[entry->length] = '\0';
	message->dir = jd_open_subdir(entry->root, entry->subdir, &message->root);
	if (message->dir < 0)
		goto error;
	/* O_NONBLOCK also protects against a concurrent replacement with a FIFO. */
	do {
		message->fd = openat(message->dir, message->name,
		    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
	} while (message->fd < 0 && errno == EINTR);
	if (message->fd < 0) {
		content_error(entry, "open message");
		goto error;
	}
	reads->opens++;
	JD_OPENED();
	if (fstat(message->fd, &observed) < 0) {
		content_error(entry, "stat message");
		goto error;
	}
	if (!matches_snapshot(&observed, &entry->metadata)) {
		errno = ESTALE;
		content_error(entry, "message changed since inventory");
		goto error;
	}
	return 0;
error:
	{
		int saved = errno;
		jd_message_close(message);
		errno = saved;
	}
	return -1;
}

int
jd_message_verify(const struct jd_message *message)
{
	const struct jd_occurrence *entry = message->entry;
	struct stat observed;
	if (jd_verify_subdir(entry->root, entry->subdir, message->root))
		return -1;
	if (fstatat(message->dir, message->name, &observed, AT_SYMLINK_NOFOLLOW) < 0)
		return content_error(entry, "stat message after reading");
	if (!matches_snapshot(&observed, &entry->metadata)) {
		errno = ESTALE;
		return content_error(entry, "message changed while reading");
	}
	return 0;
}

void
jd_message_close(struct jd_message *message)
{
	if (message->fd >= 0)
		close(message->fd);
	if (message->dir >= 0)
		close(message->dir);
	if (message->root >= 0)
		close(message->root);
	free(message->name);
}

/* Read exactly this bounded interval of the inventoried file. A short
 * positive result is usable, but EOF before the interval ends is an error. */
static int
read_at(const struct jd_message *message, unsigned char *buffer, size_t length,
    off_t offset, struct jd_read_stats *reads)
{
	while (length) {
		ssize_t got = pread(message->fd, buffer, length, offset);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			if (!got) {
				if (jd_message_verify(message))
					return -1;
				errno = EIO;
			}
			return content_error(message->entry, got ? "read message" : "unexpected message EOF");
		}
		reads->bytes += (size_t)got;
		buffer += got;
		length -= (size_t)got;
		offset += (off_t)got;
	}
	return 0;
}

static size_t
block_length(off_t remaining)
{
	return remaining < READ_BLOCK ? (size_t)remaining : READ_BLOCK;
}

static int
digest_message(const struct jd_occurrence *entry, int sample,
    char digest[65], struct jd_read_stats *reads)
{
	struct jd_message message;
	struct jf_sha256 hash;
	unsigned char block[READ_BLOCK];
	off_t size = entry->metadata.st_size, offset;
	int result = -1;
	JD_START(digest_start);

	if (jd_message_open(&message, entry, reads)) {
		reads->conflicts += jd_entry_conflict(errno);
		return -1;
	}
	jf_sha256_init(&hash);
	if (sample) {
		/* Only sizes above two windows use this route. */
		if (read_at(&message, block, SAMPLE_WINDOW, 0, reads) ||
		    read_at(&message, block + SAMPLE_WINDOW, SAMPLE_WINDOW,
		    size - SAMPLE_WINDOW, reads))
			goto done;
		jf_sha256_update(&hash, block, 2 * SAMPLE_WINDOW);
	} else {
		for (offset = 0; offset < size;) {
			size_t length = block_length(size - offset);
			if (read_at(&message, block, length, offset, reads))
				goto done;
			jf_sha256_update(&hash, block, length);
			offset += (off_t)length;
		}
	}
	if (jd_message_verify(&message))
		goto done;
	jf_sha256_hex(&hash, digest);
	result = 0;
done:
	if (result < 0 && jd_entry_conflict(errno))
		reads->conflicts++;
	jd_message_close(&message);
	JD_END(digest_start, sample ? JD_SAMPLE : JD_HASH);
	return result;
}

int
jd_compare_opened(const struct jd_message *a, const struct jd_message *b,
    struct jd_read_stats *reads)
{
	const struct stat *left = &a->entry->metadata, *right = &b->entry->metadata;
	unsigned char first[READ_BLOCK], second[READ_BLOCK];
	off_t size = left->st_size, offset;
	int same_inode = left->st_dev == right->st_dev && left->st_ino == right->st_ino;
	int equal = size == right->st_size;
	JD_START(compare_start);

	for (offset = 0; equal && !same_inode && offset < size;) {
		size_t length = block_length(size - offset);
		if (read_at(a, first, length, offset, reads) ||
		    read_at(b, second, length, offset, reads))
			return -1;
		equal = memcmp(first, second, length) == 0;
		offset += (off_t)length;
	}
	if (jd_message_verify(a) || jd_message_verify(b))
		return -1;
	if (equal && same_inode)
		reads->inode_matches++;
	JD_END(compare_start, JD_COMPARE);
	return equal;
}

static int
compare_messages(const struct jd_occurrence *left, const struct jd_occurrence *right,
    struct jd_read_stats *reads)
{
	struct jd_message a, b;
	int equal = -1;
	if (jd_message_open(&a, left, reads)) {
		reads->conflicts += jd_entry_conflict(errno);
		return -1;
	}
	if (!jd_message_open(&b, right, reads)) {
		equal = jd_compare_opened(&a, &b, reads);
		if (equal < 0)
			reads->conflicts += jd_entry_conflict(errno);
		jd_message_close(&b);
	} else {
		reads->conflicts += jd_entry_conflict(errno);
	}
	jd_message_close(&a);
	return equal;
}

struct reduction {
	struct jd_store *store;
	const struct jd_inventory *inventory;
	struct jd_classify_stats *stats;
	sqlite3_stmt *save_pair, *save_sample, *save_hash;
};

static int
insert_done(struct jd_store *store, sqlite3_stmt *statement, int code)
{
	if (code == SQLITE_OK)
		code = sqlite3_step(statement);
	if (code != SQLITE_DONE)
		return jd_store_error(store, "insert classification candidate", code);
	code = sqlite3_reset(statement);
	return code == SQLITE_OK ? 0 : jd_store_error(store, "reset candidate insert", code);
}

static int
insert_pair(struct jd_store *store, sqlite3_stmt *statement,
    sqlite3_int64 first, sqlite3_int64 second)
{
	int code = sqlite3_bind_int64(statement, 1, first);
	if (code == SQLITE_OK)
		code = sqlite3_bind_int64(statement, 2, second);
	return insert_done(store, statement, code);
}

static int
spool_pairs(struct reduction *pass, sqlite3_stmt *members)
{
	int code = sqlite3_step(members);
	sqlite3_int64 first;
	if (code != SQLITE_ROW)
		return jd_store_error(pass->store, "find first pair member", code);
	first = sqlite3_column_int64(members, JD_OCCURRENCE_ID_COLUMN);
	while ((code = sqlite3_step(members)) == SQLITE_ROW) {
		sqlite3_int64 second = sqlite3_column_int64(members, JD_OCCURRENCE_ID_COLUMN);
		if (insert_pair(pass->store, pass->save_pair, first, second))
			return -1;
	}
	return code == SQLITE_DONE ? 0 : jd_store_error(pass->store, "read pair members", code);
}

static int
digest_group(struct reduction *pass, sqlite3_stmt *members, int sample)
{
	int code;
	while ((code = sqlite3_step(members)) == SQLITE_ROW) {
		struct jd_occurrence entry;
		char digest[65];
		sqlite3_stmt *save = sample ? pass->save_sample : pass->save_hash;
		struct jd_read_stats *reads = sample ? &pass->stats->sample : &pass->stats->hash;
		if (jd_store_read_occurrence(pass->store, members, pass->inventory, &entry) ||
		    digest_message(&entry, sample, digest, reads))
			return -1;
#ifdef JARO_DEDUP_TESTING
		if (!sample && pass->store->force_digest)
			memset(digest, '0', sizeof(digest) - 1);
#endif
		code = sqlite3_bind_int64(save, 1, (sqlite3_int64)entry.metadata.st_size);
		if (code == SQLITE_OK)
			code = sqlite3_bind_text(save, 2, digest, 64, SQLITE_TRANSIENT);
		if (code == SQLITE_OK)
			code = sqlite3_bind_int64(save, 3, entry.id);
		if (insert_done(pass->store, save, code))
			return -1;
		if (sample)
			pass->stats->sampled_files++;
		else
			pass->stats->hashed_files++;
	}
	return code == SQLITE_DONE ? 0 : jd_store_error(pass->store, "read candidate group", code);
}

static int
reduce_sizes(struct reduction *pass)
{
	sqlite3_stmt *sizes = NULL, *members = NULL, *same_inode = NULL;
	int code, result = -1;
	if (jd_store_prepare(pass->store, &sizes, "SELECT size,count(*) FROM occurrences "
	    "INDEXED BY occurrence_size GROUP BY size HAVING count(*)>1") ||
	    jd_store_prepare(pass->store, &members, "SELECT " JD_OCCURRENCE_COLUMNS
	    " FROM occurrences AS o INDEXED BY occurrence_size WHERE size=? ORDER BY rank,subdir,name") ||
	    jd_store_prepare(pass->store, &same_inode, "SELECT NOT EXISTS("
	    " SELECT 1 FROM occurrences INDEXED BY occurrence_size WHERE size=?1 AND (device,inode)<>("
	    " SELECT device,inode FROM occurrences INDEXED BY occurrence_size WHERE size=?1"
	    " ORDER BY rank,subdir,name LIMIT 1))"))
		goto done;
	while ((code = sqlite3_step(sizes)) == SQLITE_ROW) {
		sqlite3_int64 size = sqlite3_column_int64(sizes, 0);
		sqlite3_int64 count = sqlite3_column_int64(sizes, 1);
		int direct = count == 2;
		/* One physical inode can be proved without hashing even with many names.
		 * Mixed-inode groups exit the indexed test at the first other inode. */
		if (!direct) {
			code = sqlite3_bind_int64(same_inode, 1, size);
			if (code == SQLITE_OK)
				code = sqlite3_step(same_inode);
			if (code != SQLITE_ROW) {
				jd_store_error(pass->store, "check size group inode identity", code);
				goto done;
			}
			direct = sqlite3_column_int(same_inode, 0);
			code = sqlite3_reset(same_inode);
			if (code != SQLITE_OK) {
				jd_store_error(pass->store, "reset inode group query", code);
				goto done;
			}
		}
		code = sqlite3_bind_int64(members, 1, size);
		if (code != SQLITE_OK) {
			jd_store_error(pass->store, "bind repeated size", code);
			goto done;
		}
		pass->stats->repeated_sizes++;
		pass->stats->repeated_entries += (uintmax_t)count;
		if (direct ? spool_pairs(pass, members) :
		    digest_group(pass, members, size > 2 * SAMPLE_WINDOW))
			goto done;
		code = sqlite3_reset(members);
		if (code != SQLITE_OK) {
			jd_store_error(pass->store, "reset size cursor", code);
			goto done;
		}
	}
	result = code == SQLITE_DONE ? 0 : jd_store_error(pass->store, "read repeated sizes", code);
done:
	sqlite3_finalize(same_inode);
	sqlite3_finalize(members);
	sqlite3_finalize(sizes);
	return result;
}

static int
reduce_samples(struct reduction *pass)
{
	sqlite3_stmt *keys = NULL, *members = NULL;
	int code, result = -1;
	if (jd_store_prepare(pass->store, &keys, "SELECT size,digest,count(*) FROM samples "
	    "GROUP BY size,digest HAVING count(*)>1") ||
	    jd_store_prepare(pass->store, &members, "SELECT " JD_OCCURRENCE_COLUMNS
	    " FROM samples AS s JOIN occurrences AS o ON o.id=s.id"
	    " WHERE s.size=? AND s.digest=? ORDER BY o.rank,o.subdir,o.name"))
		goto done;
	while ((code = sqlite3_step(keys)) == SQLITE_ROW) {
		sqlite3_int64 count = sqlite3_column_int64(keys, 2);
		code = sqlite3_bind_value(members, 1, sqlite3_column_value(keys, 0));
		if (code == SQLITE_OK)
			code = sqlite3_bind_value(members, 2, sqlite3_column_value(keys, 1));
		if (code != SQLITE_OK) {
			jd_store_error(pass->store, "bind sample key", code);
			goto done;
		}
		pass->stats->sample_buckets++;
		if (count == 2 ? spool_pairs(pass, members) : digest_group(pass, members, 0))
			goto done;
		code = sqlite3_reset(members);
		if (code != SQLITE_OK) {
			jd_store_error(pass->store, "reset sample cursor", code);
			goto done;
		}
	}
	result = code == SQLITE_DONE ? 0 : jd_store_error(pass->store, "read repeated samples", code);
done:
	sqlite3_finalize(members);
	sqlite3_finalize(keys);
	return result;
}

int
jd_reduce_candidates(struct jd_store *store, const struct jd_inventory *inventory,
    struct jd_classify_stats *stats)
{
	struct reduction pass = {.store = store, .inventory = inventory, .stats = stats};
	int result = -1;
	memset(stats, 0, sizeof(*stats));
	if (jd_store_require_inventory(store))
		return -1;
	/* The scratch transaction writes keys only. SQLite's FILE temp store
	 * and heap limit cover buckets and priority sorts, even for one huge size. */
	if (jd_store_exec(store, "BEGIN;"
	    "CREATE TEMP TABLE samples(size INTEGER,digest TEXT,id INTEGER,"
	    "PRIMARY KEY(size,digest,id)) WITHOUT ROWID;"
	    "CREATE TEMP TABLE hashes(size INTEGER,digest TEXT,id INTEGER,"
	    "PRIMARY KEY(size,digest,id)) WITHOUT ROWID;"
	    "CREATE TEMP TABLE pairs(preferred INTEGER NOT NULL,other INTEGER PRIMARY KEY);"
	    "CREATE TEMP TABLE matches(winner INTEGER NOT NULL,duplicate INTEGER PRIMARY KEY);") ||
	    jd_store_prepare(store, &pass.save_pair, "INSERT INTO pairs VALUES(?,?)") ||
	    jd_store_prepare(store, &pass.save_sample, "INSERT INTO samples VALUES(?,?,?)") ||
	    jd_store_prepare(store, &pass.save_hash, "INSERT INTO hashes VALUES(?,?,?)") ||
	    reduce_sizes(&pass) || reduce_samples(&pass))
		goto done;
	result = 0;
done:
	sqlite3_finalize(pass.save_hash);
	sqlite3_finalize(pass.save_sample);
	sqlite3_finalize(pass.save_pair);
	if (!result)
		result = jd_store_exec(store, "DROP TABLE samples; COMMIT");
	if (result && !sqlite3_get_autocommit(store->db))
		jd_store_exec(store, "ROLLBACK");
	return result;
}

static int
lookup_occurrence(struct jd_store *store, const struct jd_inventory *inventory,
    sqlite3_stmt *statement, sqlite3_int64 id, struct jd_occurrence *entry)
{
	int code = sqlite3_bind_int64(statement, 1, id);
	if (code == SQLITE_OK)
		code = sqlite3_step(statement);
	if (code != SQLITE_ROW)
		return jd_store_error(store, "look up pair member", code);
	return jd_store_read_occurrence(store, statement, inventory, entry);
}

static int
pair_cursor(struct jd_store *store, const struct jd_inventory *inventory,
    const char *query, jd_emit_pair provisional, jd_emit_pair proved, void *context)
{
	sqlite3_stmt *pairs = NULL, *preferred = NULL, *other = NULL;
	const char *lookup = "SELECT " JD_OCCURRENCE_COLUMNS " FROM occurrences AS o WHERE id=?";
	int code, result = -1;
	if (jd_store_prepare(store, &pairs, query) ||
	    jd_store_prepare(store, &preferred, lookup) || jd_store_prepare(store, &other, lookup))
		goto done;
	while ((code = sqlite3_step(pairs)) == SQLITE_ROW) {
		struct jd_occurrence first, second;
		jd_emit_pair emit = sqlite3_column_int(pairs, 2) ? proved : provisional;
		if (lookup_occurrence(store, inventory, preferred, sqlite3_column_int64(pairs, 0), &first) ||
		    lookup_occurrence(store, inventory, other, sqlite3_column_int64(pairs, 1), &second) ||
		    emit(context, &first, &second))
			goto done;
		code = sqlite3_reset(preferred);
		if (code == SQLITE_OK)
			code = sqlite3_reset(other);
		if (code != SQLITE_OK) {
			jd_store_error(store, "reset pair cursor", code);
			goto done;
		}
	}
	result = code == SQLITE_DONE ? 0 : jd_store_error(store, "read provisional pairs", code);
done:
	sqlite3_finalize(other);
	sqlite3_finalize(preferred);
	sqlite3_finalize(pairs);
	return result;
}

#define PAIR_ORDER " ORDER BY w.rank,w.subdir,w.name,d.rank,d.subdir,d.name"

int
jd_pair_cursor(struct jd_store *store, const struct jd_inventory *inventory,
    jd_emit_pair emit, void *context)
{
	return pair_cursor(store, inventory, "SELECT p.preferred,p.other,0 FROM pairs AS p"
	    " JOIN occurrences AS w ON p.preferred=w.id JOIN occurrences AS d ON p.other=d.id"
	    PAIR_ORDER, emit, emit, context);
}

int
jd_apply_cursor(struct jd_store *store, const struct jd_inventory *inventory,
    jd_emit_pair provisional, jd_emit_pair proved, void *context)
{
	return pair_cursor(store, inventory, "WITH candidates(keeper,loser,proved) AS ("
	    "SELECT preferred,other,0 FROM pairs UNION ALL SELECT winner,duplicate,1 FROM matches)"
	    " SELECT c.keeper,c.loser,c.proved FROM candidates c"
	    " JOIN occurrences w ON w.id=c.keeper JOIN occurrences d ON d.id=c.loser"
	    PAIR_ORDER, provisional, proved, context);
}

struct proof {
	struct jd_store *store;
	const struct jd_inventory *inventory;
	struct jd_classify_stats *stats;
	sqlite3_stmt *matches;
};

static int
prove_pair(void *context, const struct jd_occurrence *first,
    const struct jd_occurrence *second)
{
	struct proof *pass = context;
	int equal = compare_messages(first, second, &pass->stats->pair);
	if (equal < 0)
		return -1;
	pass->stats->compared_pairs++;
	if (equal) {
		if (insert_pair(pass->store, pass->matches, first->id, second->id))
			return -1;
		pass->stats->matching_pairs++;
	}
	return 0;
}

int
jd_prove_pairs(struct jd_store *store, const struct jd_inventory *inventory,
    struct jd_classify_stats *stats)
{
	struct proof pass = {store, inventory, stats, NULL};
	int result = -1;
	if (!jd_store_exec(store, "BEGIN") &&
	    !jd_store_prepare(store, &pass.matches, "INSERT INTO matches VALUES(?,?)") &&
	    !jd_pair_cursor(store, inventory, prove_pair, &pass))
		result = 0;
	sqlite3_finalize(pass.matches);
	if (!result)
		result = jd_store_exec(store, "COMMIT");
	if (result && !sqlite3_get_autocommit(store->db))
		jd_store_exec(store, "ROLLBACK");
	return result;
}

/* Representatives are inserted in priority order. Only a byte match to an
 * earlier, still-verified representative can make the current entry a loser. */
static int
prove_member(struct proof *pass, const struct jd_occurrence *entry,
    sqlite3_stmt *representatives, sqlite3_stmt *add_representative)
{
	int code, equal = 0;
	sqlite3_int64 keeper = 0;
	while ((code = sqlite3_step(representatives)) == SQLITE_ROW) {
		struct jd_occurrence representative;
		if (jd_store_read_occurrence(pass->store, representatives, pass->inventory, &representative))
			return -1;
		equal = compare_messages(&representative, entry, &pass->stats->proof);
		if (equal < 0)
			return -1;
		pass->stats->proof_comparisons++;
		if (equal) {
			keeper = representative.id;
			break;
		}
	}
	if (code != SQLITE_DONE && code != SQLITE_ROW)
		return jd_store_error(pass->store, "read proof representatives", code);
	code = sqlite3_reset(representatives);
	if (code != SQLITE_OK)
		return jd_store_error(pass->store, "reset proof representatives", code);
	if (equal) {
		if (insert_pair(pass->store, pass->matches, keeper, entry->id))
			return -1;
		pass->stats->proof_matches++;
		return 0;
	}
	code = sqlite3_bind_int64(add_representative, 1, entry->id);
	return insert_done(pass->store, add_representative, code);
}

int
jd_prove_hashes(struct jd_store *store, const struct jd_inventory *inventory,
    struct jd_classify_stats *stats)
{
	struct proof pass = {store, inventory, stats, NULL};
	sqlite3_stmt *buckets = NULL, *members = NULL, *representatives = NULL, *add_representative = NULL;
	int code, result = -1;
	if (jd_store_exec(store, "BEGIN; CREATE TEMP TABLE representatives("
	    "priority INTEGER PRIMARY KEY, id INTEGER NOT NULL)") ||
	    jd_store_prepare(store, &buckets, "SELECT size,digest FROM hashes GROUP BY size,digest HAVING count(*)>1") ||
	    jd_store_prepare(store, &members, "SELECT " JD_OCCURRENCE_COLUMNS
	    " FROM hashes AS h JOIN occurrences AS o ON o.id=h.id"
	    " WHERE h.size=? AND h.digest=? ORDER BY o.rank,o.subdir,o.name") ||
	    jd_store_prepare(store, &representatives, "SELECT " JD_OCCURRENCE_COLUMNS
	    " FROM representatives AS r JOIN occurrences AS o ON o.id=r.id ORDER BY r.priority") ||
	    jd_store_prepare(store, &add_representative, "INSERT INTO representatives(id) VALUES(?)") ||
	    jd_store_prepare(store, &pass.matches, "INSERT INTO matches VALUES(?,?)"))
		goto done;
	while ((code = sqlite3_step(buckets)) == SQLITE_ROW) {
		code = sqlite3_bind_value(members, 1, sqlite3_column_value(buckets, 0));
		if (code == SQLITE_OK)
			code = sqlite3_bind_value(members, 2, sqlite3_column_value(buckets, 1));
		if (code != SQLITE_OK) {
			jd_store_error(store, "bind proof bucket", code);
			goto done;
		}
		stats->proof_buckets++;
		while ((code = sqlite3_step(members)) == SQLITE_ROW) {
			struct jd_occurrence entry;
			if (jd_store_read_occurrence(store, members, inventory, &entry) ||
			    prove_member(&pass, &entry, representatives, add_representative))
				goto done;
		}
		if (code == SQLITE_DONE)
			code = sqlite3_reset(members);
		if (code != SQLITE_OK) {
			jd_store_error(store, "read proof bucket members", code);
			goto done;
		}
		if (jd_store_exec(store, "DELETE FROM representatives"))
			goto done;
	}
	result = code == SQLITE_DONE ? 0 : jd_store_error(store, "read proof buckets", code);
done:
	sqlite3_finalize(pass.matches);
	sqlite3_finalize(add_representative);
	sqlite3_finalize(representatives);
	sqlite3_finalize(members);
	sqlite3_finalize(buckets);
	if (!result)
		result = jd_store_exec(store, "DROP TABLE representatives; COMMIT");
	if (result && !sqlite3_get_autocommit(store->db))
		jd_store_exec(store, "ROLLBACK");
	return result;
}

#ifdef JARO_DEDUP_TESTING
static int
dump_path(sqlite3_stmt *cursor, int start)
{
	const void *name = sqlite3_column_blob(cursor, start + 2);
	size_t length = (size_t)sqlite3_column_bytes(cursor, start + 2);
	return !name || printf("%" PRId64 "%c%d%c", (int64_t)sqlite3_column_int64(cursor, start),
	    0, sqlite3_column_int(cursor, start + 1), 0) < 0 ||
	    fwrite(name, 1, length, stdout) != length || putchar(0) == EOF;
}

int
jd_dump_candidates(struct jd_store *store)
{
	sqlite3_stmt *cursor = NULL;
	int code;
	if (jd_store_prepare(store, &cursor, "SELECT w.rank,w.subdir,w.name,d.rank,d.subdir,d.name FROM matches AS p"
	    " JOIN occurrences AS w ON p.winner=w.id JOIN occurrences AS d ON p.duplicate=d.id"
	    " ORDER BY w.rank,w.subdir,w.name,d.rank,d.subdir,d.name"))
		return -1;
	while ((code = sqlite3_step(cursor)) == SQLITE_ROW) {
		if (fwrite("P", 1, 2, stdout) != 2 || dump_path(cursor, 0) || dump_path(cursor, 3)) {
			sqlite3_finalize(cursor);
			return -1;
		}
	}
	sqlite3_finalize(cursor);
	if (code != SQLITE_DONE)
		return jd_store_error(store, "dump direct pair matches", code);

	if (jd_store_prepare(store, &cursor, "WITH repeated AS (SELECT size,digest FROM hashes"
	    " GROUP BY size,digest HAVING count(*)>1)"
	    " SELECT h.size,h.digest,o.rank,o.subdir,o.name FROM repeated AS r"
	    " JOIN hashes AS h ON h.size=r.size AND h.digest=r.digest"
	    " JOIN occurrences AS o ON o.id=h.id ORDER BY h.size,h.digest,o.rank,o.subdir,o.name"))
		return -1;
	while ((code = sqlite3_step(cursor)) == SQLITE_ROW) {
		const char *digest = (const char *)sqlite3_column_text(cursor, 1);
		if (!digest || printf("H%c%" PRId64 "%c%s%c", 0, (int64_t)sqlite3_column_int64(cursor, 0),
		    0, digest, 0) < 0 || dump_path(cursor, 2)) {
			sqlite3_finalize(cursor);
			return -1;
		}
	}
	sqlite3_finalize(cursor);
	return code == SQLITE_DONE ? 0 : jd_store_error(store, "dump repeated hash buckets", code);
}

int
jd_dump_survivors(struct jd_store *store)
{
	sqlite3_stmt *cursor = NULL;
	int code;
	if (jd_store_prepare(store, &cursor, "SELECT o.rank,o.subdir,o.name,"
	    " coalesce(w.rank,o.rank),coalesce(w.subdir,o.subdir),coalesce(w.name,o.name)"
	    " FROM occurrences AS o LEFT JOIN matches AS m ON m.duplicate=o.id"
	    " LEFT JOIN occurrences AS w ON w.id=m.winner ORDER BY o.rank,o.subdir,o.name"))
		return -1;
	while ((code = sqlite3_step(cursor)) == SQLITE_ROW) {
		if (dump_path(cursor, 0) || dump_path(cursor, 3)) {
			sqlite3_finalize(cursor);
			return -1;
		}
	}
	sqlite3_finalize(cursor);
	return code == SQLITE_DONE ? 0 : jd_store_error(store, "dump survivor map", code);
}

void
jd_print_classify_stats(const struct jd_classify_stats *stats)
{
	fprintf(stderr, "dedup candidates: repeated-sizes=%ju repeated-entries=%ju"
	    " sample-buckets=%ju sampled-files=%ju sample-opens=%ju sample-bytes=%ju"
	    " hashed-files=%ju hash-opens=%ju hash-bytes=%ju"
	    " compared-pairs=%ju matching-pairs=%ju pair-opens=%ju pair-bytes=%ju pair-inode-matches=%ju"
	    " proof-buckets=%ju proof-comparisons=%ju proof-matches=%ju"
	    " proof-opens=%ju proof-bytes=%ju proof-inode-matches=%ju\n",
	    stats->repeated_sizes, stats->repeated_entries, stats->sample_buckets,
	    stats->sampled_files, stats->sample.opens, stats->sample.bytes,
	    stats->hashed_files, stats->hash.opens, stats->hash.bytes, stats->compared_pairs,
	    stats->matching_pairs, stats->pair.opens, stats->pair.bytes, stats->pair.inode_matches,
	    stats->proof_buckets, stats->proof_comparisons, stats->proof_matches,
	    stats->proof.opens, stats->proof.bytes, stats->proof.inode_matches);
}
#endif
