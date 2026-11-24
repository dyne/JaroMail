#ifndef JARO_CONTACT_LEGACY_IMPORT_H
#define JARO_CONTACT_LEGACY_IMPORT_H

#include "contact/legacy-book.h"
#include "contact/store.h"

struct jaro_contact_import_error {
	struct jaro_contact_error detail;
	size_t record, line, field;
};

/* Map one borrowed legacy record into the current write transaction. An
 * optional observer receives the mapped values after the save succeeds.
 * The caller must roll back the transaction if any source fails. */
int jaro_contact_import_record(struct jaro_contact_store *store, int64_t book,
	const struct jaro_legacy_record *source, jaro_contact_visitor observe, void *data,
	struct jaro_contact_import_error *error);

#endif
