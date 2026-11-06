/* Shared native filter policy. No input execution, IO or publication. */
#ifndef JARO_FILTER_POLICY_H
#define JARO_FILTER_POLICY_H

#include <stddef.h>

#define JF_INPUT_MAX (64u * 1024u * 1024u)
#define JF_LINE_MAX (64u * 1024u)
#define JF_RULE_MAX 100000u
#define JF_BUILTIN_RULES 3u
#define JF_RULE_RECORD_MAX (JF_RULE_MAX + JF_BUILTIN_RULES)
#define JF_HEADERS_MAX (1024u * 1024u)

enum jf_source_kind {
	JF_FILTERS, JF_ACCOUNT, JF_ALIASES, JF_KNOWN, JF_BLACKLIST
};
enum jf_kind {
	JF_ADDRESS, JF_DOMAIN_APEX, JF_DOMAIN_SUBDOMAINS, JF_ERE,
	JF_SENDER_CONTAINS, JF_SPAM_IS, JF_ALWAYS
};
enum jf_field { JF_FROM = 1, JF_SENDER = 2, JF_TO = 4, JF_CC = 8, JF_SPAM = 16 };

/* Buffers are borrowed for this call only; no NUL terminator is required.
 * identity is root-relative (or accounts:relative/path for external accounts).
 * path is raw diagnostic/provenance bytes, excluding NUL. Account snapshots
 * are sorted by identity; accounts precede aliases for duplicate provenance.
 * Contact exports contain one raw mailbox per nonblank line (no comments or
 * JSON decoding, so quoted local parts and leading # remain mailbox data).
 * Alias/email account fields use the policy token lexer, with # comments.
 * Account records other than email are ignored, never interpreted as code.
 */
struct jf_source {
	enum jf_source_kind kind;
	const char *identity;
	const char *path;
	const unsigned char *data;
	size_t size;
};

/* Optional per-parse allocator. All returned policy storage uses this pair;
 * context must outlive the policy. NULL selects malloc/free. No global hook.
 */
struct jf_allocator {
	void *context;
	void *(*allocate)(void *, size_t);
	void (*release)(void *, void *);
};

struct jf_error {
	const char *code;        /* Stable ASCII code, NULL on success. */
	size_t source;          /* Caller source index; SIZE_MAX if not applicable. */
	size_t line;            /* Physical 1-based line, 0 if not applicable. */
	char detail[160];        /* Bounded human diagnostic; never a partial rule. */
};

/* Shared policy/profile lexer. buffer has size+1 bytes; at most five tokens.
 * The input is one physical line without its CR/LF ending. */
int jf_policy_tokens(const unsigned char *, size_t, char *, char *[5],
	size_t *, struct jf_error *, size_t);

struct jf_rule {
	enum jf_kind kind;
	unsigned stage, fields;
	const char *selector, *predicate, *action;
	const char *logical, *local, *sieve;
	const char *source, *source_identity;
	size_t line, occurrence;
	char id[75];             /* r:/known:/blacklist:/own: + SHA256 hex */
};

struct jf_policy;
/* On any failure *out is NULL; no partially usable policy escapes.
 * Return 0, 3 (resource failure), or 4 (invalid policy). Regex compilation uses
 * a thread-local C locale, leaving the caller's process/thread locale intact.
 * Builtins are included and rules are returned in resolved stage/source order.
 */
int jf_policy_parse(const struct jf_source *, size_t,
	const struct jf_allocator *, struct jf_policy **out, struct jf_error *);
void jf_policy_free(struct jf_policy *);
size_t jf_policy_count(const struct jf_policy *);
const struct jf_rule *jf_policy_rule(const struct jf_policy *, size_t);

struct jf_text { const unsigned char *data; size_t size; };
struct jf_mailbox { unsigned field; struct jf_text value; };
/* Parsed mailbox strings only: no names/comments/body text. Equivalent quoted
 * spellings are normalized to minimal quoting, preserving necessary quotes and
 * re-escaping quote/backslash. Values are borrowed and never mutated
 * for the call, explicitly sized and need no NUL terminator. Sender/spam values
 * are already unfolded raw field values; repeated fields remain separate.
 * Parsed Sender mailboxes and raw Sender values share one content-size budget
 * (the larger representation), avoiding a double charge for the same headers.
 */
struct jf_message {
	const struct jf_mailbox *addresses;
	size_t address_count;
	const struct jf_text *senders;
	size_t sender_count;
	const struct jf_text *spam_flags;
	size_t spam_count;
};
struct jf_match_stats { size_t index_comparisons, regex_checks; };
/* Pure classification: no IO, processes, policy mutation or per-call policy
 * allocations/regex compilation. *winner borrows every decision/provenance
 * field from policy until policy_free. On error it is NULL. Return 0, 3 for a
 * resource/regex execution error, or 7 for invalid/oversized parsed input.
 * Every address is validated before selecting a winner; malformed data never
 * becomes an otherwise valid early-stage result. Unsupported domain syntax
 * (including non-ASCII domains) is an explicit E_MESSAGE_ADDRESS error.
 * Stats are optional, caller-owned deterministic work counters.
 */
int jf_policy_classify(const struct jf_policy *, const struct jf_message *,
	const struct jf_rule **winner, struct jf_error *, struct jf_match_stats *);

#endif
