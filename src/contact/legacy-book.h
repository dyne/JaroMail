#ifndef JARO_LEGACY_BOOK_H
#define JARO_LEGACY_BOOK_H

#include <stdio.h>

/* Physical line bytes exclude LF. Source bytes include line endings. */
#define JARO_LEGACY_LINE_MAX (64u * 1024u)
#define JARO_LEGACY_SOURCE_MAX (64u * 1024u * 1024u)
#define JARO_LEGACY_FIELDS_MAX 128u

enum jaro_legacy_result {
	JARO_LEGACY_OK,
	JARO_LEGACY_IO,
	JARO_LEGACY_LIMIT,
	JARO_LEGACY_INVALID,
	JARO_LEGACY_MEMORY,
	JARO_LEGACY_CONSUMER
};

struct jaro_legacy_error {
	const char *reason; /* static diagnostic, independent of source text */
	size_t line, record, field; /* one-based; zero when unavailable */
	int system_error; /* errno, when relevant */
};

struct jaro_legacy_field {
	const char *key, *value;
	size_t key_size, value_size, line;
};

struct jaro_legacy_record {
	size_t number, line, field_count;
	const struct jaro_legacy_field *fields;
};

/* Record/field storage is borrowed during the callback. Return nonzero to stop.
 * Consumers that write must roll back all emitted records if the read fails,
 * including a failure reported while closing a path opened by read_at. */
typedef int (*jaro_legacy_consumer)(const struct jaro_legacy_record *, void *);
/* An optional observer receives borrowed, unmodified input bytes in order. */
typedef void (*jaro_legacy_bytes)(const char *, size_t, void *);
enum jaro_legacy_result jaro_legacy_read(FILE *stream,
	jaro_legacy_consumer consume, jaro_legacy_bytes observe, void *data,
	struct jaro_legacy_error *error);
enum jaro_legacy_result jaro_legacy_read_at(int dir_fd, const char *name,
	jaro_legacy_consumer consume, void *data, struct jaro_legacy_error *error);

/* Read borrows the stream. The caller frees reader.line once done. */
struct jaro_legacy_line_reader {
	char *line;
	size_t length, capacity, source_size;
	int ended_with_lf;
};

enum jaro_legacy_line_result {
	JARO_LEGACY_LINE_OK,
	JARO_LEGACY_LINE_EOF,
	JARO_LEGACY_LINE_IO,
	JARO_LEGACY_LINE_LIMIT,
	JARO_LEGACY_LINE_MEMORY
};

enum jaro_legacy_line_result jaro_legacy_line_read(FILE *stream,
	struct jaro_legacy_line_reader *reader);

#endif
