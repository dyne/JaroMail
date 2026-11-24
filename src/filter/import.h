#ifndef JARO_FILTER_IMPORT_H
#define JARO_FILTER_IMPORT_H
#include "snapshot.h"
/* Prospective inputs include two text files and two SQLite memberships. */
#define JF_IMPORT_FILES 4
#define JF_IMPORT_DISK_FILES 2
extern const char *const jf_import_names[JF_IMPORT_FILES];
int jf_sieve_import(const struct jf_bytes *, int, const struct jf_profile *,
	struct jf_policy **, struct jf_error *);
int jf_import_merge(const struct jf_policy *, const struct jf_policy *,
	const struct jf_bytes [JF_IMPORT_FILES], struct jf_bytes [JF_IMPORT_FILES],
	struct jf_bytes [2], struct jf_error *);
#endif
