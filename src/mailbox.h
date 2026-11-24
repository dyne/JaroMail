#ifndef JARO_MAILBOX_H
#define JARO_MAILBOX_H

#include <stddef.h>

#define JARO_MAILBOX_MAX (64u * 1024u)

int jf_valid_utf8(const unsigned char *, size_t);
int jf_valid_domain(const char *);
int jf_valid_mailbox(const char *);
/* In-place minimal quoting. Never grows a valid input or folds case. */
int jf_normalize_mailbox(char *);

#endif
