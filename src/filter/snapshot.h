#ifndef JARO_FILTER_SNAPSHOT_H
#define JARO_FILTER_SNAPSHOT_H
#include "products.h"
struct jaro_contact_store;
struct jf_input_override { const char *path; const struct jf_bytes *bytes; const char *role; };
extern const char *const jf_contact_roles[2];

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
/* Membership in both lists comes from one SQLite read transaction.
 * The caller may supply a store with a transaction already held. */
int jf_snapshot_read(const char *root, const char *accounts, const char *profile,
	const char *compiler, const struct jf_snapshot *prior,
	struct jf_snapshot *, struct jf_error *);
int jf_snapshot_collect(const char *, const char *, const char *, const char *, const char *,
	int, struct jaro_contact_store *, int (*snapshot_hook)(const char *), struct jf_snapshot *, struct jf_error *);
int jf_snapshot_overlay(const char *, const char *, const char *, const char *, const char *,
	int, const struct jf_input_override *, size_t, struct jf_snapshot *, struct jf_error *);
int jf_snapshot_parse(struct jf_snapshot *, struct jf_error *);
void jf_snapshot_free(struct jf_snapshot *);
const struct jf_dependency *jf_snapshot_dependency(const struct jf_snapshot *, const char *, const char *);
#endif
