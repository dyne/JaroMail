#ifndef JARO_DEDUP_CLASSIFY_H
#define JARO_DEDUP_CLASSIFY_H

#include "store.h"
#include <stdint.h>
#include <stdio.h>

struct jd_read_stats {
	uintmax_t opens, bytes, inode_matches, conflicts;
};

struct jd_classify_stats {
	uintmax_t repeated_sizes, repeated_entries, sample_buckets;
	uintmax_t sampled_files, hashed_files, compared_pairs, matching_pairs;
	uintmax_t proof_buckets, proof_comparisons, proof_matches;
	struct jd_read_stats sample, hash, pair, proof;
};

struct jd_message {
	int root, dir, fd;
	char *name;
	const struct jd_occurrence *entry;
};

/* Pin and validate a snapshot. The entry is borrowed until close. */
int jd_message_open(struct jd_message *, const struct jd_occurrence *, struct jd_read_stats *);
int jd_message_verify(const struct jd_message *);
void jd_message_close(struct jd_message *);
int jd_write_path(FILE *, const struct jd_occurrence *);
/* Return 1 for stable equal bytes, 0 for a difference, or -1 on error. */
int jd_compare_opened(const struct jd_message *, const struct jd_message *, struct jd_read_stats *);

/* Build disk-backed hash keys and provisional pairs in keeper priority. */
int jd_reduce_candidates(struct jd_store *, const struct jd_inventory *,
    struct jd_classify_stats *);
typedef int (*jd_emit_pair)(void *, const struct jd_occurrence *, const struct jd_occurrence *);
int jd_pair_cursor(struct jd_store *, const struct jd_inventory *, jd_emit_pair, void *);
int jd_apply_cursor(struct jd_store *, const struct jd_inventory *, jd_emit_pair, jd_emit_pair, void *);

/* Record only byte-proved matches, keyed by the lower-priority occurrence. */
int jd_prove_pairs(struct jd_store *, const struct jd_inventory *, struct jd_classify_stats *);
int jd_prove_hashes(struct jd_store *, const struct jd_inventory *, struct jd_classify_stats *);

#ifdef JARO_DEDUP_TESTING
int jd_dump_candidates(struct jd_store *);
int jd_dump_survivors(struct jd_store *);
void jd_print_classify_stats(const struct jd_classify_stats *);
#endif

#endif
