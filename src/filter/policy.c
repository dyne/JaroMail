#define _POSIX_C_SOURCE 200809L
#include "policy_internal.h"
#include "sha256.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
	struct jf_policy *policy;
	struct jf_error *error;
	size_t source, line;
};

static int fail(struct parser *p, const char *code, const char *detail)
{
	if (p->error) {
		p->error->code = code;
		p->error->source = p->source;
		p->error->line = p->line;
		snprintf(p->error->detail, sizeof p->error->detail, "%s", detail);
	}
	return !strcmp(code,"E_MEMORY") ? 3 : 4;
}

static void *default_allocate(void *ctx, size_t size)
{
	(void)ctx;
	return malloc(size);
}

static void default_release(void *ctx, void *ptr)
{
	(void)ctx;
	free(ptr);
}

static void *allocate(struct jf_policy *p, size_t size)
{
	return p->allocator.allocate(p->allocator.context,size);
}

static void release(struct jf_policy *p, void *ptr)
{
	if (ptr) p->allocator.release(p->allocator.context,ptr);
}

static char *copy(struct jf_policy *p, const char *s)
{
	size_t n = strlen(s);
	char *v = allocate(p,n+1);
	if (v) memcpy(v,s,n+1);
	return v;
}

static void free_entry(struct jf_policy *p, struct jf_entry *e)
{
	struct jf_rule *r = &e->rule;
	if (e->compiled) regfree(&e->regex);
	release(p,(void *)r->selector); release(p,(void *)r->predicate);
	release(p,(void *)r->logical); release(p,(void *)r->local);
	release(p,(void *)r->sieve); release(p,(void *)r->source);
	release(p,(void *)r->source_identity);
}

void jf_policy_free(struct jf_policy *p)
{
	struct jf_allocator a;
	size_t i;
	if (!p) return;
	a = p->allocator;
	for (i=0; i<p->count; i++) free_entry(p,&p->entries[i]);
	release(p,p->index); release(p,p->legacy);
	release(p,p->entries);
	if (p->locale) freelocale(p->locale);
	a.release(a.context,p);
}

size_t jf_policy_count(const struct jf_policy *p)
{
	return p ? p->count : 0;
}

const struct jf_rule *jf_policy_rule(const struct jf_policy *p, size_t i)
{
	return p && i<p->count ? &p->entries[i].rule : NULL;
}

/* Reject overlong UTF8, surrogates, isolated continuations and > U+10FFFF. */
int jf_valid_utf8(const unsigned char *s, size_t n)
{
	size_t i=0;
	while (i<n) {
		uint32_t c=s[i++], min;
		unsigned need;
		if (!c) return 0;
		if (c<128) continue;
		if (c>=0xc2 && c<=0xdf) { need=1; min=0x80; c&=31; }
		else if (c>=0xe0 && c<=0xef) { need=2; min=0x800; c&=15; }
		else if (c>=0xf0 && c<=0xf4) { need=3; min=0x10000; c&=7; }
		else return 0;
		if (n-i<need) return 0;
		while (need--) {
			if ((s[i]&0xc0)!=0x80) return 0;
			c=(c<<6)|(s[i++]&63);
		}
		if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return 0;
	}
	return 1;
}

static int hex4(const unsigned char *s, size_t n, uint32_t *out)
{
	unsigned i;
	uint32_t v=0;
	if (n<4) return 0;
	for (i=0;i<4;i++) {
		unsigned c=s[i], d;
		if (c>='0' && c<='9') d=c-'0';
		else if (c>='a' && c<='f') d=c-'a'+10;
		else if (c>='A' && c<='F') d=c-'A'+10;
		else return 0;
		v=(v<<4)|d;
	}
	*out=v;
	return 1;
}

static char *encode(char *b, uint32_t c)
{
	if (c<0x80) *b++=(char)c;
	else if (c<0x800) { *b++=(char)(0xc0|(c>>6)); *b++=(char)(0x80|(c&63)); }
	else if (c<0x10000) {
		*b++=(char)(0xe0|(c>>12)); *b++=(char)(0x80|((c>>6)&63)); *b++=(char)(0x80|(c&63));
	} else {
		*b++=(char)(0xf0|(c>>18)); *b++=(char)(0x80|((c>>12)&63));
		*b++=(char)(0x80|((c>>6)&63)); *b++=(char)(0x80|(c&63));
	}
	return b;
}

