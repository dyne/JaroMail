#define _POSIX_C_SOURCE 200809L
#include "import.h"
#include "policy_internal.h"
#include "headers.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

const char *const jf_import_names[JF_IMPORT_FILES]={"Filters.txt","Aliases.txt","whitelist.abook","blacklist.abook"};
enum { WORD=256, STRING };
struct parser {
	const unsigned char *data; size_t size,pos,line,token_line,rule_line,rules;
	int token,rc,legacy,create; unsigned capabilities,used,builtins[9]; size_t routes;
	char *value;
	struct jf_error *error;
	const struct jf_profile *profile;
	struct jf_bytes recovered[JF_IMPORT_FILES];
	size_t *source_lines[JF_IMPORT_FILES],line_counts[JF_IMPORT_FILES];
};
struct strings { char **items; size_t count; };

static int invalid(struct parser *p,const char *detail)
{
	if (!p->rc) { jf_product_error(p->error,"E_SIEVE_IMPORT",p->token_line,detail); p->rc=4; }
	return p->rc;
}

static int memory(struct parser *p)
{
	if (!p->rc) { jf_product_error(p->error,"E_MEMORY",p->token_line,"import allocation/size limit"); p->rc=3; }
	return p->rc;
}

static int next(struct parser *p)
{
	struct jf_bytes b={0}; unsigned char c;
	free(p->value); p->value=NULL;
	if (p->rc) return p->rc;
	for (;;) {
		if (p->pos==p->size) { p->token=0; p->token_line=p->line; return 0; }
		c=p->data[p->pos];
		if (c==' ' || c=='\t' || c=='\r' || c=='\n') { p->pos++; if (c=='\n') p->line++; continue; }
		if (c=='#') { while (p->pos<p->size && p->data[p->pos]!='\n') p->pos++; continue; }
		if (c=='/' && p->pos+1<p->size && p->data[p->pos+1]=='*') {
			p->pos+=2;
			while (p->pos+1<p->size && !(p->data[p->pos]=='*' && p->data[p->pos+1]=='/')) {
				if (p->data[p->pos++]=='\n') p->line++;
			}
			if (p->pos+1>=p->size) { p->token_line=p->line; return invalid(p,"unterminated block comment"); }
			p->pos+=2; continue;
		}
		break;
	}
	p->token_line=p->line; p->pos++;
	if (strchr("{}[],;",c)) { p->token=c; return 0; }
	if (c=='"') {
		p->token=STRING;
		for (;;) {
			if (p->pos==p->size) { invalid(p,"unterminated quoted string"); goto done; }
			c=p->data[p->pos++];
			if (c=='"') break;
			if (c=='\\') {
				if (p->pos==p->size) { invalid(p,"truncated quoted pair"); goto done; }
				c=p->data[p->pos++];
				/* Other quoted pairs have implementation-dependent meaning. */
				if (c!='"' && c!='\\') { invalid(p,"unsupported quoted pair; use quote/backslash escaping"); goto done; }
			}
			if (c<32 || c==127) { invalid(p,"control byte or multiline string unsupported"); goto done; }
			if (jf_bytes_add_limit(&b,&c,1,JF_LINE_MAX)) { memory(p); goto done; }
		}
	} else {
		p->token=WORD;
		if (jf_bytes_add_limit(&b,&c,1,JF_LINE_MAX)) { memory(p); goto done; }
		while (p->pos<p->size) {
			c=p->data[p->pos];
			if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_')) break;
			p->pos++;
			if (jf_bytes_add_limit(&b,&c,1,JF_LINE_MAX)) { memory(p); goto done; }
		}
	}
	if (jf_bytes_add_limit(&b,"",1,JF_LINE_MAX+1u)) { memory(p); goto done; }
	p->value=(char *)b.data; memset(&b,0,sizeof b);
done:
	jf_bytes_free(&b); return p->rc;
}

static int is(struct parser *p,const char *word)
{
	return p->token==WORD && !strcasecmp(p->value,word);
}

static int take(struct parser *p,int token)
{
	if (p->rc) return p->rc;
	if (p->token!=token) return invalid(p,"unexpected token or truncated generated script");
	return next(p);
}

