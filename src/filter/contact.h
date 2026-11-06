#ifndef JARO_FILTER_CONTACT_H
#define JARO_FILTER_CONTACT_H
#include "policy.h"
/* Append one native abook record without reparsing a display-name header or
 * rewriting existing records. Return 1 for an exact existing mailbox. */
int jf_contact_add(const char *,const char *,const char *,int,struct jf_error *);
#endif