/* Decoding never expands a line; b has n+1 bytes. At most five tokens. */
static int lex(struct parser *p, const unsigned char *s, size_t n, char *b,
	char *tokens[5], size_t *count)
{
	size_t i=0;
	*count=0;
	while (i<n) {
		while (i<n && (s[i]==' ' || s[i]=='\t')) i++;
		if (i==n || s[i]=='#') break;
		if (*count==5) return fail(p,"E_FIELDS","too many fields");
		tokens[(*count)++]=b;
		if (s[i]=='"') {
			int closed=0;
			i++;
			while (i<n) {
				uint32_t c=s[i++];
				if (c=='"') { closed=1; break; }
				if (c<32) return fail(p,"E_QUOTE","control byte in quoted token");
				if (c!='\\') { *b++=(char)c; continue; }
				if (i==n) return fail(p,"E_QUOTE","unfinished escape");
				c=s[i++];
				switch (c) {
				case '"': case '\\': case '/': *b++=(char)c; break;
				case 'b': *b++='\b'; break;
				case 'f': *b++='\f'; break;
				case 'n': *b++='\n'; break;
				case 'r': *b++='\r'; break;
				case 't': *b++='\t'; break;
				case 'u': {
					uint32_t low;
					if (!hex4(s+i,n-i,&c)) return fail(p,"E_QUOTE","invalid Unicode escape");
					i+=4;
					if (c>=0xd800 && c<=0xdbff) {
						if (n-i<6 || s[i]!='\\' || s[i+1]!='u' ||
							!hex4(s+i+2,n-i-2,&low) || low<0xdc00 || low>0xdfff)
							return fail(p,"E_QUOTE","unpaired Unicode surrogate");
						i+=6; c=0x10000+((c-0xd800)<<10)+(low-0xdc00);
					} else if (c>=0xdc00 && c<=0xdfff)
						return fail(p,"E_QUOTE","unpaired Unicode surrogate");
					if (!c) return fail(p,"E_NUL","decoded NUL is forbidden");
					b=encode(b,c); break;
				}
				default: return fail(p,"E_QUOTE","unknown JSON escape");
				}
			}
			if (!closed || (i<n && s[i]!=' ' && s[i]!='\t'))
				return fail(p,"E_QUOTE","quoted token requires a closing quote and separator");
		} else {
			while (i<n && s[i]!=' ' && s[i]!='\t') {
				if (s[i]<32 || s[i]==127 || s[i]=='"')
					return fail(p,"E_TOKEN","invalid bare token");
				*b++=(char)s[i++];
			}
		}
		*b++=0;
	}
	return 0;
}

static int alnum_ascii(unsigned c)
{
	return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9');
}

static void fold(char *s)
{
	for (;*s;s++) if (*s>='A' && *s<='Z') *s=(char)(*s+'a'-'A');
}

static int domain(const char *s)
{
	size_t n=strlen(s), label=0, i;
	if (!n || n>253) return 0;
	for (i=0;i<n;i++) {
		unsigned c=(unsigned char)s[i];
		if (c=='.') {
			if (!label || s[i-1]=='-') return 0;
			label=0;
		} else {
			if ((!alnum_ascii(c) && c!='-') || (!label && c=='-') || ++label>63) return 0;
		}
	}
	return label && s[n-1]!='-';
}

/* Policy inputs are single mailboxes, not display names/address lists. Quoted
 * local parts and SMTPUTF8 local bytes are preserved; domains are ASCII/A-label.
 */
