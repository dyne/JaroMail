#ifndef JARO_FILTER_CONTACT_H
#define JARO_FILTER_CONTACT_H
#include "policy.h"
/* Returns -1 when argv belongs to the filter compiler proper. */
int jf_contact_command(int argc, char **argv, struct jf_error *error);
#endif
