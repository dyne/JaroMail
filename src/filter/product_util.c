#include "products.h"
#include "sha256.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void jf_bytes_free(struct jf_bytes *b)
{
	free(b->data); memset(b,0,sizeof *b);
}

int jf_bytes_fits(size_t used, size_t added, size_t limit)
{
	return used<=limit && added<=limit-used;
}

int jf_cache_file_size(size_t payload, size_t *total)
{
	if (payload>JF_CACHE_MAX || !jf_bytes_fits(JF_CACHE_HEADER_SIZE,payload,JF_CACHE_FILE_MAX)) return 3;
	if (total) *total=JF_CACHE_HEADER_SIZE+payload;
	return 0;
}

int jf_bytes_add_limit(struct jf_bytes *b, const void *data, size_t n, size_t limit)
{
	size_t cap;
	unsigned char *p;
	if (!jf_bytes_fits(b->size,n,limit)) return 3;
	if (b->size+n>b->capacity) {
		cap=b->capacity ? b->capacity : (limit<256 ? limit : 256);
		while (cap<b->size+n) cap=cap>limit/2 ? limit : cap*2;
		p=realloc(b->data,cap);
		if (!p) return 3;
		b->data=p; b->capacity=cap;
	}
	if (n) memcpy(b->data+b->size,data,n);
	b->size+=n; return 0;
}

int jf_bytes_add(struct jf_bytes *b, const void *data, size_t n)
{
	return jf_bytes_add_limit(b,data,n,JF_CACHE_MAX);
}

int jf_bytes_text(struct jf_bytes *b, const char *s)
{
	return jf_bytes_add(b,s,strlen(s));
}

void jf_digest_hex(const unsigned char digest[32], char out[65])
{
	static const char hex[]="0123456789abcdef";
	size_t i;
	for (i=0;i<32;i++) { out[i*2]=hex[digest[i]>>4]; out[i*2+1]=hex[digest[i]&15]; }
	out[64]=0;
}

void jf_digest(const void *data, size_t n, unsigned char out[32])
{
	struct jf_sha256 h;
	char hex[65];
	size_t i;
	jf_sha256_init(&h); jf_sha256_update(&h,data,n); jf_sha256_hex(&h,hex);
	for (i=0;i<32;i++) {
		unsigned a=(unsigned)(hex[i*2]<='9' ? hex[i*2]-'0' : hex[i*2]-'a'+10);
		unsigned b=(unsigned)(hex[i*2+1]<='9' ? hex[i*2+1]-'0' : hex[i*2+1]-'a'+10);
		out[i]=(unsigned char)((a<<4)|b);
	}
}

int jf_product_error(struct jf_error *e, const char *code, size_t line, const char *detail)
{
	if (e) { e->code=code; e->source=SIZE_MAX; e->line=line;
		snprintf(e->detail,sizeof e->detail,"%s",detail); }
	return !strcmp(code,"E_MEMORY") || !strcmp(code,"E_IO") ? 3 : 4;
}

void jf_products_free(struct jf_products *p)
{
	jf_bytes_free(&p->zsh); jf_bytes_free(&p->sieve); jf_bytes_free(&p->cache);
	memset(p,0,sizeof *p);
}
