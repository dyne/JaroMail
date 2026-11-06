#ifndef JARO_FILTER_SNAPSHOT_H
#define JARO_FILTER_SNAPSHOT_H
#include "products.h"
struct jf_input_override { const char *path; const struct jf_bytes *bytes; };

struct jf_snapshot {
	struct jf_source *sources;
	size_t source_count;
	struct jf_dependency *dependencies;
	size_t dependency_count;
	struct jf_profile profile;
	struct jf_policy *policy;
	struct jf_bytes warnings;
	/* Owned exact input paths for byte-preserving diagnostics. */
	char *profile_path;
	char *error_path;
	int changed;
	/* Borrowed only while collecting an import's prospective snapshot. */
	const struct jf_input_override *overrides;
	size_t override_count;
};
/* Read each source once. Export a present abook once, through argv and a
 * private descriptor containing snapshot bytes. prior can reuse exports only
 * after raw book/export bytes, exporter flags/locale, compiler bytes and
 * normalization/schema identities have been checked. No source writes.
 * The caller owns publication locking and a complete recheck (L4.2). */
int jf_snapshot_read(const char *root, const char *accounts, const char *profile,
	const char *compiler, const struct jf_snapshot *prior,
	struct jf_snapshot *, struct jf_error *);
int jf_snapshot_collect(const char *, const char *, const char *, const char *,
	const struct jf_snapshot *, struct jf_snapshot *, struct jf_error *);
int jf_snapshot_overlay(const char *, const char *, const char *, const char *,
	const struct jf_input_override *, size_t, struct jf_snapshot *, struct jf_error *);
int jf_snapshot_parse(struct jf_snapshot *, struct jf_error *);
void jf_snapshot_free(struct jf_snapshot *);
const struct jf_dependency *jf_snapshot_dependency(const struct jf_snapshot *, const char *, const char *);
#endif
