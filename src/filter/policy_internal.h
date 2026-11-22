#ifndef JARO_FILTER_POLICY_INTERNAL_H
#define JARO_FILTER_POLICY_INTERNAL_H
#include "policy.h"
#include <locale.h>
#include <regex.h>

struct jf_entry {
	struct jf_rule rule;
	int discard;
	size_t order, input_index;
};
struct jf_policy {
	struct jf_allocator allocator;
	struct jf_entry *entries;
	size_t count, capacity;
	locale_t locale;
};
int jf_valid_glob(const char *);
int jf_valid_domain(const char *);
int jf_valid_utf8(const unsigned char *, size_t);
int jf_valid_mailbox(const char *);
/* In-place minimal mailbox quoting. Never grows a valid input. Does not fold
 * case or reinterpret a regex. Returns 0 for unsupported/empty local parts. */
int jf_normalize_mailbox(char *);
#define JF_ADDRESS_NORMALIZATION "mailbox-minimal-quoting-v2"
#endif