static void strings_free(struct strings *s)
{
	size_t i; for (i=0;i<s->count;i++) free(s->items[i]); free(s->items); memset(s,0,sizeof *s);
}

static int strings(struct parser *p,struct strings *s)
{
	int list=p->token=='[';
	if (list && next(p)) return p->rc;
	for (;;) {
		char **v;
		if (p->token!=STRING || s->count==JF_RULE_MAX) return invalid(p,"expected nonempty bounded string/list");
		v=realloc(s->items,(s->count+1)*sizeof *v); if (!v) return memory(p);
		s->items=v; v[s->count]=strdup(p->value); if (!v[s->count]) return memory(p);
		s->count++;
		if (next(p)) return p->rc;
		if (!list) return 0;
		if (p->token==']') return next(p);
		if (take(p,',')) return p->rc;
	}
}

/* JSON quoting is the policy lexer format, distinct from Sieve quoted pairs. */
static int quote(struct jf_bytes *b,const char *s)
{
	if (jf_bytes_text(b,"\"")) return 3;
	for (;*s;s++) {
		if ((*s=='"' || *s=='\\') && jf_bytes_text(b,"\\")) return 3;
		if ((unsigned char)*s<32 || jf_bytes_add(b,s,1)) return 3;
	}
	return jf_bytes_text(b,"\"");
}

static char *mapped(const struct jf_profile *profile,const char *logical,enum jf_kind kind)
{
	struct jf_rule r={0}; r.logical=logical; r.kind=kind; return jf_profile_mailbox(profile,&r);
}

static int mapping_is(struct parser *p,const char *mailbox,const char *logical,enum jf_kind kind)
{
	char *value=mapped(p->profile,logical,kind); int equal;
	if (!value) { memory(p); return 0; }
	equal=!strcmp(value,mailbox); free(value); return equal;
}

static char *unmap(struct parser *p,const char *mailbox)
{
	const struct jf_profile *profile=p->profile; size_t i,n=strlen(profile->prefix),sep=strlen(profile->separator);
	char *result=NULL; struct jf_bytes b={0}; const char *s;
	/* Resolve configured mappings, then check for a second named inverse. */
	for (i=0;i<profile->count+1;i++) {
		const char *key=i<profile->count ? profile->mappings[i].logical : "known";
		if (mapping_is(p,mailbox,key,JF_ADDRESS)) {
			if (result && strcmp(result,key)) { invalid(p,"ambiguous mailbox mappings"); goto fail; }
			if (!result && !(result=strdup(key))) { memory(p); goto fail; }
		}
	}
	if (p->rc) goto fail;
	if (!sep || strncmp(mailbox,profile->prefix,n)) {
		if (result) return result;
		invalid(p,"mailbox outside configured namespace"); goto fail;
	}
	/* A non-slash separator can also occur literally in a local component.
	 * Without an explicit mapping there is no unique inverse for that case. */
	if (strcmp(profile->separator,"/") && strstr(mailbox+n,profile->separator)) {
		if (result) return result;
		invalid(p,"ambiguous namespace separator; declare an explicit mailbox mapping"); goto fail;
	}
	for (s=mailbox+n;*s;) {
		if (!strncmp(s,profile->separator,sep)) { if (jf_bytes_text(&b,"/")) goto oom; s+=sep; }
		else { if (jf_bytes_add(&b,s,1)) goto oom; s++; }
	}
	if (jf_bytes_add(&b,"",1)) goto oom;
	if (result) {
		char *inverse=mapped(profile,(char *)b.data,JF_ADDRESS);
		if (!inverse) goto oom;
		if (!strcmp(inverse,mailbox) && strcmp(result,(char *)b.data)) {
			free(inverse); invalid(p,"ambiguous mapped and named mailbox destinations"); goto fail;
		}
		free(inverse); jf_bytes_free(&b); return result;
	}
	return (char *)b.data;
oom:
	memory(p);
fail:
	free(result); jf_bytes_free(&b); return NULL;
}

