#define _POSIX_C_SOURCE 200809L
#include "products.h"
#include "sha256.h"
#include "policy_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char *jf_kind_name(enum jf_kind);
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

static int number(struct jf_bytes *b, size_t n)
{
	char v[32]; snprintf(v,sizeof v,"%zu",n); return string(b,v);
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
	if (count>JF_DEPENDENCY_MAX || n<JF_BUILTIN_RULES || n>JF_RULE_RECORD_MAX || count>UINT32_MAX-n-1) goto memory;
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
	jf_digest(p.zsh.data,p.zsh.size,zd); jf_digest(p.sieve.data,p.sieve.size,sd);
	if (u32(&payload,(uint32_t)(1+count+n)) || jf_bytes_text(&payload,"M") || u32(&payload,7) ||
		string(&payload,JF_COMPILER_ID) || string(&payload,profile->id) || string(&payload,"posix-ere-c-icase-v1") ||
		string(&payload,"ascii-mailbox-casemap-v1") || string(&payload,"i;ascii-casemap") ||
		field(&payload,zd,32) || field(&payload,sd,32)) goto memory;
	for (i=0;i<count;i++) if (dependency(&payload,sorted[i])) goto memory;
	for (i=0;i<n;i++) {
		const struct jf_rule *r=jf_policy_rule(policy,i);
		if (jf_bytes_text(&payload,"R") || u32(&payload,12) || number(&payload,i+1) || number(&payload,r->stage) ||
			string(&payload,jf_kind_name(r->kind)) || number(&payload,r->fields) || string(&payload,r->id) ||
			string(&payload,r->predicate) || string(&payload,r->logical) || string(&payload,r->local) ||
			string(&payload,mailboxes[i]) || string(&payload,r->source) || number(&payload,r->line) || string(&payload,r->action)) goto memory;
	}
	jf_digest(payload.data,payload.size,pd);
	if (jf_cache_file_size(payload.size,NULL)) goto memory;
	if (jf_bytes_text(&p.cache,"JFNCACHE") || u32(&p.cache,1) || u32(&p.cache,1) || u64(&p.cache,payload.size) ||
		jf_bytes_add(&p.cache,p.fingerprint,32) || jf_bytes_add(&p.cache,pd,32) ||
		jf_bytes_add_limit(&p.cache,payload.data,payload.size,JF_CACHE_FILE_MAX)) goto memory;
	rc=jf_cache_validate(p.cache.data,p.cache.size,&p.zsh,&p.sieve,p.fingerprint,error);
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

static int safe_text(struct view v, int relative)
{
	size_t i,start=0;
	if (!v.size || !jf_valid_utf8(v.data,v.size) || (relative && v.data[0]=='/')) return 0;
	for (i=0;i<v.size;i++) if (v.data[i]<32 || v.data[i]==127) return 0;
	if (relative) for (i=0;i<=v.size;i++) if (i==v.size || v.data[i]=='/') {
		size_t n=i-start;
		if (!n || (n==1 && v.data[start]=='.') || (n==2 && !memcmp(v.data+start,"..",2))) return 0;
		start=i+1;
	}
	return 1;
}

static int compare(struct view a, struct view b)
{
	size_t n=a.size<b.size ? a.size : b.size;
	int c=n ? memcmp(a.data,b.data,n) : 0;
	return c ? c : a.size<b.size ? -1 : a.size>b.size;
}

static int decimal(struct view v, size_t *out)
{
	size_t n=0,i;
	if (!v.size || (v.size>1 && v.data[0]=='0')) return 0;
	for (i=0;i<v.size;i++) {
		unsigned d=v.data[i]-'0';
		if (d>9 || n>(SIZE_MAX-d)/10) return 0;
		n=n*10+d;
	}
	*out=n; return 1;
}

static int id(struct view v)
{
	size_t prefix,i;
	if (equal(v,"builtin:bounce") || equal(v,"builtin:spam") || equal(v,"builtin:unsorted")) return 1;
	for (prefix=0;prefix<v.size && v.data[prefix]!=':';prefix++);
	if (prefix==v.size || v.size-prefix!=65) return 0;
	if (!((prefix==1 && v.data[0]=='r') || (prefix==5 && !memcmp(v.data,"known",5)) ||
		(prefix==9 && !memcmp(v.data,"blacklist",9)) || (prefix==3 && !memcmp(v.data,"own",3)))) return 0;
	for (i=prefix+1;i<v.size;i++) if (!((v.data[i]>='0' && v.data[i]<='9') || (v.data[i]>='a' && v.data[i]<='f'))) return 0;
	return 1;
}

int jf_cache_validate(const unsigned char *data, size_t size, const struct jf_bytes *zsh,
	const struct jf_bytes *sieve, const unsigned char *fingerprint, struct jf_error *error)
{
	struct view previous_role={0}, previous_path={0};
	struct jf_sha256 hash;
	unsigned char digest[32]; char hex[65], fphex[65];
	size_t pos=92, i,j, ordinal=0, prior_stage=0, dependencies=0;
	uint32_t count;
	int phase=0, fallback=0, bounce=0, spam=0,regex_profile=0,qualification=0,catalogue=0,normalization=0;
	uint64_t payload=0;
	if (size<92 || jf_cache_file_size(size-JF_CACHE_HEADER_SIZE,NULL) || memcmp(data,"JFNCACHE",8) || read32(data+8)!=1 || read32(data+12)!=1) goto bad;
	for (i=0;i<8;i++) payload|=(uint64_t)data[16+i]<<(i*8);
	if (payload!=size-88 || (fingerprint && memcmp(fingerprint,data+24,32))) goto bad;
	jf_digest(data+88,size-88,digest); if (memcmp(digest,data+56,32)) goto bad;
	count=read32(data+88); if (count<1+JF_BUILTIN_RULES || count>JF_CACHE_RECORD_MAX) goto bad;
	jf_sha256_init(&hash); jf_sha256_update(&hash,"JFFP1",6);
	for (i=0;i<count;i++) {
		struct view f[12];
		size_t start=pos,n;
		unsigned char tag;
		uint32_t fields;
		if (size-pos<5) goto bad;
		tag=data[pos++]; fields=read32(data+pos); pos+=4;
		if (fields!=(tag=='M' ? 7u : tag=='D' ? 5u : tag=='R' ? 12u : 0u) || !fields) goto bad;
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
			if (zsh) { jf_digest(zsh->data,zsh->size,digest); if (memcmp(digest,f[5].data,32)) goto bad; }
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
		} else {
			size_t order,stage,mask,line;
			if (!phase || !decimal(f[0],&order) || order!=++ordinal || ordinal>JF_RULE_RECORD_MAX ||
				!decimal(f[1],&stage) || stage<1 || stage>8 || stage<prior_stage ||
				!decimal(f[3],&mask) || !decimal(f[10],&line) || !id(f[4]) ||
				!safe_text(f[6],1) || !safe_text(f[7],1) || !safe_text(f[8],0) || !equal(f[11],"move")) goto bad;
			phase=2; prior_stage=stage;
			if (equal(f[2],"address") || equal(f[2],"domain-apex") || equal(f[2],"domain-subdomains")) {
				if (!f[5].size || f[5].size>JF_LINE_MAX || !jf_valid_utf8(f[5].data,f[5].size) ||
					!((stage==1 && mask==1) || (stage==3 && mask==3) ||
					(stage==4 && mask==12) || (stage==5 && mask==1) || (stage==7 && mask==12))) goto bad;
				if (equal(f[2],"address")) {
					char address[JF_LINE_MAX+1]; size_t k;
					memcpy(address,f[5].data,f[5].size); address[f[5].size]=0;
					if (!jf_normalize_mailbox(address) || strlen(address)!=f[5].size || memcmp(address,f[5].data,f[5].size)) goto bad;
					for (k=0;k<f[5].size;k++) if (address[k]>='A' && address[k]<='Z') goto bad;
				} else {
					char domain[JF_LINE_MAX+1]; size_t k,skip=equal(f[2],"domain-subdomains") ? 2 : 0;
					if (!(stage==3 || stage==4 || stage==5) || f[5].size<=skip ||
						(skip && memcmp(f[5].data,"*.",2))) goto bad;
					memcpy(domain,f[5].data+skip,f[5].size-skip); domain[f[5].size-skip]=0;
					if (!jf_valid_domain(domain)) goto bad;
					for (k=0;domain[k];k++) if (domain[k]>='A' && domain[k]<='Z') goto bad;
				}
			} else if (equal(f[2],"ere")) {
				char pattern[JF_LINE_MAX+1];
				if (!regex_profile || !qualification || !catalogue || !f[5].size || f[5].size>JF_LINE_MAX ||
					!((stage==3 && mask==3) || (stage==4 && mask==12))) goto bad;
				memcpy(pattern,f[5].data,f[5].size); pattern[f[5].size]=0;
				if (!jf_regex_supported(pattern)) goto bad;
			} else if (equal(f[2],"sender-contains")) {
				if (stage!=2 || mask!=2 || bounce++ || !equal(f[4],"builtin:bounce") || !equal(f[5],"mailman-bounce")) goto bad;
			} else if (equal(f[2],"spam-is")) {
				if (stage!=6 || mask!=16 || spam++ || !equal(f[4],"builtin:spam") || !equal(f[5],"YES")) goto bad;
			} else if (equal(f[2],"always")) {
				if (stage!=8 || mask || fallback++ || i+1!=count || !equal(f[4],"builtin:unsorted") || f[5].size) goto bad;
			} else goto bad;
		}
	}
	if (pos!=size || bounce!=1 || spam!=1 || fallback!=1 || !normalization || (regex_profile && (!qualification || !catalogue))) goto bad;
	jf_sha256_hex(&hash,hex); jf_digest_hex(data+24,fphex);
	if (strcmp(hex,fphex)) goto bad;
	return 0;
bad:
	jf_product_error(error,"E_CACHE",0,"incompatible, noncanonical or corrupt generation product"); return 5;
}

