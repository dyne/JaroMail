#include "products.h"
#include <stdio.h>
#include <string.h>

/* A literal quoted word, never a shell expression. Single quotes also preserve
 * embedded newlines and backslashes; only the quote itself needs splitting. */
static int quote_zsh(struct jf_bytes *b, const char *s)
{
	if (jf_bytes_text(b,"'")) return 3;
	for (;*s;s++) if (jf_bytes_text(b,*s=='\'' ? "'\\''" : "") ||
		(*s!='\'' && jf_bytes_add(b,s,1))) return 3;
	return jf_bytes_text(b,"'");
}

static int quote_sieve(struct jf_bytes *b, const char *s)
{
	if (jf_bytes_text(b,"\"")) return 3;
	for (;*s;s++) {
		if ((*s=='"' || *s=='\\') && jf_bytes_text(b,"\\")) return 3;
		if (jf_bytes_add(b,s,1)) return 3;
	}
	return jf_bytes_text(b,"\"");
}

const char *jf_kind_name(enum jf_kind kind)
{
	static const char *names[]={"address","domain-apex","domain-subdomains","ere",
		"sender-contains","spam-is","always"};
	return (unsigned)kind<sizeof names/sizeof *names ? names[kind] : NULL;
}

static int zsh(const struct jf_policy *policy, struct jf_bytes *b)
{
	static const char *columns[]={"ids","kinds","patterns","destinations","stages","masks"};
	size_t i,j;
	if (jf_bytes_text(b,"# Jaromail native filter compatibility v1\n"
		"unset filter_from filter_to filter_own filter_whitelist filter_blacklist "
		"filter_rule_ids filter_rule_kinds filter_rule_patterns filter_rule_destinations "
		"filter_rule_stages filter_rule_masks\n"
		"typeset -gA filter_from filter_to\n"
		"typeset -ga filter_own filter_whitelist filter_blacklist filter_rule_ids "
		"filter_rule_kinds filter_rule_patterns filter_rule_destinations filter_rule_stages filter_rule_masks\n"
		"filter_from=()\nfilter_to=()\nfilter_own=()\nfilter_whitelist=()\nfilter_blacklist=()\n")) return 3;
	for (i=0;i<jf_policy_count(policy);i++) {
		const struct jf_rule *r=jf_policy_rule(policy,i);
		const char *array=r->stage==1 ? "filter_blacklist" : r->stage==5 && r->kind==JF_ADDRESS ?
			"filter_whitelist" : r->stage==7 ? "filter_own" : NULL;
		if (r->kind==JF_ERE) {
			if (jf_bytes_text(b,r->stage==3 ? "filter_from+=(" : "filter_to+=(") ||
				quote_zsh(b,r->predicate) || jf_bytes_text(b," ") || quote_zsh(b,r->local) || jf_bytes_text(b,")\n")) return 3;
		} else if (array) {
			if (jf_bytes_text(b,array) || jf_bytes_text(b,"+=(") || quote_zsh(b,r->predicate) || jf_bytes_text(b,")\n")) return 3;
		}
	}
	for (j=0;j<6;j++) {
		if (jf_bytes_text(b,"filter_rule_") || jf_bytes_text(b,columns[j]) || jf_bytes_text(b,"=(")) return 3;
		for (i=0;i<jf_policy_count(policy);i++) {
			const struct jf_rule *r=jf_policy_rule(policy,i);
			char number[32];
			const char *value=j==0 ? r->id : j==1 ? jf_kind_name(r->kind) : j==2 ? r->predicate :
				j==3 ? r->local : number;
			if (j>=4) snprintf(number,sizeof number,"%u",j==4 ? r->stage : r->fields);
			if (jf_bytes_text(b," ") || quote_zsh(b,value)) return 3;
		}
		if (jf_bytes_text(b," )\n")) return 3;
	}
	return 0;
}

static int action(struct jf_bytes *b, const struct jf_profile *p, const char *mailbox)
{
	return jf_bytes_text(b,p->create ? "fileinto :create " : "fileinto ") ||
		quote_sieve(b,mailbox) || jf_bytes_text(b,";");
}

int jf_emit_text(const struct jf_policy *policy, const struct jf_profile *profile,
	char *const *mailboxes, struct jf_products *out, struct jf_error *error)
{
	size_t i=0;
	unsigned stage; int regex=0;
	struct jf_bytes *b=&out->sieve;
	if (zsh(policy,&out->zsh)) goto memory;
	for (i=0;i<jf_policy_count(policy);i++) if (jf_policy_rule(policy,i)->kind==JF_ERE) regex=1;
	i=0;
	if (jf_bytes_text(b,"require [\"fileinto\"") ||
		(profile->create && jf_bytes_text(b,",\"mailbox\"")) ||
		(regex && profile->regex && jf_bytes_text(b,",\"regex\"")) || jf_bytes_text(b,"];\n")) goto memory;
	for (stage=1;stage<=7;stage++) {
		if (jf_bytes_text(b,"if true {\n")) goto memory;
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
					r->kind==JF_DOMAIN_APEX ? "address :domain :is " : "address :domain :matches ") ||
					jf_bytes_text(b,":comparator \"i;ascii-casemap\" ") || jf_bytes_text(b,fields) || jf_bytes_text(b," ")) goto memory;
			}
			if (quote_sieve(b,r->predicate) || jf_bytes_text(b," { ") || action(b,profile,mailboxes[i]) ||
				jf_bytes_text(b," stop; }\n")) goto memory;
		}
		if (jf_bytes_text(b,"}\n")) goto memory;
	}
	if (i+1!=jf_policy_count(policy) || jf_policy_rule(policy,i)->kind!=JF_ALWAYS)
		return jf_product_error(error,"E_RULE",0,"missing unique final fallback");
	if (action(b,profile,mailboxes[i]) || jf_bytes_text(b,"\n")) goto memory;
	return 0;
memory:
	return jf_product_error(error,"E_MEMORY",0,"text output allocation/size limit");
}
