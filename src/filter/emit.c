#include "products.h"
#include <stdio.h>
#include <string.h>

static int quote_sieve(struct jf_bytes *b, const char *s, int glob)
{
	if (jf_bytes_text(b,"\"")) return 3;
	for (;*s;s++) {
		if (glob && *s=='?' && jf_bytes_text(b,"\\\\")) return 3;
		if ((*s=='"' || *s=='\\') && jf_bytes_text(b,"\\")) return 3;
		if (jf_bytes_add(b,s,1)) return 3;
	}
	return jf_bytes_text(b,"\"");
}

static int action(struct jf_bytes *b, const struct jf_profile *p, const char *mailbox)
{
	return jf_bytes_text(b,p->create ? "fileinto :create " : "fileinto ") ||
		quote_sieve(b,mailbox,0) || jf_bytes_text(b,";");
}

/* One NUL-framed row per generated action. Raw authored paths stay local. */
static int provenance(struct jf_bytes *b, size_t line, const struct jf_rule *rule)
{
	char location[32], authored[32];
	snprintf(location,sizeof location,"%zu",line);
	snprintf(authored,sizeof authored,"%zu",rule->line);
	const char *fields[]={location,rule->id,rule->source,authored};
	for (size_t i=0;i<sizeof fields/sizeof *fields;i++)
		if (jf_bytes_add(b,fields[i],strlen(fields[i])+1)) return 3;
	return 0;
}

int jf_emit_text(const struct jf_policy *policy, const struct jf_profile *profile,
	char *const *mailboxes, struct jf_products *out, struct jf_error *error)
{
	size_t i=0, line=2;
	unsigned stage; int regex=0;
	struct jf_bytes *b=&out->sieve;
	for (i=0;i<jf_policy_count(policy);i++) if (jf_policy_rule(policy,i)->kind==JF_ERE) regex=1;
	i=0;
	if (jf_bytes_text(b,"require [\"fileinto\"") ||
		(profile->create && jf_bytes_text(b,",\"mailbox\"")) ||
		(regex && profile->regex && jf_bytes_text(b,",\"regex\"")) || jf_bytes_text(b,"];\n")) goto memory;
	for (stage=1;stage<=7;stage++) {
		/* An empty block shares its bytecode offset with the next command.
		 * Keep both on one source line so Pigeonhole's debug location is exact. */
		if (i==jf_policy_count(policy) || jf_policy_rule(policy,i)->stage!=stage) {
			if (jf_bytes_text(b,"if true {} ")) goto memory;
			continue;
		}
		if (jf_bytes_text(b,"if true {\n")) goto memory;
		line++;
		for (;i<jf_policy_count(policy) && jf_policy_rule(policy,i)->stage==stage;i++) {
			const struct jf_rule *r=jf_policy_rule(policy,i);
			const char *fields=r->fields==1 ? "\"From\"" : r->fields==3 ? "[\"From\",\"Sender\"]" :
				r->fields==12 ? "[\"To\",\"Cc\"]" : NULL;
			if (r->kind==JF_ERE && (!profile->regex || !jf_regex_supported(r->predicate))) {
				if (error) error->source=i;
				jf_product_error(error,"E_REGEX_EXPORT",r->line,profile->regex ?
					"ERE outside qualified ASCII concatenation subset; migrate to typed address/domain rules" :
					"core-v1 cannot export legacy ERE; migrate to typed address/domain rules or select qualified profile");
				if (error) error->source=i; /* rule index, resolved by compiler diagnostic adapter */
				return 4;
			}
			if (jf_bytes_text(b,"  if ")) goto memory;
			if (r->kind==JF_SENDER_CONTAINS) {
				if (jf_bytes_text(b,"header :contains :comparator \"i;ascii-casemap\" \"Sender\" ")) goto memory;
			} else if (r->kind==JF_SPAM_IS) {
				if (jf_bytes_text(b,"header :is :comparator \"i;ascii-casemap\" \"X-Spam-Flag\" ")) goto memory;
			} else {
				if (!fields) return jf_product_error(error,"E_RULE",r->line,"invalid address field mask");
				if (jf_bytes_text(b,r->kind==JF_ERE ? "address :all :regex " : r->kind==JF_ADDRESS ? "address :all :is " :
					r->kind==JF_GLOB ? "address :all :matches " : r->kind==JF_DOMAIN_APEX ? "address :domain :is " : "address :domain :matches ") ||
					jf_bytes_text(b,":comparator \"i;ascii-casemap\" ") || jf_bytes_text(b,fields) || jf_bytes_text(b," ")) goto memory;
			}
			if (quote_sieve(b,r->predicate,r->kind==JF_GLOB) || jf_bytes_text(b," { ") || action(b,profile,mailboxes[i]) ||
				jf_bytes_text(b," stop; }\n") || provenance(&out->provenance,line++,r)) goto memory;
		}
		if (jf_bytes_text(b,"}\n")) goto memory;
		line++;
	}
	if (i+1!=jf_policy_count(policy) || jf_policy_rule(policy,i)->kind!=JF_ALWAYS)
		return jf_product_error(error,"E_RULE",0,"missing unique final fallback");
	if (action(b,profile,mailboxes[i]) || jf_bytes_text(b,"\n") ||
		provenance(&out->provenance,line,jf_policy_rule(policy,i))) goto memory;
	return 0;
memory:
	return jf_product_error(error,"E_MEMORY",0,"text output allocation/size limit");
}
