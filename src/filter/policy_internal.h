#ifndef JARO_FILTER_POLICY_INTERNAL_H
#define JARO_FILTER_POLICY_INTERNAL_H
#include "policy.h"
#include <locale.h>
#include <regex.h>

struct jf_entry {
	struct jf_rule rule;
	regex_t regex;
	int compiled, discard;
	size_t order, input_index;
};
struct jf_policy {
	struct jf_allocator allocator;
	struct jf_entry *entries;
	size_t count, capacity;
	locale_t locale;
	struct jf_entry **index, **legacy;
	size_t index_count, legacy_count;
	const struct jf_entry *bounce, *spam, *fallback;
};
int jf_policy_build_index(struct jf_policy *, struct jf_error *);
/* Canonical cache R fields, borrowed only during reconstruction. The cache
 * validator must have accepted the complete product before this is called. */
struct jf_cached_rule { struct jf_text fields[12]; };
int jf_policy_restore(const struct jf_cached_rule *, size_t,
	const struct jf_allocator *, struct jf_policy **, struct jf_error *);
int jf_valid_domain(const char *);
int jf_valid_utf8(const unsigned char *, size_t);
int jf_valid_mailbox(const char *);
/* In-place minimal mailbox quoting. Never grows a valid input. Does not fold
 * case or reinterpret a regex. Returns 0 for unsupported/empty local parts. */
int jf_normalize_mailbox(char *);
#define JF_ADDRESS_NORMALIZATION "mailbox-minimal-quoting-v2"
#endif