int jf_valid_mailbox(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	if (*p=='"') {
		p++;
		while (*p && *p!='"') {
			if (*p=='\\') { p++; if (!*p) return 0; }
			if (*p<32 || *p==127) return 0;
			p++;
		}
		if (*p++!='"' || *p++!='@') return 0;
	} else {
		int component=0;
		while (*p && *p!='@') {
			if (*p=='.') { if (!component) return 0; component=0; }
			else {
				if (*p<128 && !alnum_ascii(*p) && !strchr("!#$%&'*+-/=?^_`{|}~",*p)) return 0;
				component=1;
			}
			p++;
		}
		if (!component || *p++!='@') return 0;
	}
	return domain((const char *)p);
}

static int destination(const char *s)
{
	const char *start=s, *p;
	if (!*s || *s=='/') return 0;
	for (p=s;;p++) {
		unsigned c=(unsigned char)*p;
		if (c && (c<32 || c==127)) return 0;
		if (!c || c=='/') {
			size_t n=(size_t)(p-start);
			if (!n || (n==1&&*start=='.') || (n==2&&start[0]=='.'&&start[1]=='.')) return 0;
			if (!c) break;
			start=p+1;
		}
	}
	return 1;
}

static int append(struct parser *p, const struct jf_source *s, const char *selector,
	char *predicate, const char *logical, enum jf_kind kind, unsigned stage, unsigned fields)
{
	struct jf_policy *pol=p->policy;
	struct jf_entry *e;
	struct jf_rule *r;
	if (pol->count==pol->capacity) {
		size_t cap=pol->capacity ? pol->capacity*2 : 32;
		struct jf_entry *v;
		if (cap<pol->capacity || cap>SIZE_MAX/sizeof *v) return fail(p,"E_MEMORY","policy allocation overflow");
		v=allocate(pol,cap*sizeof *v);
		if (!v) return fail(p,"E_MEMORY","policy allocation failed");
		if (pol->count) memcpy(v,pol->entries,pol->count*sizeof *v);
		release(pol,pol->entries); pol->entries=v; pol->capacity=cap;
	}
	e=&pol->entries[pol->count]; memset(e,0,sizeof *e);
	e->order=pol->count++; e->input_index=p->source;
	r=&e->rule;
	r->kind=kind; r->stage=stage; r->fields=fields; r->line=p->line; r->occurrence=1;
	r->action="move";
	r->selector=copy(pol,selector); r->predicate=copy(pol,predicate);
	r->logical=copy(pol,logical);
	r->local=copy(pol,!strcmp(selector,"spam") ? "zz.spam" : logical);
	r->sieve=copy(pol,!strcmp(logical,"known") ? "INBOX" : !strcmp(selector,"spam") ? "Spam" : logical);
	r->source=copy(pol,s ? s->path : ""); r->source_identity=copy(pol,s ? s->identity : "");
	if (!r->selector || !r->predicate || !r->logical || !r->local || !r->sieve ||
		!r->source || !r->source_identity) return fail(p,"E_MEMORY","rule allocation failed");
	return 0;
}

