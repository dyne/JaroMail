#ifndef JARO_DEDUP_APPLY_H
#define JARO_DEDUP_APPLY_H

#include "classify.h"

struct jd_apply_stats {
	uintmax_t groups, removed, proposed, conflicts, logical_bytes, last_link_bytes;
	int preview;
#ifdef JARO_DEDUP_TESTING
	const char *hold_at;
	int fail_links;
#endif
};

/* Consume provisional pairs and byte-proved hash matches under apply locks. */
int jd_apply_matches(struct jd_store *, const struct jd_inventory *,
    struct jd_classify_stats *, struct jd_apply_stats *);

#endif
