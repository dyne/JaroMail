#ifndef JARO_FILTER_HEADERS_H
#define JARO_FILTER_HEADERS_H
#include "policy.h"

struct jf_headers;
/* Owns every parsed mailbox and unfolded Sender/spam value; all-or-nothing.
 * Parse a message buffer, stopping at the first LF/CRLF empty line. Body bytes
 * are never examined. Original bytes are borrowed only during this call.
 * Missing relevant fields/empty groups are valid. Malformed/unsupported address
 * syntax, empty address fields, NUL/control bytes or truncation fail with 7;
 * allocation/read failures use 3. No message bytes are changed.
 * Raw physical line payload and unfolded field value are each <=64 KiB;
 * total wire header bytes, including line endings/separator, are <=1 MiB.
 */
int jf_headers_parse(const unsigned char *, size_t, const struct jf_allocator *,
	struct jf_headers **out, struct jf_error *);
/* Reads from the caller-owned descriptor using bounded chunks. May read ahead
 * by at most 4095 bytes past the separator; never interprets those body bytes.
 * Does not close/seek/write the descriptor. Caller must not share its offset.
 */
int jf_headers_read(int fd, const struct jf_allocator *, struct jf_headers **,
	struct jf_error *);
const struct jf_message *jf_headers_message(const struct jf_headers *);
size_t jf_headers_wire_size(const struct jf_headers *);
/* SHA256 of exact consumed wire header bytes, including the separator. */
const char *jf_headers_digest(const struct jf_headers *);
void jf_headers_free(struct jf_headers *);
#endif
