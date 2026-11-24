#ifndef JARO_CONTACT_MIGRATE_H
#define JARO_CONTACT_MIGRATE_H

#include "contact/store.h"

#define JARO_CONTACT_DATABASE "contacts.sqlite"

struct jaro_contact_migration_error {
	const char *reason, *field;
	size_t source, record, line, field_number; /* One-based where known. */
	int native_code;
};

/* Reports counts after successful publication, in bytewise filename order.
 * Sources are identified by their one-based position in that order. */
typedef void (*jaro_contact_migration_notice)(void *, size_t source, size_t records);

/* First use of an existing, owner-controlled profile directory. Existing
 * contacts.sqlite is validated without reading legacy sources. */
int jaro_contact_initialize(const char *profile,
	jaro_contact_migration_notice notice, void *data, struct jaro_contact_migration_error *error);

#endif
