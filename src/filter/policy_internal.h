#ifndef JARO_FILTER_POLICY_INTERNAL_H
#define JARO_FILTER_POLICY_INTERNAL_H
#include "policy.h"
#include "../mailbox.h"
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
#define JF_ADDRESS_NORMALIZATION "mailbox-minimal-quoting-v2"
#endif
