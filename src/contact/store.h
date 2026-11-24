#ifndef JARO_CONTACT_STORE_H
#define JARO_CONTACT_STORE_H

#include <stdint.h>
#include <stdio.h>

#define JARO_CONTACT_APPLICATION_ID 0x4a434e54
#define JARO_CONTACT_SCHEMA_VERSION 1
#define JARO_CONTACT_TEXT_MAX (64u * 1024u)
#define JARO_CONTACT_FIELDS_MAX 128u
#define JARO_CONTACT_ADDRESSES_MAX 16384u
#define JARO_CONTACT_RECORD_MAX (16u * 1024u * 1024u)

struct jaro_contact_store;

struct jaro_contact_text {
	const char *data; /* delimiters and trailing NUL are outside the span */
	size_t size;
};

struct jaro_contact_field {
	struct jaro_contact_text key, value;
};

struct jaro_contact {
	int64_t id, book_id, revision;
	struct jaro_contact_text name, nickname, notes;
	const struct jaro_contact_text *addresses;
	size_t address_count;
	const struct jaro_contact_field *fields;
	size_t field_count;
};

struct jaro_contact_error {
	const char *reason, *field; /* static labels, independent of private data */
	size_t ordinal;            /* one-based for repeated fields */
	int native_code;           /* errno or extended SQLite result, if relevant */
};

enum jaro_contact_result {
	JARO_CONTACT_OK,
	JARO_CONTACT_INVALID,
	JARO_CONTACT_MISSING,
	JARO_CONTACT_CONFLICT,
	JARO_CONTACT_BUSY,
	JARO_CONTACT_STORAGE,
	JARO_CONTACT_CONSUMER
};

enum jaro_contact_open_mode {
	JARO_CONTACT_READ,
	JARO_CONTACT_WRITE,
	JARO_CONTACT_CREATE_EXCLUSIVE
};

/* CREATE_EXCLUSIVE is for a private unpublished path in a trusted directory.
 * Existing databases are opened without SQLite's CREATE flag. */
int jaro_contact_open(const char *path, enum jaro_contact_open_mode mode,
	struct jaro_contact_store **store, struct jaro_contact_error *error);
int jaro_contact_close(struct jaro_contact_store *store, struct jaro_contact_error *error);
int jaro_contact_verify(struct jaro_contact_store *store, struct jaro_contact_error *error);
int jaro_contact_begin(struct jaro_contact_store *store, struct jaro_contact_error *error);
int jaro_contact_commit(struct jaro_contact_store *store, struct jaro_contact_error *error);
int jaro_contact_rollback(struct jaro_contact_store *store, struct jaro_contact_error *error);

/* Create and save require a write transaction. Book lookup is exact.
 * An id of zero appends a contact. An existing id requires its current revision. */
int jaro_contact_book(struct jaro_contact_store *store, struct jaro_contact_text name,
	int create, int64_t *book_id, struct jaro_contact_error *error);
int jaro_contact_save(struct jaro_contact_store *store, const struct jaro_contact *contact,
	int64_t *id, struct jaro_contact_error *error);
int jaro_contact_generation(struct jaro_contact_store *store, int64_t *generation,
	struct jaro_contact_error *error);
int jaro_contact_has_mailbox(struct jaro_contact_store *store, int64_t book_id,
	struct jaro_contact_text mailbox, int *present, struct jaro_contact_error *error);

/* An empty query enumerates a book. A nonempty query matches a contiguous
 * folded substring within one searchable field. Callback data is borrowed. */
typedef int (*jaro_contact_visitor)(const struct jaro_contact *, void *);
int jaro_contact_visit(struct jaro_contact_store *store, int64_t book_id,
	struct jaro_contact_text query, jaro_contact_visitor visit, void *data,
	struct jaro_contact_error *error);

/* Emit a single legacy-compatible Mutt row. The selected mailbox is validated
 * and copied verbatim, while display values have controls replaced by spaces. */
int jaro_contact_write_mutt_row(FILE *out, struct jaro_contact_text mailbox,
	struct jaro_contact_text name, struct jaro_contact_text notes);

#endif