static int record(struct parser *p,unsigned stage,const char *selector,const char *key,const char *destination)
{
	unsigned file=stage==1 ? 3 : stage==7 ? 1 : stage==5 && !strcmp(selector,"address") ? 2 : 0;
	struct jf_bytes *b=&p->recovered[file]; size_t *lines;
	if (++p->rules>JF_RULE_MAX) return invalid(p,"too many imported rules");
	lines=realloc(p->source_lines[file],(p->line_counts[file]+1)*sizeof *lines);
	if (!lines) return memory(p);
	p->source_lines[file]=lines; lines[p->line_counts[file]++]=p->rule_line;
	if (b==&p->recovered[0]) {
		p->routes++;
		if (jf_bytes_text(b,selector) || jf_bytes_text(b," ") || quote(b,key) ||
			(destination && (jf_bytes_text(b," move ") || quote(b,destination))) || jf_bytes_text(b,"\n")) return memory(p);
	} else if ((stage==7 ? quote(b,key) : jf_bytes_text(b,key)) || jf_bytes_text(b,"\n")) return memory(p);
	return 0;
}

static int contact(struct parser *p,unsigned stage,const char *raw)
{
	struct jf_bytes wire={0}; struct jf_headers *headers=NULL; struct jf_error e={0};
	const struct jf_message *m; int rc=4; size_t i;
	if (jf_bytes_text(&wire,"From: ") || jf_bytes_text(&wire,raw) || jf_bytes_text(&wire,"\n\n")) { memory(p); goto done; }
	if (jf_headers_parse(wire.data,wire.size,NULL,&headers,&e)) { invalid(p,"legacy contact is not a representable mailbox"); goto done; }
	m=jf_headers_message(headers);
	if (!m->address_count) { invalid(p,"empty legacy contact"); goto done; }
	for (i=0;i<m->address_count;i++) {
		char *key=malloc(m->addresses[i].value.size+1);
		if (!key) { memory(p); goto done; }
		memcpy(key,m->addresses[i].value.data,m->addresses[i].value.size); key[m->addresses[i].value.size]=0;
		rc=record(p,stage,"address",key,NULL); free(key); if (rc) goto done;
	}
	rc=0;
done:
	jf_headers_free(headers); jf_bytes_free(&wire); return p->rc ? p->rc : rc;
}

static int action(struct parser *p,char **mailbox)
{
	if (!is(p,"fileinto")) return invalid(p,"only fileinto move actions are supported");
	if (next(p)) return p->rc;
	if (is(p,":create")) { p->create=1; p->used|=2; if (next(p)) return p->rc; }
	else p->create=0;
	if (p->token!=STRING || !*p->value) return invalid(p,"fileinto requires one nonempty mailbox");
	*mailbox=strdup(p->value); if (!*mailbox) return memory(p);
	p->used|=1;
	if (p->create!=p->profile->create) return invalid(p,"fileinto :create differs from configured profile");
	return next(p) || take(p,';') ? p->rc : 0;
}

