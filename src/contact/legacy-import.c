#include "contact/legacy-import.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int
field_is(const struct jaro_legacy_field *field, const char *name)
{
	return field->key_size == strlen(name) && !memcmp(field->key, name, field->key_size);
}

int
jaro_contact_split_mailboxes(struct jaro_contact_text list, struct jaro_contact_text **output, size_t *count)
{
	size_t start = 0, capacity = 0;
	int quoted = 0;

	for (size_t i = 0; i <= list.size; i++) {
		if (i == list.size || (list.data[i] == ',' && !quoted)) {
			size_t end = i;
			while (start < end && (list.data[start] == ' ' || list.data[start] == '\t'))
				start++;
			while (end > start && (list.data[end - 1] == ' ' || list.data[end - 1] == '\t'))
				end--;
			if (end > start) {
				if (*count == JARO_CONTACT_ADDRESSES_MAX)
					return JARO_CONTACT_INVALID;
				if (*count == capacity) {
					size_t next = capacity ? 2 * capacity : 8;
					struct jaro_contact_text *items = realloc(*output, next * sizeof *items);
					if (!items)
						return JARO_CONTACT_STORAGE;
					*output = items;
					capacity = next;
				}
				(*output)[(*count)++] = (struct jaro_contact_text){list.data + start, end - start};
			}
			start = i + 1;
		} else if (quoted && list.data[i] == '\\' && i + 1 < list.size) {
			i++;
		} else if (list.data[i] == '"') {
			quoted = !quoted;
		}
	}
	return quoted ? JARO_CONTACT_INVALID : JARO_CONTACT_OK;
}

int
jaro_contact_import_record(struct jaro_contact_store *store, int64_t book,
	const struct jaro_legacy_record *source, jaro_contact_visitor observe, void *data,
	struct jaro_contact_import_error *error)
{
	struct jaro_contact_text *addresses = NULL;
	struct jaro_contact_field fields[JARO_LEGACY_FIELDS_MAX];
	size_t field_ordinals[JARO_LEGACY_FIELDS_MAX];
	size_t name = 0, nickname = 0, notes = 0, email = 0, bad = 0;
	struct jaro_contact contact = {.book_id = book, .fields = fields};
	struct jaro_contact_error detail = {0};
	int64_t id;
	int status = JARO_CONTACT_OK;

	if (error)
		*error = (struct jaro_contact_import_error){0};
	if (source->field_count > JARO_LEGACY_FIELDS_MAX) {
		status = JARO_CONTACT_INVALID;
		detail = (struct jaro_contact_error){"legacy field count exceeded", NULL, 0, 0};
		goto done;
	}
	for (size_t i = 0; i < source->field_count; i++) {
		const struct jaro_legacy_field *field = &source->fields[i];
		struct jaro_contact_text value = {field->value, field->value_size};
		if (field_is(field, "name")) {
			name = i + 1;
			contact.name = value;
		} else if (field_is(field, "nick")) {
			nickname = i + 1;
			contact.nickname = value;
		} else if (field_is(field, "notes")) {
			notes = i + 1;
			contact.notes = value;
		} else if (field_is(field, "email")) {
			email = i + 1;
			status = jaro_contact_split_mailboxes(value, &addresses, &contact.address_count);
			if (status) {
				bad = email;
				detail = (struct jaro_contact_error){"cannot split legacy mailboxes", "email", 0,
					status == JARO_CONTACT_STORAGE ? ENOMEM : 0};
				goto done;
			}
		} else {
			size_t slot = contact.field_count++;
			fields[slot] = (struct jaro_contact_field){{field->key, field->key_size}, value};
			field_ordinals[slot] = i + 1;
		}
	}
	contact.addresses = addresses;
	status = jaro_contact_save(store, &contact, &id, &detail);
	if (!status && observe && observe(&contact, data)) {
		status = JARO_CONTACT_CONSUMER;
		detail.reason = "cannot verify mapped contact";
	}
	if (status && detail.field) {
		if (!strcmp(detail.field, "name"))
			bad = name;
		else if (!strcmp(detail.field, "nickname"))
			bad = nickname;
		else if (!strcmp(detail.field, "notes"))
			bad = notes;
		else if (!strcmp(detail.field, "email"))
			bad = email;
		else if (detail.ordinal && detail.ordinal <= contact.field_count)
			bad = field_ordinals[detail.ordinal - 1];
	}
done:
	free(addresses);
	if (status && error)
		*error = (struct jaro_contact_import_error){
			detail, source->number, bad ? source->fields[bad - 1].line : source->line, bad
		};
	return status;
}