int jf_cache_policy(const struct jf_bytes *cache, const struct jf_allocator *allocator,
	struct jf_policy **out, struct jf_error *error)
{
	struct jf_cached_rule *rules=NULL;
	size_t pos=92,i,j,n=0;
	int rc;
	if (!out) return 5;
	*out=NULL;
	if (!cache || (cache->size && !cache->data)) {
		jf_product_error(error,"E_CACHE",0,"missing cache bytes"); return 5;
	}
	if ((rc=jf_cache_validate(cache->data,cache->size,NULL,NULL,NULL,error))) return rc;
	/* Validation established complete framing, record counts and order. Keep
	 * views into the pinned bytes only until the shared policy owns its copies. */
	for (i=0;i<read32(cache->data+88);i++) {
		unsigned tag=cache->data[pos++], fields=read32(cache->data+pos); pos+=4;
		if (tag=='R') n++;
		for (j=0;j<fields;j++) { size_t size=read32(cache->data+pos); pos+=4+size; }
	}
	rules=malloc(n*sizeof *rules);
	if (!rules) { jf_product_error(error,"E_MEMORY",0,"cache rule views allocation failed"); return 3; }
	pos=92; n=0;
	for (i=0;i<read32(cache->data+88);i++) {
		unsigned tag=cache->data[pos++], fields=read32(cache->data+pos); pos+=4;
		for (j=0;j<fields;j++) {
			size_t size=read32(cache->data+pos); pos+=4;
			if (tag=='R') { rules[n].fields[j].data=cache->data+pos; rules[n].fields[j].size=size; }
			pos+=size;
		}
		if (tag=='R') n++;
	}
	rc=jf_policy_restore(rules,n,allocator,out,error); free(rules); return rc;
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
