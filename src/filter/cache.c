#define _POSIX_C_SOURCE 200809L
#include "products.h"
#include "sha256.h"
#include "policy_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int jf_emit_text(const struct jf_policy *, const struct jf_profile *, char *const *,
	struct jf_products *, struct jf_error *);

static int u32(struct jf_bytes *b, uint32_t n)
{
	unsigned char v[4]; unsigned i;
	for (i=0;i<4;i++) v[i]=(unsigned char)(n>>(i*8));
	return jf_bytes_add(b,v,4);
}

static int u64(struct jf_bytes *b, uint64_t n)
{
	unsigned char v[8]; unsigned i;
	for (i=0;i<8;i++) v[i]=(unsigned char)(n>>(i*8));
	return jf_bytes_add(b,v,8);
}

static int field(struct jf_bytes *b, const void *s, size_t n)
{
	return n>UINT32_MAX || u32(b,(uint32_t)n) || jf_bytes_add(b,s,n);
}

static int string(struct jf_bytes *b, const char *s)
{
	return field(b,s,strlen(s));
}

static int dependency(struct jf_bytes *b, const struct jf_dependency *d)
{
	return jf_bytes_text(b,"D") || u32(b,5) || string(b,d->role) || string(b,d->path) ||
		string(b,d->present ? "1" : "0") || field(b,d->digest,d->present ? 32 : 0) ||
		field(b,d->auxiliary.data,d->auxiliary.size);
}

static int dep_compare(const void *a, const void *b)
{
	const struct jf_dependency *x=*(const struct jf_dependency *const *)a;
	const struct jf_dependency *y=*(const struct jf_dependency *const *)b;
	int c=strcmp(x->role,y->role); return c ? c : strcmp(x->path,y->path);
}

int jf_dependencies_fingerprint(const struct jf_dependency *deps, size_t count, unsigned char out[32])
{
	const struct jf_dependency **sorted=NULL;
	struct jf_bytes b={0};
	size_t i;
	int rc=3;
	if (count>JF_DEPENDENCY_MAX) return 3;
	sorted=malloc((count ? count : 1)*sizeof *sorted); if (!sorted) return 3;
	for (i=0;i<count;i++) sorted[i]=&deps[i];
	qsort(sorted,count,sizeof *sorted,dep_compare);
	if (jf_bytes_add(&b,"JFFP1",6)) goto done;
	for (i=0;i<count;i++) {
		if (!*sorted[i]->role || (i && !dep_compare(&sorted[i-1],&sorted[i]))) { rc=4; goto done; }
		if (dependency(&b,sorted[i])) goto done;
	}
	jf_digest(b.data,b.size,out); rc=0;
done:
	free(sorted); jf_bytes_free(&b); return rc;
}

struct mailbox_ref { const char *mailbox, *logical; };
static int mailbox_compare(const void *a, const void *b)
{
	const struct mailbox_ref *x=a,*y=b;
	int c=strcmp(x->mailbox,y->mailbox);
	return c ? c : strcmp(x->logical,y->logical);
}