static int parse_line(struct parser *p, const struct jf_source *s,
	const unsigned char *line, size_t n, char *buffer)
{
	char *t[5];
	size_t count;
	int rc;
	if (s->kind==JF_KNOWN || s->kind==JF_BLACKLIST) {
		size_t start=0;
		while (start<n && (line[start]==' ' || line[start]=='\t')) start++;
		while (n>start && (line[n-1]==' ' || line[n-1]=='\t')) n--;
		if (start==n) return 0;
		memcpy(buffer,line+start,n-start); buffer[n-start]=0;
		if (!jf_valid_mailbox(buffer)) return fail(p,"E_ADDRESS","invalid exported mailbox");
		fold(buffer);
		return append(p,s,s->kind==JF_KNOWN ? "known" : "blacklist",buffer,
			s->kind==JF_KNOWN ? "known" : "zz.blacklist",JF_ADDRESS,
			s->kind==JF_KNOWN ? 5 : 1,JF_FROM);
	}
	if (s->kind==JF_ACCOUNT) {
		size_t i=0, start;
		while (i<n && (line[i]==' ' || line[i]=='\t')) i++;
		start=i;
		while (i<n && line[i]!=' ' && line[i]!='\t') i++;
		if (i-start!=5 || memcmp(line+start,"email",5)) return 0;
	}
	if ((rc=lex(p,line,n,buffer,t,&count)) || !count) return rc;
	if (s->kind!=JF_FILTERS) {
		char *address;
		if (count!=(s->kind==JF_ACCOUNT ? 2u : 1u)) return fail(p,"E_FIELDS","expected one mailbox");
		address=t[s->kind==JF_ACCOUNT ? 1 : 0];
		if (!jf_valid_mailbox(address)) return fail(p,"E_ADDRESS","invalid mailbox");
		fold(address);
		return append(p,s,"own",address,"priv",JF_ADDRESS,7,JF_TO|JF_CC);
	} else {
		enum jf_kind kind;
		unsigned stage, fields;
		int known=!strcmp(t[0],"known-domain"), is_from=!strncmp(t[0],"from",4);
		if (known || !strcmp(t[0],"from-domain") || !strcmp(t[0],"to-domain")) {
			kind=JF_DOMAIN_APEX;
		} else if (!strcmp(t[0],"from-address") || !strcmp(t[0],"to-address")) kind=JF_ADDRESS;
		else if (!strcmp(t[0],"from") || !strcmp(t[0],"to")) kind=JF_ERE;
		else return fail(p,"E_SELECTOR",t[0]);
		if (count!=(known ? 2u : 4u)) return fail(p,"E_FIELDS","wrong rule field count");
		if (!*t[1]) return fail(p,"E_PREDICATE","empty predicate");
		if (!known && strcmp(t[2],"move") && strcmp(t[2],"save")) return fail(p,"E_ACTION",t[2]);
		if (!known && !destination(t[3])) return fail(p,"E_DESTINATION","invalid relative destination");
		if (kind==JF_DOMAIN_APEX) {
			const char *d=t[1];
			if (!strncmp(d,"*.",2)) { kind=JF_DOMAIN_SUBDOMAINS; d+=2; }
			if (!domain(d)) return fail(p,"E_DOMAIN","invalid ASCII domain");
			fold(t[1]);
		} else if (kind==JF_ADDRESS) {
			if (!jf_valid_mailbox(t[1])) return fail(p,"E_ADDRESS","invalid exact mailbox");
			fold(t[1]);
		}
		stage=known ? 5 : is_from ? 3 : 4;
		fields=known ? JF_FROM : is_from ? JF_FROM|JF_SENDER : JF_TO|JF_CC;
		return append(p,s,t[0],t[1],known ? "known" : t[3],kind,stage,fields);
	}
}

struct source_ref { const struct jf_source *source; size_t index; };
static int compare_sources(const void *a, const void *b)
{
	const struct source_ref *x=a, *y=b;
	int c;
	if (x->source->kind!=y->source->kind) return x->source->kind<y->source->kind ? -1 : 1;
	c=strcmp(x->source->identity,y->source->identity);
	if (c) return c;
	return x->index<y->index ? -1 : x->index>y->index;
}

static int compare_key(const struct jf_entry *a, const struct jf_entry *b)
{
	int c=strcmp(a->rule.selector,b->rule.selector);
	return c ? c : strcmp(a->rule.predicate,b->rule.predicate);
}

static int compare_records(const void *a, const void *b)
{
	const struct jf_entry *x=*(struct jf_entry * const *)a, *y=*(struct jf_entry * const *)b;
	int c=compare_key(x,y);
	if (!c) c=strcmp(x->rule.logical,y->rule.logical);
	if (!c) c=strcmp(x->rule.source_identity,y->rule.source_identity);
	if (!c) c=x->order<y->order ? -1 : x->order>y->order;
	return c;
}

static int compare_keys(const void *a, const void *b)
{
	const struct jf_entry *x=*(struct jf_entry * const *)a, *y=*(struct jf_entry * const *)b;
	int c=compare_key(x,y);
	return c ? c : x->order<y->order ? -1 : x->order>y->order;
}

static int compare_order(const void *a, const void *b)
{
	const struct jf_entry *x=a, *y=b;
	if (x->rule.stage!=y->rule.stage) return x->rule.stage<y->rule.stage ? -1 : 1;
	if (x->rule.stage==5 && x->rule.kind!=y->rule.kind) {
		if (x->rule.kind==JF_ADDRESS) return -1;
		if (y->rule.kind==JF_ADDRESS) return 1;
	}
	return x->order<y->order ? -1 : x->order>y->order;
}

