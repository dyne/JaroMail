#ifndef JARO_FILTER_SHA256_H
#define JARO_FILTER_SHA256_H
#include <stddef.h>
#include <stdint.h>
struct jf_sha256 {
	uint32_t state[8];
	uint64_t bytes;
	unsigned char block[64];
	size_t used;
};
void jf_sha256_init(struct jf_sha256 *);
void jf_sha256_update(struct jf_sha256 *, const void *, size_t);
void jf_sha256_hex(struct jf_sha256 *, char out[65]);
#endif
