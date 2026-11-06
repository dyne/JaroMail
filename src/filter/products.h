/* Compiler product construction from one immutable parsed snapshot.
 * No publication, shell execution or runtime Dovecot dependency. */
#ifndef JARO_FILTER_PRODUCTS_H
#define JARO_FILTER_PRODUCTS_H
#include "policy.h"
#include <stdint.h>
#define JF_COMPILER_ID "jaromail-filter-compiler-v2"
#define JF_CACHE_MAX (128u * 1024u * 1024u)
#define JF_CACHE_HEADER_SIZE 88u
#define JF_CACHE_FILE_MAX (JF_CACHE_MAX + JF_CACHE_HEADER_SIZE)
/* Independent inventory budget; never charged against user policy rules. */
#define JF_DEPENDENCY_MAX 100000u
#define JF_CACHE_RECORD_MAX (1u + JF_DEPENDENCY_MAX + JF_RULE_RECORD_MAX)

struct jf_bytes { unsigned char *data; size_t size, capacity; };
struct jf_mapping { char *logical, *mailbox; };
struct jf_profile {
	char *id, *prefix, *separator;
	int create, fileinto, mailbox, regex;
	struct jf_mapping *mappings;
	size_t count;
};
struct jf_dependency {
	char *role, *path;
	int present;
	unsigned char digest[32];
	struct jf_bytes auxiliary;
};
struct jf_products {
	struct jf_bytes zsh, sieve, cache;
	unsigned char fingerprint[32];
	char id[65];
};
void jf_bytes_free(struct jf_bytes *);
int jf_bytes_add(struct jf_bytes *, const void *, size_t);
/* Default buffers keep the 128 MiB text/payload bound. Only a whole cache
 * buffer uses FILE_MAX, to allow the fixed header in addition to its payload. */
int jf_bytes_add_limit(struct jf_bytes *, const void *, size_t, size_t);
int jf_bytes_fits(size_t, size_t, size_t);
int jf_cache_file_size(size_t, size_t *);
int jf_dependencies_fingerprint(const struct jf_dependency *, size_t, unsigned char [32]);
int jf_cache_dependencies(const struct jf_bytes *, struct jf_dependency **, size_t *);
int jf_bytes_text(struct jf_bytes *, const char *);
void jf_digest(const void *, size_t, unsigned char [32]);
void jf_digest_hex(const unsigned char [32], char [65]);
int jf_product_error(struct jf_error *, const char *, size_t, const char *);
int jf_profile_parse(const unsigned char *, size_t, struct jf_profile *, struct jf_error *);
const char *jf_regex_catalogue(void);
const char *jf_regex_qualification(void);
int jf_regex_supported(const char *);
void jf_profile_free(struct jf_profile *);
char *jf_profile_mailbox(const struct jf_profile *, const struct jf_rule *);
int jf_products_build(const struct jf_policy *, const struct jf_profile *,
	const struct jf_dependency *, size_t, struct jf_products *, struct jf_error *);
void jf_products_free(struct jf_products *);
/* Validate binary framing, canonical records and both product digests.
 * Does not use manifest.json as an authority. Returns 0 or status 5. */
int jf_cache_validate(const unsigned char *, size_t, const struct jf_bytes *,
	const struct jf_bytes *, const unsigned char *, struct jf_error *);
/* Reconstruct the shared indexed model from canonical R records, preserving
 * serialized IDs, order, provenance and mapped destinations. Never reads source
 * inputs. On failure *out is NULL; returns 3 for resources or 5 for bad cache. */
int jf_cache_policy(const struct jf_bytes *, const struct jf_allocator *,
	struct jf_policy **out, struct jf_error *);
#endif
