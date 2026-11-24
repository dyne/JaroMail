#include "contact/text.h"
#include "mailbox.h"

#include <stdlib.h>
#include <string.h>

int
jaro_contact_valid_text(struct jaro_contact_text text)
{
	return text.size <= JARO_CONTACT_TEXT_MAX && (!text.size ||
		(text.data && jf_valid_utf8((const unsigned char *)text.data, text.size)));
}

/* Byte widths of UTF-8 terminal controls at the current position. */
static size_t
control_size(const unsigned char *bytes, size_t left)
{
	unsigned c = bytes[0];
	if (c < 32 || c == 127)
		return 1;
	if (left >= 2 && c == 0xc2 && bytes[1] >= 0x80 && bytes[1] <= 0x9f)
		return 2;
	if (left >= 3 && c == 0xe2) {
		if (bytes[1] == 0x80 && (bytes[2] == 0x8e || bytes[2] == 0x8f ||
				(bytes[2] >= 0xa8 && bytes[2] <= 0xae)))
			return 3;
		if (bytes[1] == 0x81 && (bytes[2] == 0xa0 ||
				(bytes[2] >= 0xa6 && bytes[2] <= 0xa9)))
			return 3;
	}
	if (left >= 2 && c == 0xd8 && bytes[1] == 0x9c) /* Arabic letter mark */
		return 2;
	return 0;
}

int
jaro_contact_mailbox_key(struct jaro_contact_text text, char **key)
{
	char *value;

	*key = NULL;
	if (!text.size || !jaro_contact_valid_text(text))
		return JARO_CONTACT_INVALID;
	for (size_t i = 0; i < text.size; i++)
		if (control_size((const unsigned char *)text.data + i, text.size - i))
			return JARO_CONTACT_INVALID;
	value = malloc(text.size + 1);
	if (!value)
		return JARO_CONTACT_STORAGE;
	memcpy(value, text.data, text.size);
	value[text.size] = '\0';
	if (!jf_normalize_mailbox(value)) {
		free(value);
		return JARO_CONTACT_INVALID;
	}
	for (char *p = value; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += 'a' - 'A';
	*key = value;
	return JARO_CONTACT_OK;
}

int
jaro_contact_print_text(FILE *out, struct jaro_contact_text text)
{
	for (size_t i = 0; i < text.size;) {
		size_t control = control_size((const unsigned char *)text.data + i, text.size - i);
		if (fputc(control ? ' ' : (unsigned char)text.data[i], out) == EOF)
			return -1;
		i += control ? control : 1;
	}
	return 0;
}

char *
jaro_contact_display_text(struct jaro_contact_text text)
{
	char *display = malloc(text.size + 1);
	if (!display)
		return NULL;
	size_t out = 0;
	for (size_t i = 0; i < text.size;) {
		size_t control = control_size((const unsigned char *)text.data + i, text.size - i);
		display[out++] = control ? ' ' : text.data[i];
		i += control ? control : 1;
	}
	display[out] = '\0';
	return display;
}

int
jaro_contact_write_mutt_row(FILE *out, struct jaro_contact_text mailbox,
	struct jaro_contact_text name, struct jaro_contact_text notes)
{
	char *key;
	int status = jaro_contact_mailbox_key(mailbox, &key);
	free(key);
	if (status != JARO_CONTACT_OK || !jaro_contact_valid_text(name) ||
			!jaro_contact_valid_text(notes))
		return -1;
	if (!notes.size)
		notes = (struct jaro_contact_text){" ", 1};
	if (fwrite(mailbox.data, 1, mailbox.size, out) != mailbox.size || fputc('\t', out) == EOF ||
			jaro_contact_print_text(out, name) || fputc('\t', out) == EOF ||
			jaro_contact_print_text(out, notes) || fputc('\n', out) == EOF)
		return -1;
	return 0;
}
