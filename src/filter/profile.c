#define _POSIX_C_SOURCE 200809L
#include "products.h"
#include "policy_internal.h"
#include <stdlib.h>
#include <string.h>

void jf_profile_free(struct jf_profile *p)
{
	size_t i;
	free(p->id); free(p->prefix); free(p->separator);
	for (i=0;i<p->count;i++) { free(p->mappings[i].logical); free(p->mappings[i].mailbox); }
	free(p->mappings); memset(p,0,sizeof *p);
}

static int text(const char *s, int empty)
{
	const unsigned char *p=(const unsigned char *)s;
	if (!empty && !*p) return 0;
	for (;*p;p++) if (*p<32 || *p==127) return 0;
	return 1;
}

static int logical(const char *s)
{
	const char *p=s, *start=s;
	if (!text(s,0) || *s=='/') return 0;
	for (;;p++) if (!*p || *p=='/') {
		size_t n=(size_t)(p-start);
		if (!n || (n==1 && *start=='.') || (n==2 && !memcmp(start,"..",2))) return 0;
		if (!*p) return 1;
		start=p+1;
	}
}

int jf_profile_parse(const unsigned char *data, size_t size, struct jf_profile *out,
	struct jf_error *error)
{
	struct jf_profile p={0};
	size_t pos=0, line=0;
	unsigned seen=0;
	int rc=3;
	char *buffer=NULL;
	memset(out,0,sizeof *out);
	if (error) memset(error,0,sizeof *error);
	if (size>JF_INPUT_MAX) return jf_product_error(error,"E_INPUT_LIMIT",0,"profile exceeds input limit");
	/* Normal installations need no profile file. The built-in catalogue
	 * qualifies legacy ERE export; emit only capabilities the policy uses.
	 * Nonempty profiles remain explicit capability/mapping overrides. */
	p.id=strdup(size ? "core-v1" : "pigeonhole-2.4.5-posix-ere-v1");
	p.prefix=strdup(""); p.separator=strdup("/");
	p.create=1; p.fileinto=p.mailbox=p.regex=!size;
	buffer=malloc(JF_LINE_MAX+1);
	if (!p.id || !p.prefix || !p.separator || !buffer) goto memory;
	while (pos<size) {
		size_t start=pos, n, count, i;
		char *t[5], **slot=NULL;
		unsigned bit=0;
		while (pos<size && data[pos]!='\n') pos++;
		n=pos-start; if (pos<size) pos++;
		if (n && data[start+n-1]=='\r') n--;
		line++;
		if ((rc=jf_policy_tokens(data+start,n,buffer,t,&count,error,line))) goto fail;
		if (!count) continue;
		if (!strcmp(t[0],"version")) bit=1;
		else if (!strcmp(t[0],"profile")) bit=2;
		else if (!strcmp(t[0],"create-mailboxes")) bit=4;
		else if (!strcmp(t[0],"namespace-prefix")) { bit=8; slot=&p.prefix; }
		else if (!strcmp(t[0],"namespace-separator")) { bit=16; slot=&p.separator; }
		else if (!strcmp(t[0],"regex-qualification")) bit=32;
		if (bit) {
			if (count!=2 || seen&bit) goto invalid;
			seen|=bit;
			if (bit==1 && strcmp(t[1],"1")) goto invalid;
			if (bit==2) {
				char *v;
				if (strcmp(t[1],"core-v1") && strcmp(t[1],"pigeonhole-2.4.5-posix-ere-v1")) goto invalid;
				v=strdup(t[1]); if (!v) goto memory;
				free(p.id); p.id=v;
			}
			if (bit==32 && strcmp(t[1],jf_regex_qualification())) {
				rc=jf_product_error(error,"E_REGEX_QUALIFICATION",line,"digest does not match this compiler's installed qualification catalogue"); goto fail;
			}
			if (bit==4) {
				if (!strcmp(t[1],"yes")) p.create=1;
				else if (!strcmp(t[1],"no")) p.create=0;
				else goto invalid;
			}
			if (slot) {
				char *v;
				if (!text(t[1],1)) goto invalid;
				v=strdup(t[1]); if (!v) goto memory;
				free(*slot); *slot=v;
			}
		} else if (!strcmp(t[0],"capability")) {
			int *cap;
			if (count!=2) goto invalid;
			if (!strcmp(t[1],"fileinto")) cap=&p.fileinto;
			else if (!strcmp(t[1],"mailbox")) cap=&p.mailbox;
			else if (!strcmp(t[1],"regex")) cap=&p.regex;
			else { rc=jf_product_error(error,"E_CAPABILITY",line,t[1]); goto fail; }
			if (*cap) goto invalid;
			*cap=1;
		} else if (!strcmp(t[0],"mailbox")) {
			struct jf_mapping *v;
			if (count!=3 || !logical(t[1]) || !text(t[2],0) || p.count==JF_RULE_MAX) goto invalid;
			for (i=0;i<p.count;i++) if (!strcmp(t[1],p.mappings[i].logical)) goto invalid;
			v=realloc(p.mappings,(p.count+1)*sizeof *v); if (!v) goto memory;
			p.mappings=v; v=&p.mappings[p.count++]; memset(v,0,sizeof *v);
			v->logical=strdup(t[1]); v->mailbox=strdup(t[2]);
			if (!v->logical || !v->mailbox) goto memory;
		} else goto invalid;
	}
	if (size && (seen&3)!=3) { rc=jf_product_error(error,"E_PROFILE",0,"version 1 and explicit supported profile required"); goto fail; }
	if (!p.fileinto || (p.create && !p.mailbox)) {
		rc=jf_product_error(error,"E_CAPABILITY",0,"fileinto required; create-mailboxes yes also requires mailbox"); goto fail;
	}
	if ((!strcmp(p.id,"core-v1") && (p.regex || (seen&32))) ||
		(strcmp(p.id,"core-v1") && (!p.regex || (size && !(seen&32))))) {
		rc=jf_product_error(error,"E_REGEX_QUALIFICATION",0,"optional regex profile requires regex capability and installed qualification digest; core-v1 admits neither"); goto fail;
	}
	free(buffer); *out=p; return 0;
invalid:
	rc=jf_product_error(error,"E_PROFILE",line,"unknown, duplicate or malformed profile record"); goto fail;
memory:
	rc=jf_product_error(error,"E_MEMORY",line,"profile allocation failed");
fail:
	free(buffer); jf_profile_free(&p); return rc;
}

char *jf_profile_mailbox(const struct jf_profile *p, const struct jf_rule *r)
{
	size_t i;
	struct jf_bytes b={0};
	const char *s;
	for (i=0;i<p->count;i++) if (!strcmp(p->mappings[i].logical,r->logical)) return strdup(p->mappings[i].mailbox);
	if (!strcmp(r->logical,"known")) return strdup("INBOX");
	if (r->kind==JF_SPAM_IS) return strdup("Spam");
	if (jf_bytes_text(&b,p->prefix)) goto fail;
	for (s=r->logical;*s;s++) {
		if (*s=='/') { if (jf_bytes_text(&b,p->separator)) goto fail; }
		else if (jf_bytes_add(&b,s,1)) goto fail;
	}
	if (jf_bytes_add(&b,"",1)) goto fail;
	return (char *)b.data;
fail:
	jf_bytes_free(&b); return NULL;
}
