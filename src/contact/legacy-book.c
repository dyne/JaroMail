#define _POSIX_C_SOURCE 200809L

#include "legacy-book.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum jaro_legacy_line_result
jaro_legacy_line_read(FILE *stream, struct jaro_legacy_line_reader *reader)
{
	int c;

	reader->length = 0;
	reader->ended_with_lf = 0;
	while ((c = fgetc(stream)) != EOF) {
		if (reader->source_size == JARO_LEGACY_SOURCE_MAX)
			return JARO_LEGACY_LINE_LIMIT;
		reader->source_size++;
		if (c == '\n') {
			reader->ended_with_lf = 1;
			break;
		}
		if (reader->length == JARO_LEGACY_LINE_MAX)
			return JARO_LEGACY_LINE_LIMIT;
		if (reader->length + 1 >= reader->capacity) {
			size_t capacity = reader->capacity ? reader->capacity * 2 : 256;
			char *line;

			if (capacity > JARO_LEGACY_LINE_MAX + 1)
				capacity = JARO_LEGACY_LINE_MAX + 1;
			line = realloc(reader->line, capacity);
			if (!line)
				return JARO_LEGACY_LINE_MEMORY;
			reader->line = line;
			reader->capacity = capacity;
		}
		reader->line[reader->length++] = (char)c;
	}
	if (c == EOF && ferror(stream))
		return JARO_LEGACY_LINE_IO;
	if (c == EOF && !reader->length)
		return JARO_LEGACY_LINE_EOF;
	if (reader->line)
		reader->line[reader->length] = '\0';
	return JARO_LEGACY_LINE_OK;
}

static enum jaro_legacy_result
failure(struct jaro_legacy_error *error, enum jaro_legacy_result result,
	const char *reason, size_t line, size_t record, size_t field, int system_error)
{
	if (error)
		*error = (struct jaro_legacy_error){reason, line, record, field, system_error};
	return result;
}

static void
clear_record(struct jaro_legacy_record *record, char **allocations)
{
	for (size_t i = 0; i < record->field_count; i++)
		free(allocations[i]);
	record->field_count = 0;
}

static int
contact_section(const char *line, size_t size)
{
	if (size < 3 || line[size - 1] != ']')
		return 0;
	for (size_t i = 1; i < size - 1; i++)
		if (line[i] < '0' || line[i] > '9')
			return 0;
	return 1;
}

static int
key_equals(const char *key, size_t size, const char *literal)
{
	return size == strlen(literal) && !memcmp(key, literal, size);
}

static enum jaro_legacy_result
emit_record(const struct jaro_legacy_record *record, size_t name_field,
	jaro_legacy_consumer consume, void *data, struct jaro_legacy_error *error)
{
	if (!name_field)
		return failure(error, JARO_LEGACY_INVALID, "missing name field",
			record->line, record->number, 0, 0);
	if (!record->fields[name_field - 1].value_size)
		return failure(error, JARO_LEGACY_INVALID, "empty name field",
			record->fields[name_field - 1].line, record->number, name_field, 0);
	if (consume(record, data))
		return failure(error, JARO_LEGACY_CONSUMER, "record consumer failed",
			record->line, record->number, 0, 0);
	return JARO_LEGACY_OK;
}

enum jaro_legacy_result
jaro_legacy_read(FILE *stream, jaro_legacy_consumer consume, jaro_legacy_bytes observe, void *data,
	struct jaro_legacy_error *error)
{
	struct jaro_legacy_line_reader reader = {0};
	struct jaro_legacy_field fields[JARO_LEGACY_FIELDS_MAX];
	char *allocations[JARO_LEGACY_FIELDS_MAX];
	struct jaro_legacy_record record = {.fields = fields};
	enum jaro_legacy_result result = JARO_LEGACY_OK;
	size_t line_number = 0, name_field = 0;
	int format = 0;

