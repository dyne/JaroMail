#define _POSIX_C_SOURCE 200809L
#include "policy_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int error(struct jf_error *e, const char *code, const char *detail, int rc)
{
	if (e) {
		e->code=code; e->source=SIZE_MAX; e->line=0;
		snprintf(e->detail,sizeof e->detail,"%s",detail);
	}
	return rc;
}

static const char *index_key(const struct jf_entry *e)
{
	return e->rule.predicate+(e->rule.kind==JF_DOMAIN_SUBDOMAINS ? 2 : 0);
}

static int compare_index(const void *a, const void *b)
{
	const struct jf_entry *x=*(struct jf_entry * const *)a, *y=*(struct jf_entry * const *)b;
	int c;
	if (x->rule.kind!=y->rule.kind) return x->rule.kind<y->rule.kind ? -1 : 1;
	c=strcmp(index_key(x),index_key(y));
	return c ? c : x<y ? -1 : x>y;
}

int jf_policy_build_index(struct jf_policy *p, struct jf_error *e)
{
	size_t i, indexed=0, legacy=0;
	for (i=0;i<p->count;i++) {
		switch (p->entries[i].rule.kind) {
		case JF_ADDRESS: case JF_DOMAIN_APEX: case JF_DOMAIN_SUBDOMAINS: indexed++; break;
		case JF_ERE: legacy++; break;
		case JF_SENDER_CONTAINS: p->bounce=&p->entries[i]; break;
		case JF_SPAM_IS: p->spam=&p->entries[i]; break;
		case JF_ALWAYS: p->fallback=&p->entries[i]; break;
		}
	}
	if (indexed) p->index=p->allocator.allocate(p->allocator.context,indexed*sizeof *p->index);
	if (legacy) p->legacy=p->allocator.allocate(p->allocator.context,legacy*sizeof *p->legacy);
	if ((indexed && !p->index) || (legacy && !p->legacy))
		return error(e,"E_MEMORY","policy matching index allocation failed",3);
	for (i=0;i<p->count;i++) {
		struct jf_entry *entry=&p->entries[i];
		if (entry->rule.kind==JF_ERE) p->legacy[p->legacy_count++]=entry;
		else if (entry->rule.kind<=JF_DOMAIN_SUBDOMAINS) p->index[p->index_count++]=entry;
	}
	if (indexed) qsort(p->index,indexed,sizeof *p->index,compare_index);
	return 0;
}

static unsigned char fold(unsigned char c)
{
	return c>='A' && c<='Z' ? (unsigned char)(c+'a'-'A') : c;
}

static int compare_lookup(enum jf_kind kind, const char *key, const struct jf_entry *e,
	struct jf_match_stats *stats)
{
	if (stats) stats->index_comparisons++;
	if (kind!=e->rule.kind) return kind<e->rule.kind ? -1 : 1;
	return strcmp(key,index_key(e));
}

static const struct jf_entry *lookup(const struct jf_policy *p, enum jf_kind kind,
	const char *key, unsigned field, const struct jf_entry *best, struct jf_match_stats *stats)
{
	size_t low=0, high=p->index_count, i;
	while (low<high) {
		size_t mid=low+(high-low)/2;
		if (compare_lookup(kind,key,p->index[mid],stats)>0) low=mid+1;
		else high=mid;
	}
	for (i=low;i<p->index_count && !compare_lookup(kind,key,p->index[i],stats);i++)
		if ((p->index[i]->rule.fields&field) && p->index[i]<best) best=p->index[i];
	return best;
}

static int validate_text(struct jf_text text, size_t *total, struct jf_error *e)
{
	size_t i;
	if (text.size>JF_LINE_MAX || text.size>JF_HEADERS_MAX-*total)
		return error(e,"E_MESSAGE_LIMIT","parsed message field/total limit exceeded",7);
	if (text.size && !text.data) return error(e,"E_MESSAGE_ARGUMENT","missing field buffer",7);
	*total+=text.size;
	for (i=0;i<text.size;i++)
		if ((text.data[i]<32 && text.data[i]!='\t') || text.data[i]==127)
			return error(e,"E_MESSAGE_TEXT","NUL/control byte in parsed field",7);
	return 0;
}

static int raw_match(struct jf_text text, const char *key, int contains)
{
	size_t i, j, n=strlen(key);
	/* Sieve header :is strips surrounding SP/HTAB (RFC 5228 section 5.6).
	 * Adjust this local view only; Sender :contains and caller bytes stay intact.
	 */
	if (!contains) {
		while (text.size && (text.data[0]==' ' || text.data[0]=='\t')) {
			text.data++; text.size--;
		}
		while (text.size && (text.data[text.size-1]==' ' || text.data[text.size-1]=='\t')) text.size--;
	}
	if (text.size<n || (!contains && text.size!=n)) return 0;
	for (i=0;i<=text.size-n;i++) {
		for (j=0;j<n && fold(text.data[i+j])==fold((unsigned char)key[j]);j++) {}
		if (j==n) return 1;
		if (!contains) break;
	}
	return 0;
}

