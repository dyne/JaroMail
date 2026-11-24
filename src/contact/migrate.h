#ifndef JARO_CONTACT_MIGRATE_H
#define JARO_CONTACT_MIGRATE_H

#include "contact/store.h"

#define JARO_CONTACT_DATABASE "contacts.sqlite"

struct jaro_contact_migration_error {
	const char *reason, *field;
	size_t source, record, line, field_number; /* One-based where known. */
	int native_code;
};

/* Format a diagnostic without including contact values or source names. */
void jaro_contact_migration_diagnostic(char *buffer, size_t size,
	const struct jaro_contact_migration_error *error);

/* Reports counts after successful publication, in bytewise filename order.
 * Sources are identified by their one-based position in that order. */
typedef void (*jaro_contact_migration_notice)(void *, size_t source, size_t records);

/* First use of an existing, owner-controlled profile directory. Existing
 * contacts.sqlite is validated without reading legacy sources. */
int jaro_contact_initialize(const char *profile,
	jaro_contact_migration_notice notice, void *data, struct jaro_contact_migration_error *error);

enum jaro_contact_profile_mode {
	JARO_CONTACT_PROFILE_READ,
	JARO_CONTACT_PROFILE_WRITE,
	JARO_CONTACT_PROFILE_PREVIEW
};

/* Open the single store for a profile, initializing it when necessary.
 * A preview opens existing data read-only and imports missing data privately
 * in memory without creating anything in the profile. */
int jaro_contact_profile_open(const char *profile, enum jaro_contact_profile_mode mode,
	struct jaro_contact_store **store, struct jaro_contact_migration_error *error);

#endif