static int rule(struct parser *p,unsigned stage,unsigned *legacy_stage)
{
	struct strings fields={0},keys={0}; char *mailbox=NULL,*destination=NULL;
	int address,part=0,match=0,comparator=0; unsigned mask=0; size_t i;
	p->rule_line=p->token_line;
	if (!is(p,"if") || next(p)) goto bad;
	address=is(p,"address"); if (!address && !is(p,"header")) goto bad;
	if (next(p)) goto done;
	while (p->token==WORD && p->value[0]==':') {
		if (is(p,":all") || is(p,":domain")) { if (part) goto bad; part=is(p,":all") ? 1 : 2; if (next(p)) goto done; }
		else if (is(p,":is") || is(p,":matches") || is(p,":regex") || is(p,":contains")) {
			if (match) goto bad;
			match=is(p,":is") ? 1 : is(p,":matches") ? 2 : is(p,":regex") ? 3 : 4;
			if (match==3) p->used|=4;
			if (next(p)) goto done;
		} else if (is(p,":comparator")) {
			if (comparator || next(p)) goto bad;
			if (p->token!=STRING || strcmp(p->value,"i;ascii-casemap")) goto bad;
			comparator=1; if (next(p)) goto done;
		} else goto bad;
	}
	if (!p->legacy && !comparator) goto bad;
	if (strings(p,&fields) || strings(p,&keys)) goto done;
	for (i=0;i<fields.count;i++) {
		unsigned bit=!strcasecmp(fields.items[i],"From") ? 1 : !strcasecmp(fields.items[i],"Sender") ? 2 :
			!strcasecmp(fields.items[i],"To") ? 4 : !strcasecmp(fields.items[i],"Cc") ? 8 :
			!strcasecmp(fields.items[i],"X-Spam-Flag") ? 16 : 0;
		if (!bit || (mask&bit)) goto bad;
		mask|=bit;
	}
	if (take(p,'{') || action(p,&mailbox)) goto done;
	if (!is(p,"stop") || next(p) || take(p,';') || take(p,'}')) goto bad;
	if (p->legacy) {
		if (address || part || (match!=1 && match!=4)) goto bad;
		stage=mask==1 && mapping_is(p,mailbox,"zz.blacklist",JF_ADDRESS) ? 1 :
			mask==2 && mapping_is(p,mailbox,"zz.bounces",JF_SENDER_CONTAINS) ? 2 :
			mask==3 && mapping_is(p,mailbox,"known",JF_ADDRESS) ? 5 :
			mask==16 && mapping_is(p,mailbox,"spam",JF_SPAM_IS) ? 6 :
			mask==12 && mapping_is(p,mailbox,"priv",JF_ADDRESS) ? 7 : mask==3 ? 3 : mask==12 ? 4 : 0;
		/* Historical generators emitted To before From in the route section. */
		if (!stage || (stage<*legacy_stage && !(stage==3 && *legacy_stage==4))) goto bad;
		if (stage>*legacy_stage) *legacy_stage=stage;
	}
	if (stage==2 || stage==6) {
		const char *key=stage==2 ? "mailman-bounce" : "YES";
		if (address || part || mask!=(stage==2 ? 2u : 16u) || match!=(stage==2 ? 4 : 1) ||
			keys.count!=1 || strcasecmp(keys.items[0],key) ||
			!mapping_is(p,mailbox,stage==2 ? "zz.bounces" : "spam",stage==2 ? JF_SENDER_CONTAINS : JF_SPAM_IS)) goto bad;
		if (++p->builtins[stage]!=1) goto bad;
	} else {
		if (mask!=(stage==1 || stage==5 ? 1u : stage==3 ? 3u : 12u) &&
			!(p->legacy && stage==5 && mask==3)) goto bad;
		if (stage==1 || stage==5 || stage==7) {
			if (!mapping_is(p,mailbox,stage==1 ? "zz.blacklist" : stage==5 ? "known" : "priv",JF_ADDRESS)) goto bad;
		} else if (!(destination=unmap(p,mailbox))) goto done;
		for (i=0;i<keys.count;i++) {
			const char *selector;
			if (p->legacy) {
				if (match!=4) goto bad;
				if (stage==1 || stage==5 || stage==7) { if (contact(p,stage,keys.items[i])) goto done; continue; }
				selector=stage==3 ? "from" : "to";
			} else {
				if (!address || (stage!=3 && stage!=4 && !(part==1 && match==1) && !(stage==5 && part==2 && (match==1 || match==2)))) goto bad;
				if (part==1 && match==1) selector=stage==3 ? "from-address" : stage==4 ? "to-address" : "address";
				else if (part==2 && (match==1 || match==2)) {
					int wildcard=!strncmp(keys.items[i],"*.",2);
					if ((match==2)!=wildcard) goto bad;
					selector=stage==3 ? "from-domain" : stage==4 ? "to-domain" : "known-domain";
				} else if (part==1 && match==3 && (stage==3 || stage==4)) selector=stage==3 ? "from" : "to";
				else goto bad;
			}
			if (record(p,stage,selector,keys.items[i],destination)) goto done;
		}
	}
	goto done;
bad:
	invalid(p,"unsupported test, stage, comparator, order or action in generated script");
done:
	strings_free(&fields); strings_free(&keys); free(mailbox); free(destination); return p->rc;
}