int jf_products_build(const struct jf_policy *policy, const struct jf_profile *profile,
	const struct jf_dependency *deps, size_t count, struct jf_products *out, struct jf_error *error)
{
	struct jf_products p={0};
	struct jf_bytes payload={0}, inventory={0};
	const struct jf_dependency **sorted=NULL;
	unsigned char zd[32], sd[32], pd[32];
	char **mailboxes=NULL;
	struct mailbox_ref *mapping=NULL;
	size_t i, n=jf_policy_count(policy);
	int rc=3;
	memset(out,0,sizeof *out);
	if (error) memset(error,0,sizeof *error);
	if (count>JF_DEPENDENCY_MAX || n<JF_BUILTIN_RULES || n>JF_RULE_RECORD_MAX) goto memory;
	mailboxes=calloc(n ? n : 1,sizeof *mailboxes);
	sorted=malloc((count ? count : 1)*sizeof *sorted);
	mapping=malloc((n ? n : 1)*sizeof *mapping);
	if (!mailboxes || !sorted || !mapping) goto memory;
	for (i=0;i<n;i++) {
		if (!(mailboxes[i]=jf_profile_mailbox(profile,jf_policy_rule(policy,i)))) goto memory;
		mapping[i].mailbox=mailboxes[i]; mapping[i].logical=jf_policy_rule(policy,i)->logical;
	}
	qsort(mapping,n,sizeof *mapping,mailbox_compare);
	for (i=1;i<n;i++) if (!strcmp(mapping[i-1].mailbox,mapping[i].mailbox) && strcmp(mapping[i-1].logical,mapping[i].logical)) {
		rc=jf_product_error(error,"E_MAILBOX_MAPPING",0,"different logical destinations map to the same Sieve mailbox"); goto done;
	}
	if ((rc=jf_emit_text(policy,profile,mailboxes,&p,error))) goto done;
	for (i=0;i<count;i++) sorted[i]=&deps[i];
	qsort(sorted,count,sizeof *sorted,dep_compare);
	if (jf_bytes_add(&inventory,"JFFP1",6)) goto memory;
	for (i=0;i<count;i++) {
		if (!*sorted[i]->role || (i && !dep_compare(&sorted[i-1],&sorted[i]))) {
			rc=jf_product_error(error,"E_DEPENDENCY",0,"empty or duplicate dependency key"); goto done;
		}
		if (dependency(&inventory,sorted[i])) goto memory;
	}
	jf_digest(inventory.data,inventory.size,p.fingerprint); jf_digest_hex(p.fingerprint,p.id);
	jf_digest(p.provenance.data,p.provenance.size,zd); jf_digest(p.sieve.data,p.sieve.size,sd);
	if (u32(&payload,(uint32_t)(1+count)) || jf_bytes_text(&payload,"M") || u32(&payload,7) ||
		string(&payload,JF_COMPILER_ID) || string(&payload,profile->id) || string(&payload,"posix-ere-c-icase-v1") ||
		string(&payload,"ascii-mailbox-casemap-v1") || string(&payload,"i;ascii-casemap") ||
		field(&payload,zd,32) || field(&payload,sd,32)) goto memory;
	for (i=0;i<count;i++) if (dependency(&payload,sorted[i])) goto memory;
	jf_digest(payload.data,payload.size,pd);
	if (jf_cache_file_size(payload.size,NULL)) goto memory;
	if (jf_bytes_text(&p.cache,"JFDEPS01") || u32(&p.cache,1) || u32(&p.cache,1) || u64(&p.cache,payload.size) ||
		jf_bytes_add(&p.cache,p.fingerprint,32) || jf_bytes_add(&p.cache,pd,32) ||
		jf_bytes_add_limit(&p.cache,payload.data,payload.size,JF_CACHE_FILE_MAX)) goto memory;
	rc=jf_cache_validate(p.cache.data,p.cache.size,&p.provenance,&p.sieve,p.fingerprint,NULL,error);
	if (!rc) { *out=p; memset(&p,0,sizeof p); }
	goto done;
memory:
	rc=jf_product_error(error,"E_MEMORY",0,"cache allocation/size limit");
done:
	for (i=0;mailboxes && i<n;i++) free(mailboxes[i]);
	free(mailboxes); free(mapping); free(sorted); jf_bytes_free(&payload); jf_bytes_free(&inventory); jf_products_free(&p);
	return rc;
}

static uint32_t read32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

struct view { const unsigned char *data; size_t size; };

static int equal(struct view v, const char *s)
{
	return v.size==strlen(s) && !memcmp(v.data,s,v.size);
}

static int prefix(struct view v, const char *s)
{
	size_t n=strlen(s);
	return v.size>=n && !memcmp(v.data,s,n);
}

static int compare(struct view a, struct view b)
{
	size_t n=a.size<b.size ? a.size : b.size;
	int c=n ? memcmp(a.data,b.data,n) : 0;
	return c ? c : a.size<b.size ? -1 : a.size>b.size;
}