static void identify(struct jf_rule *r)
{
	struct jf_sha256 hash;
	char number[32], *out;
	const char *fields[6];
	unsigned i;
	jf_sha256_init(&hash);
	if (!strcmp(r->selector,"bounce") || !strcmp(r->selector,"spam") || !strcmp(r->selector,"unsorted")) {
		snprintf(r->id,sizeof r->id,"builtin:%s",r->selector); return;
	}
	if (!strcmp(r->selector,"known") || !strcmp(r->selector,"blacklist") || !strcmp(r->selector,"own")) {
		snprintf(r->id,sizeof r->id,"%s:",r->selector);
		out=r->id+strlen(r->id);
		jf_sha256_update(&hash,r->predicate,strlen(r->predicate));
	} else {
		strcpy(r->id,"r:"); out=r->id+2;
		snprintf(number,sizeof number,"%zu",r->occurrence);
		fields[0]=r->selector; fields[1]=r->predicate; fields[2]=r->action;
		fields[3]=r->logical; fields[4]=r->source_identity; fields[5]=number;
		for (i=0;i<6;i++) jf_sha256_update(&hash,fields[i],strlen(fields[i])+(i<5));
	}
	jf_sha256_hex(&hash,out);
}

static int resolve(struct parser *p)
{
	struct jf_policy *pol=p->policy;
	struct jf_entry **refs;
	size_t i, j, kept;
	if (pol->count>SIZE_MAX/sizeof *refs) return fail(p,"E_MEMORY","index allocation overflow");
	refs=allocate(pol,pol->count*sizeof *refs);
	if (!refs) return fail(p,"E_MEMORY","index allocation failed");
	for (i=0;i<pol->count;i++) refs[i]=&pol->entries[i];
	qsort(refs,pol->count,sizeof *refs,compare_records);
	for (i=0;i<pol->count;i=j) {
		for (j=i+1;j<pol->count && !compare_key(refs[i],refs[j]) &&
			!strcmp(refs[i]->rule.logical,refs[j]->rule.logical) &&
			!strcmp(refs[i]->rule.source_identity,refs[j]->rule.source_identity);j++)
			refs[j]->rule.occurrence=j-i+1;
	}
	qsort(refs,pol->count,sizeof *refs,compare_keys);
	for (i=0;i<pol->count;i=j) {
		int contact=!strcmp(refs[i]->rule.selector,"own") || !strcmp(refs[i]->rule.selector,"known") ||
			!strcmp(refs[i]->rule.selector,"blacklist");
		for (j=i+1;j<pol->count && !compare_key(refs[i],refs[j]);j++) {}
		for (kept=i;kept<j;kept++) if (kept!=(contact ? i : j-1)) refs[kept]->discard=1;
	}
	release(pol,refs);
	for (i=0,kept=0;i<pol->count;i++) {
		if (pol->entries[i].discard) free_entry(pol,&pol->entries[i]);
		else {
			if (kept!=i) pol->entries[kept]=pol->entries[i];
			kept++;
		}
	}
	pol->count=kept;
	if (kept-3>JF_RULE_MAX) {
		p->source=pol->entries[JF_RULE_MAX].input_index;
		p->line=pol->entries[JF_RULE_MAX].rule.line;
		return fail(p,"E_RULE_LIMIT","too many effective rules/contact predicates");
	}
	qsort(pol->entries,pol->count,sizeof *pol->entries,compare_order);
	for (i=0;i<pol->count;i++) {
		struct jf_entry *e=&pol->entries[i];
		identify(&e->rule);
		if (e->rule.kind==JF_ERE) {
			locale_t old=uselocale(pol->locale);
			int rc;
			if (!old) return fail(p,"E_MEMORY","cannot select C regex locale");
			rc=regcomp(&e->regex,e->rule.predicate,REG_EXTENDED|REG_ICASE|REG_NOSUB);
			uselocale(old);
			if (rc) {
				char detail[160];
				p->source=e->input_index; p->line=e->rule.line;
				regerror(rc,&e->regex,detail,sizeof detail);
				return fail(p,rc==REG_ESPACE ? "E_MEMORY" : "E_REGEX",detail);
			}
			e->compiled=1;
		}
	}
	return 0;
}