int jf_sieve_import(const struct jf_bytes *input,int legacy,const struct jf_profile *profile,
	struct jf_policy **out,struct jf_error *error)
{
	struct parser p={0}; struct strings required={0}; struct jf_source sources[4];
	unsigned stage,legacy_stage=0; size_t i; char *fallback=NULL; int rc=4;
	*out=NULL; p.data=input->data; p.size=input->size; p.line=1; p.error=error; p.legacy=legacy; p.profile=profile;
	if (input->size>JF_INPUT_MAX || (input->size && memchr(input->data,0,input->size))) return jf_product_error(error,"E_SIEVE_IMPORT",1,"oversized script or encoded NUL");
	if (next(&p)) goto done;
	if (is(&p,"require")) {
		if (next(&p) || strings(&p,&required) || take(&p,';')) goto done;
		for (i=0;i<required.count;i++) {
			unsigned cap=!strcmp(required.items[i],"fileinto") ? 1 : !strcmp(required.items[i],"mailbox") ? 2 :
				!strcmp(required.items[i],"regex") && profile->regex ? 4 : legacy && !strcmp(required.items[i],"variables") ? 8 : 0;
			if (!cap || (p.capabilities&cap)) { invalid(&p,"unsupported/duplicate require extension"); goto done; }
			p.capabilities|=cap;
		}
	}
	if (legacy) {
		while (is(&p,"if")) if (rule(&p,0,&legacy_stage)) goto done;
	} else for (stage=1;stage<=7;stage++) {
		if (!is(&p,"if") || next(&p) || !is(&p,"true") || next(&p) || take(&p,'{')) { invalid(&p,"expected seven fixed policy stage blocks"); goto done; }
		while (is(&p,"if")) if (rule(&p,stage,&legacy_stage)) goto done;
		if (take(&p,'}')) goto done;
	}
	if (action(&p,&fallback) || !mapping_is(&p,fallback,"unsorted",JF_ALWAYS) || p.token || (p.used&~p.capabilities)) {
		invalid(&p,"missing fallback, extra command or undeclared extension"); goto done;
	}
	if (!legacy && (p.builtins[2]!=1 || p.builtins[6]!=1)) { invalid(&p,"bounce and spam stages require their unique builtin"); goto done; }
	for (i=0;i<4;i++) {
		sources[i].kind=i==0 ? JF_FILTERS : i==1 ? JF_ALIASES : i==2 ? JF_KNOWN : JF_BLACKLIST;
		sources[i].identity=jf_import_names[i]; sources[i].path="imported Sieve";
		sources[i].data=p.recovered[i].data; sources[i].size=p.recovered[i].size;
	}
	rc=jf_policy_parse(sources,4,NULL,out,error);
	if (rc && error && error->source<4 && error->line && error->line<=p.line_counts[error->source])
		error->line=p.source_lines[error->source][error->line-1];
	if (!rc) {
		size_t routes=0;
		for (i=0;i<jf_policy_count(*out);i++) {
			struct jf_rule *r=&(*out)->entries[i].rule; size_t file;
			for (file=0;file<4;file++) if (!strcmp(r->source_identity,jf_import_names[file]) && r->line && r->line<=p.line_counts[file]) {
				r->line=p.source_lines[file][r->line-1]; break;
			}
			if (r->stage==3 || r->stage==4 || (r->stage==5 && r->kind!=JF_ADDRESS)) routes++;
		}
		if (routes!=p.routes || (!legacy && jf_policy_count(*out)!=p.rules+3)) {
			jf_policy_free(*out); *out=NULL;
			rc=invalid(&p,"duplicate predicates cannot preserve generated source precedence");
		}
	}
done:
	for (i=0;i<4;i++) { jf_bytes_free(&p.recovered[i]); free(p.source_lines[i]); }
	strings_free(&required); free(p.value); free(fallback);
	return p.rc ? p.rc : rc;
}

static int key_compare(const struct jf_rule *a,const struct jf_rule *b)
{
	int order;
	if (a->stage!=b->stage) return a->stage<b->stage ? -1 : 1;
	order=strcmp(a->selector,b->selector);
	return order ? order : strcmp(a->predicate,b->predicate);
}

struct indexed { const struct jf_rule *rule; size_t position; };
static int indexed_compare(const void *a,const void *b)
{
	return key_compare(((const struct indexed *)a)->rule,((const struct indexed *)b)->rule);
}