int jf_cache_validate(const unsigned char *data, size_t size, const struct jf_bytes *provenance,
	const struct jf_bytes *sieve, const unsigned char *fingerprint, struct jf_sieve_binding *binding, struct jf_error *error)
{
	struct view previous_role={0}, previous_path={0};
	struct jf_sha256 hash;
	unsigned char digest[32]; char hex[65], fphex[65];
	size_t pos=92, i,j, dependencies=0;
	uint32_t count;
	int phase=0, regex_profile=0,qualification=0,catalogue=0,normalization=0;
	uint64_t payload=0;
	if (binding) memset(binding,0,sizeof *binding);
	if (size<92 || jf_cache_file_size(size-JF_CACHE_HEADER_SIZE,NULL) || memcmp(data,"JFDEPS01",8) || read32(data+8)!=1 || read32(data+12)!=1) goto bad;
	for (i=0;i<8;i++) payload|=(uint64_t)data[16+i]<<(i*8);
	if (payload!=size-88 || (fingerprint && memcmp(fingerprint,data+24,32))) goto bad;
	jf_digest(data+88,size-88,digest); if (memcmp(digest,data+56,32)) goto bad;
	count=read32(data+88); if (count<1 || count>JF_CACHE_RECORD_MAX) goto bad;
	jf_sha256_init(&hash); jf_sha256_update(&hash,"JFFP1",6);
	for (i=0;i<count;i++) {
		struct view f[12];
		size_t start=pos,n;
		unsigned char tag;
		uint32_t fields;
		if (size-pos<5) goto bad;
		tag=data[pos++]; fields=read32(data+pos); pos+=4;
		if (fields!=(tag=='M' ? 7u : tag=='D' ? 5u : 0u) || !fields) goto bad;
		for (j=0;j<fields;j++) {
			if (size-pos<4) goto bad;
			n=read32(data+pos); pos+=4;
			if (n>size-pos) goto bad;
			f[j].data=data+pos; f[j].size=n; pos+=n;
			if (!(tag=='D' && j>=3) && !(tag=='M' && j>=5) && memchr(f[j].data,0,n)) goto bad;
		}
		if (tag=='M') {
			if (i || phase || !equal(f[0],JF_COMPILER_ID) ||
				(!equal(f[1],"core-v1") && !equal(f[1],"pigeonhole-2.4.5-posix-ere-v1")) ||
				!equal(f[2],"posix-ere-c-icase-v1") || !equal(f[3],"ascii-mailbox-casemap-v1") ||
				!equal(f[4],"i;ascii-casemap") || f[5].size!=32 || f[6].size!=32) goto bad;
			if (provenance) { jf_digest(provenance->data,provenance->size,digest); if (memcmp(digest,f[5].data,32)) goto bad; }
			if (sieve) { jf_digest(sieve->data,sieve->size,digest); if (memcmp(digest,f[6].data,32)) goto bad; }
			phase=1;
			regex_profile=equal(f[1],"pigeonhole-2.4.5-posix-ere-v1");
		} else if (tag=='D') {
			int c=compare(previous_role,f[0]);
			if (++dependencies>JF_DEPENDENCY_MAX || phase!=1 || !f[0].size || (previous_role.size && (c>0 || (!c && compare(previous_path,f[1])>=0))) ||
				!((equal(f[2],"1") && f[3].size==32) || (equal(f[2],"0") && !f[3].size))) goto bad;
			if (equal(f[2],"0") && f[4].size) goto bad;
			if (equal(f[2],"1") && (prefix(f[0],"config.") || prefix(f[0],"export.") ||
				prefix(f[0],"link.") || prefix(f[0],"inventory."))) {
				jf_digest(f[4].data,f[4].size,digest); if (memcmp(digest,f[3].data,32)) goto bad;
			}
			previous_role=f[0]; previous_path=f[1];
			if (binding && f[3].size==32) {
				if (equal(f[0],"input.sieve-runtime")) jf_digest_hex(f[3].data,binding->runtime);
				if (equal(f[0],"config.profile")) jf_digest_hex(f[3].data,binding->profile);
			}
			if (equal(f[0],"config.address-normalization")) {
				if (f[1].size || !equal(f[2],"1") || !equal(f[4],JF_ADDRESS_NORMALIZATION)) goto bad;
				normalization=1;
			}
			if (equal(f[0],"config.regex-qualification")) {
				if (f[1].size || !equal(f[2],"1") || (regex_profile && !equal(f[4],jf_regex_qualification()))) goto bad;
				qualification=1;
			}
			if (equal(f[0],"config.regex-catalogue")) {
				if (f[1].size || !equal(f[2],"1") || (regex_profile && !equal(f[4],jf_regex_catalogue()))) goto bad;
				catalogue=1;
			}
			jf_sha256_update(&hash,data+start,pos-start);
		}
	}
	if (pos!=size || !normalization || (regex_profile && (!qualification || !catalogue))) goto bad;
	jf_sha256_hex(&hash,hex); jf_digest_hex(data+24,fphex);
	if (strcmp(hex,fphex)) goto bad;
	return 0;
bad:
	jf_product_error(error,"E_CACHE",0,"incompatible, noncanonical or corrupt generation product"); return 5;
}

int jf_cache_dependencies(const struct jf_bytes *cache, struct jf_dependency **out, size_t *count)
{
	size_t pos=92,i,j,n=0;
	struct jf_dependency *deps=NULL;
	int rc=5;
	*out=NULL; *count=0;
	if (cache->size<92) return 5;
	for (i=0;i<read32(cache->data+88);i++) {
		struct view f[12];
		unsigned tag,fields;
		if (cache->size-pos<5) goto done;
		tag=cache->data[pos++]; fields=read32(cache->data+pos); pos+=4;
		if (fields>12) goto done;
		for (j=0;j<fields;j++) {
			size_t size;
			if (cache->size-pos<4) goto done;
			size=read32(cache->data+pos); pos+=4;
			if (size>cache->size-pos) goto done;
			f[j].data=cache->data+pos; f[j].size=size; pos+=size;
		}
		if (tag=='D') {
			struct jf_dependency *v,*d;
			if (fields!=5 || n==JF_DEPENDENCY_MAX || f[3].size>32) goto done;
			v=realloc(deps,(n+1)*sizeof *v); if (!v) { rc=3; goto done; }
			deps=v; d=&deps[n++]; memset(d,0,sizeof *d);
			d->role=malloc(f[0].size+1); d->path=malloc(f[1].size+1);
			if (!d->role || !d->path || jf_bytes_add(&d->auxiliary,f[4].data,f[4].size)) { rc=3; goto done; }
			memcpy(d->role,f[0].data,f[0].size); d->role[f[0].size]=0;
			memcpy(d->path,f[1].data,f[1].size); d->path[f[1].size]=0;
			d->present=equal(f[2],"1"); memcpy(d->digest,f[3].data,f[3].size);
		}
	}
	if (pos!=cache->size) goto done;
	*out=deps; *count=n; return 0;
done:
	for (i=0;i<n;i++) { free(deps[i].role); free(deps[i].path); jf_bytes_free(&deps[i].auxiliary); }
	free(deps); return rc;
}
