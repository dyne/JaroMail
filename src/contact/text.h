#ifndef JARO_CONTACT_TEXT_H
#define JARO_CONTACT_TEXT_H

#include "contact/store.h"

int jaro_contact_valid_text(struct jaro_contact_text text);
int jaro_contact_mailbox_key(struct jaro_contact_text text, char **key);
int jaro_contact_print_text(FILE *out, struct jaro_contact_text text);

#endif
