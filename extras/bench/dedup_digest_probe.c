/* Linux benchmark-only linker wrappers. Link with --wrap=jf_sha256_{init,
 * update,hex} in the profiling helper. The installed helpers never link this. */
#include "filter/sha256.h"
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static EVP_MD_CTX *
context(const struct jf_sha256 *hash)
{
	EVP_MD_CTX *ctx;
	memcpy(&ctx, hash->block, sizeof(ctx));
	return ctx;
}

void
__wrap_jf_sha256_init(struct jf_sha256 *hash)
{
	const EVP_MD *digest = getenv("JARO_BENCH_BLAKE2S") ? EVP_blake2s256() : EVP_sha256();
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx || !EVP_DigestInit_ex(ctx, digest, NULL))
		abort();
	memcpy(hash->block, &ctx, sizeof(ctx));
}

void
__wrap_jf_sha256_update(struct jf_sha256 *hash, const void *data, size_t size)
{
	if (!EVP_DigestUpdate(context(hash), data, size))
		abort();
}

void
__wrap_jf_sha256_hex(struct jf_sha256 *hash, char out[65])
{
	static const char digits[] = "0123456789abcdef";
	EVP_MD_CTX *ctx = context(hash);
	unsigned length = 0;
	unsigned char bytes[EVP_MAX_MD_SIZE];
	if (!EVP_DigestFinal_ex(ctx, bytes, &length) || length != 32)
		abort();
	EVP_MD_CTX_free(ctx);
	for (unsigned i = 0; i < length; i++) {
		out[2*i] = digits[bytes[i] >> 4];
		out[2*i+1] = digits[bytes[i] & 15];
	}
	out[64] = 0;
}