	if (error)
		*error = (struct jaro_legacy_error){0};
	for (;;) {
		enum jaro_legacy_line_result next = jaro_legacy_line_read(stream, &reader);
		const char *line = reader.line;
		size_t end = reader.length, start = 0, trimmed_end;
		const char *equals, *reason = NULL;
		size_t key_size, slot = record.field_count;
		char *field;

		if (next == JARO_LEGACY_LINE_EOF) {
			if (record.number)
				result = emit_record(&record, name_field, consume, data, error);
			break;
		}
		line_number++;
		if (next == JARO_LEGACY_LINE_IO) {
			result = failure(error, JARO_LEGACY_IO, "cannot read source",
				line_number, record.number, record.number && end ? slot + 1 : 0, errno);
			break;
		}
		if (next == JARO_LEGACY_LINE_LIMIT) {
			result = failure(error, JARO_LEGACY_LIMIT, "source or line byte limit exceeded",
				line_number, record.number, record.number && end ? slot + 1 : 0, 0);
			break;
		}
		if (next == JARO_LEGACY_LINE_MEMORY) {
			result = failure(error, JARO_LEGACY_MEMORY, "cannot allocate source line",
				line_number, record.number, 0, errno);
			break;
		}
		if (observe) {
			if (end)
				observe(line, end, data);
			if (reader.ended_with_lf)
				observe("\n", 1, data);
		}
		if (reader.ended_with_lf && end && line[end - 1] == '\r')
			end--;
		while (start < end && (line[start] == ' ' || line[start] == '\t'))
			start++;
		if (start == end || line[start] == '#')
			continue;
		trimmed_end = end;
		while (trimmed_end > start && (line[trimmed_end - 1] == ' ' ||
				line[trimmed_end - 1] == '\t'))
			trimmed_end--;
		if (line[start] == '[') {
			if (!format && !record.number &&
					key_equals(line + start, trimmed_end - start, "[format]")) {
				format = 1;
				continue;
			}
			if (!contact_section(line + start, trimmed_end - start)) {
				result = failure(error, JARO_LEGACY_INVALID, "unsupported section",
					line_number, record.number + 1, 0, 0);
				break;
			}
			if (record.number) {
				result = emit_record(&record, name_field, consume, data, error);
				if (result != JARO_LEGACY_OK)
					break;
				clear_record(&record, allocations);
			}
			record.number++;
			record.line = line_number;
			name_field = 0;
			continue;
		}

		equals = memchr(line, '=', end);
		key_size = equals ? (size_t)(equals - line) : 0;
		if (memchr(line, 0, end))
			reason = "embedded NUL in field";
		else if (!key_size || start >= key_size)
			reason = "expected a field assignment";
		else if (!record.number) {
			size_t key_end = key_size;
			while (key_end > start && (line[key_end - 1] == ' ' ||
					line[key_end - 1] == '\t'))
				key_end--;
			if (!format || (!key_equals(line + start, key_end - start, "program") &&
					!key_equals(line + start, key_end - start, "version")))
				reason = "unsupported field outside a contact record";
		}
		if (!reason && record.number) {
			for (size_t i = 0; i < slot; i++)
				if (key_size == fields[i].key_size && !memcmp(line, fields[i].key, key_size))
					reason = "duplicate field";
		}
		if (reason) {
			result = failure(error, JARO_LEGACY_INVALID, reason, line_number,
				record.number, record.number ? slot + 1 : 0, 0);
			break;
		}
		if (!record.number)
			continue;
		if (slot == JARO_LEGACY_FIELDS_MAX) {
			result = failure(error, JARO_LEGACY_LIMIT, "contact field count limit exceeded",
				line_number, record.number, slot + 1, 0);
			break;
		}
		field = malloc(end + 1);
		if (!field) {
			result = failure(error, JARO_LEGACY_MEMORY, "cannot allocate contact field",
				line_number, record.number, slot + 1, errno);
			break;
		}
		memcpy(field, line, end);
		field[key_size] = field[end] = '\0';
		allocations[slot] = field;
		fields[slot] = (struct jaro_legacy_field){
			field, field + key_size + 1, key_size, end - key_size - 1, line_number
		};
		if (key_equals(field, key_size, "name"))
			name_field = slot + 1;
		record.field_count++;
	}
	clear_record(&record, allocations);
	free(reader.line);
	return result;
}

enum jaro_legacy_result
jaro_legacy_read_at(int dir_fd, const char *name, jaro_legacy_consumer consume,
	void *data, struct jaro_legacy_error *error)
{
	int fd = openat(dir_fd, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	struct stat statbuf;
	FILE *stream;
	enum jaro_legacy_result result;

	if (fd < 0)
		return failure(error, JARO_LEGACY_IO, "cannot open source", 0, 0, 0, errno);
	if (fstat(fd, &statbuf) < 0) {
		int saved = errno;
		close(fd);
		return failure(error, JARO_LEGACY_IO, "cannot inspect source", 0, 0, 0, saved);
	}
	if (!S_ISREG(statbuf.st_mode)) {
		close(fd);
		return failure(error, JARO_LEGACY_INVALID, "expected a regular source file", 0, 0, 0, 0);
	}
	stream = fdopen(fd, "r");
	if (!stream) {
		int saved = errno;
		close(fd);
		return failure(error, JARO_LEGACY_IO, "cannot open source stream", 0, 0, 0, saved);
	}
	result = jaro_legacy_read(stream, consume, NULL, data, error);
	if (fclose(stream) && result == JARO_LEGACY_OK)
		return failure(error, JARO_LEGACY_IO, "cannot close source", 0, 0, 0, errno);
	return result;
}
