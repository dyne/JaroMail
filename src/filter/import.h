#ifndef JARO_FILTER_IMPORT_H
#define JARO_FILTER_IMPORT_H
#include "snapshot.h"
/* Fixed prospective input set: Filters.txt, Aliases.txt, whitelist.abook,
 * blacklist.abook. Parser/merge have no filesystem effects. */
#define JF_IMPORT_FILES 4
extern const char *const jf_import_names[JF_IMPORT_FILES];
int jf_sieve_import(const struct jf_bytes *, int, const struct jf_profile *,
	struct jf_policy **, struct jf_error *);
int jf_import_merge(const struct jf_policy *, const struct jf_policy *,
	const struct jf_bytes [JF_IMPORT_FILES], struct jf_bytes [JF_IMPORT_FILES], struct jf_error *);
#endif