int jf_policy_classify(const struct jf_policy *p, const struct jf_message *message,
	const struct jf_rule **winner, struct jf_error *e, struct jf_match_stats *stats)
{
	const struct jf_entry *best;
	char mailbox[JF_LINE_MAX+1];
	size_t i, j, total=0, sender_addresses=0, sender_values=0;
	int rc;
	if (e) memset(e,0,sizeof *e);
	if (stats) memset(stats,0,sizeof *stats);
	if (!winner) return error(e,"E_MESSAGE_ARGUMENT","missing result pointer",7);
	*winner=NULL;
	if (!p || !message || (message->address_count && !message->addresses) ||
		(message->sender_count && !message->senders) || (message->spam_count && !message->spam_flags))
		return error(e,"E_MESSAGE_ARGUMENT","invalid policy/parsed message",7);
	if (message->address_count>JF_HEADERS_MAX || message->sender_count>JF_HEADERS_MAX ||
		message->spam_count>JF_HEADERS_MAX)
		return error(e,"E_MESSAGE_LIMIT","too many parsed message fields",7);
	/* Validate the complete input before considering an early-stage winner. */
	for (i=0;i<message->address_count;i++) {
		struct jf_mailbox address=message->addresses[i];
		if (address.field!=JF_FROM && address.field!=JF_SENDER && address.field!=JF_TO && address.field!=JF_CC)
			return error(e,"E_MESSAGE_FIELD","invalid parsed address field",7);
		if ((rc=validate_text(address.value,address.field==JF_SENDER ? &sender_addresses : &total,e))) return rc;
		if (!jf_valid_utf8(address.value.data,address.value.size))
			return error(e,"E_MESSAGE_ADDRESS","invalid UTF8 mailbox",7);
		if (address.value.size) memcpy(mailbox,address.value.data,address.value.size);
		mailbox[address.value.size]=0;
		if (!jf_valid_mailbox(mailbox)) return error(e,"E_MESSAGE_ADDRESS","invalid parsed mailbox/domain",7);
	}
	for (i=0;i<message->sender_count;i++)
		if ((rc=validate_text(message->senders[i],&sender_values,e))) return rc;
	/* Raw Sender values and their extracted mailboxes represent the same wire
	 * content. Bound both independently and count their larger representation
	 * once, so a valid near-limit header block is not charged twice.
	 */
	if (sender_values<sender_addresses) sender_values=sender_addresses;
	if (sender_values>JF_HEADERS_MAX-total)
		return error(e,"E_MESSAGE_LIMIT","parsed message total limit exceeded",7);
	total+=sender_values;
	for (i=0;i<message->spam_count;i++)
		if ((rc=validate_text(message->spam_flags[i],&total,e))) return rc;
	best=p->fallback;
	for (i=0;i<message->sender_count;i++)
		if (raw_match(message->senders[i],"mailman-bounce",1)) best=p->bounce;
	for (i=0;i<message->spam_count;i++)
		if (raw_match(message->spam_flags[i],"YES",0) && p->spam<best) best=p->spam;
	for (i=0;i<message->address_count;i++) {
		struct jf_mailbox address=message->addresses[i];
		char *domain, *suffix;
		for (j=0;j<address.value.size;j++) mailbox[j]=(char)fold(address.value.data[j]);
		mailbox[address.value.size]=0;
		best=lookup(p,JF_ADDRESS,mailbox,address.field,best,stats);
		domain=strrchr(mailbox,'@')+1; /* Complete mailbox was validated above. */
		best=lookup(p,JF_DOMAIN_APEX,domain,address.field,best,stats);
		/* Strict suffixes only: remove at least one complete label, never apex. */
		for (suffix=strchr(domain,'.');suffix;suffix=strchr(suffix+1,'.'))
			best=lookup(p,JF_DOMAIN_SUBDOMAINS,suffix+1,address.field,best,stats);
		/* Legacy EREs are an ordered side index; no typed/contact scan. */
		if (p->legacy_count) {
			locale_t old=uselocale(p->locale);
			if (!old) return error(e,"E_REGEX_EXEC","cannot select C regex locale",3);
			memcpy(mailbox,address.value.data,address.value.size); mailbox[address.value.size]=0;
			for (j=0;j<p->legacy_count && p->legacy[j]<best;j++) {
				const struct jf_entry *entry=p->legacy[j];
				if (!(entry->rule.fields&address.field)) continue;
				if (stats) stats->regex_checks++;
				rc=regexec(&entry->regex,mailbox,0,NULL,0);
				if (!rc) { best=entry; break; }
				if (rc!=REG_NOMATCH) {
					uselocale(old);
					return error(e,"E_REGEX_EXEC","legacy regex execution failed",3);
				}
			}
			uselocale(old);
		}
	}
	*winner=&best->rule;
	return 0;
}