static int book_next(const struct jf_bytes *out,unsigned long *next_id)
{
	size_t i; unsigned long max=0;
	/* abook section IDs must be fresh numeric IDs; retain existing records. */
	for (i=0;i<out->size;i++) if ((i==0 || out->data[i-1]=='\n') && out->data[i]=='[') {
		size_t j=i+1; unsigned long n=0;
		while (j<out->size && out->data[j]>='0' && out->data[j]<='9') {
			if (n>(ULONG_MAX-9)/10) return 4;
			n=n*10+(unsigned)(out->data[j++]-'0');
		}
		if (j>i+1 && j<out->size && out->data[j]==']' && n>=max) { if (n==ULONG_MAX) return 4; max=n+1; }
	}
	*next_id=max; return 0;
}

static int append_book(struct jf_bytes *out,const char *address,unsigned long *next_id)
{
	char number[64];
	if (*next_id==ULONG_MAX) return 4;
	snprintf(number,sizeof number,"\n[%lu]\nname=Imported Sieve\nemail=",(*next_id)++);
	return jf_bytes_text(out,number) || jf_bytes_text(out,address) || jf_bytes_text(out,"\n") ? 3 : 0;
}

int jf_import_merge(const struct jf_policy *current,const struct jf_policy *imported,
	const struct jf_bytes original[JF_IMPORT_FILES],struct jf_bytes output[JF_IMPORT_FILES],struct jf_error *error)
{
	size_t i,last[9]={0},count=jf_policy_count(current); int rc=3;
	unsigned long next_id[4]={0}; int book_ready[4]={0};
	struct indexed *index=malloc(count*sizeof *index);
	if (!index) return 3;
	for (i=0;i<count;i++) { index[i].rule=jf_policy_rule(current,i); index[i].position=i; }
	qsort(index,count,sizeof *index,indexed_compare);
	for (i=0;i<4;i++) if (jf_bytes_add(&output[i],original[i].data,original[i].size)) goto done;
	for (i=0;i<jf_policy_count(imported);i++) {
		const struct jf_rule *r=jf_policy_rule(imported,i); unsigned file;
		size_t low=0,high=count; const struct indexed *found=NULL;
		if (r->stage==2 || r->stage==6 || r->stage==8) continue;
		while (low<high) {
			size_t mid=low+(high-low)/2; int order=key_compare(r,index[mid].rule);
			if (!order) { found=&index[mid]; break; }
			if (order<0) high=mid; else low=mid+1;
		}
		if (found) {
			if (r->kind!=found->rule->kind || strcmp(r->logical,found->rule->logical)) {
				rc=jf_product_error(error,"E_IMPORT_COLLISION",r->line,"existing selector has a different destination"); goto done;
			}
			if ((r->stage==3 || r->stage==4 || (r->stage==5 && r->kind!=JF_ADDRESS)) && found->position+1<last[r->stage]) {
				rc=jf_product_error(error,"E_IMPORT_ORDER",r->line,"existing rules conflict with imported source order"); goto done;
			}
			last[r->stage]=found->position+1; continue;
		}
		/* A newly appended route cannot precede an existing imported route. */
		if (r->stage==3 || r->stage==4 || (r->stage==5 && r->kind!=JF_ADDRESS)) last[r->stage]=count+1;
		file=r->stage==1 ? 3 : r->stage==7 ? 1 : r->stage==5 && r->kind==JF_ADDRESS ? 2 : 0;
		if (file>=2) {
			if (!book_ready[file] && book_next(&output[file],&next_id[file])) { rc=4; goto done; }
			book_ready[file]=1;
			if (append_book(&output[file],r->predicate,&next_id[file])) { rc=jf_product_error(error,"E_IMPORT_BOOK",r->line,"cannot append bounded abook record"); goto done; }
		} else {
			struct jf_bytes *b=&output[file];
			if (b->size && b->data[b->size-1]!='\n' && jf_bytes_text(b,"\n")) goto done;
			if ((!file && (jf_bytes_text(b,r->selector) || jf_bytes_text(b," "))) || quote(b,r->predicate) ||
				(!file && r->stage!=5 && (jf_bytes_text(b," move ") || quote(b,r->logical))) || jf_bytes_text(b,"\n")) goto done;
		}
	}
	rc=0;
done:
	free(index); return rc;
}