int jf_policy_parse(const struct jf_source *sources, size_t count,
	const struct jf_allocator *allocator, struct jf_policy **out, struct jf_error *error)
{
	struct jf_allocator a={NULL,default_allocate,default_release};
	struct jf_policy *pol;
	struct source_ref *refs=NULL;
	struct parser p;
	size_t i;
	int rc=0;
	if (error) memset(error,0,sizeof *error);
	if (!out) return 4;
	*out=NULL;
	if (allocator) a=*allocator;
	p.policy=NULL; p.error=error; p.source=SIZE_MAX; p.line=0;
	if (!a.allocate || !a.release || (count && !sources) || count>SIZE_MAX/sizeof *refs)
		return fail(&p,"E_ARGUMENT","invalid snapshot/allocator");
	pol=a.allocate(a.context,sizeof *pol);
	if (!pol) return fail(&p,"E_MEMORY","policy allocation failed");
	memset(pol,0,sizeof *pol); pol->allocator=a; p.policy=pol;
	pol->locale=newlocale(LC_ALL_MASK,"C",(locale_t)0);
	if (!pol->locale) { rc=fail(&p,"E_MEMORY","cannot create C regex locale"); goto done; }
	if (count) {
		refs=allocate(pol,count*sizeof *refs);
		if (!refs) { rc=fail(&p,"E_MEMORY","source index allocation failed"); goto done; }
	}
	for (i=0;i<count;i++) {
		const struct jf_source *s=&sources[i];
		p.source=i;
		if (s->kind<JF_FILTERS || s->kind>JF_BLACKLIST || !s->identity || !*s->identity || !s->path ||
			(s->size && !s->data)) { rc=fail(&p,"E_ARGUMENT","invalid source"); goto done; }
		if (s->size>JF_INPUT_MAX) { rc=fail(&p,"E_INPUT_LIMIT","policy input exceeds 64 MiB"); goto done; }
		refs[i].source=s; refs[i].index=i;
	}
	if (count) qsort(refs,count,sizeof *refs,compare_sources);
	for (i=0;i<count;i++) {
		const struct jf_source *s=refs[i].source;
		size_t pos=0;
		char *buffer=allocate(pol,JF_LINE_MAX+1);
		p.source=refs[i].index; p.line=0;
		if (!buffer) { rc=fail(&p,"E_MEMORY","line allocation failed"); goto done; }
		while (pos<s->size) {
			size_t start=pos, n;
			while (pos<s->size && s->data[pos]!='\n') pos++;
			n=pos-start; if (pos<s->size) pos++;
			p.line++;
			if (n && s->data[start+n-1]=='\r') n--;
			if (n>JF_LINE_MAX) { rc=fail(&p,"E_LINE_LIMIT","policy line exceeds 64 KiB"); break; }
			if (!jf_valid_utf8(s->data+start,n)) { rc=fail(&p,"E_UTF8","invalid UTF8 or NUL input"); break; }
			rc=parse_line(&p,s,s->data+start,n,buffer);
			if (rc) break;
		}
		release(pol,buffer);
		if (rc) goto done;
	}
	p.source=SIZE_MAX; p.line=0;
	if ((rc=append(&p,NULL,"bounce","mailman-bounce","zz.bounces",JF_SENDER_CONTAINS,2,JF_SENDER)) ||
		(rc=append(&p,NULL,"spam","YES","spam",JF_SPAM_IS,6,JF_SPAM)) ||
		(rc=append(&p,NULL,"unsorted","","unsorted",JF_ALWAYS,8,0))) goto done;
	rc=resolve(&p);
	if (!rc) rc=jf_policy_build_index(pol,error);
done:
	release(pol,refs);
	if (rc) jf_policy_free(pol);
	else *out=pol;
	return rc;
}
