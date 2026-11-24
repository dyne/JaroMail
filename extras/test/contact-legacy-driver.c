#define _POSIX_C_SOURCE 200809L

#include "contact/legacy-book.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void
hex(const char *text, size_t size)
{
	for (size_t i = 0; i < size; i++)
		printf("%02x", (unsigned char)text[i]);
}

static int
record(const struct jaro_legacy_record *rec, void *data)
{
	int dump = *(int *)data;
	printf("record %zu %zu %zu\n", rec->number, rec->line, rec->field_count);
	for (size_t i = 0; i < rec->field_count; i++) {
		const struct jaro_legacy_field *field = &rec->fields[i];
		printf("field %zu %zu %zu", field->line, field->key_size, field->value_size);
		if (dump) {
			putchar(' ');
			hex(field->key, field->key_size);
			putchar(' ');
			hex(field->value, field->value_size);
		}
		putchar('\n');
	}
	return 0;
}

static int
report(enum jaro_legacy_result status, const struct jaro_legacy_error *error)
{
	if (status != JARO_LEGACY_OK)
		fprintf(stderr, "%d %zu %zu %zu %d %s\n", status,
			error->line, error->record, error->field, error->system_error, error->reason);
	return status;
}

static int
read_failure(int dump)
{
	static const char partial[] = "[0]\nname=First\n[1]\nname=Second\nnotes=partial";
	struct jaro_legacy_error error;
	enum jaro_legacy_result status;
	int fds[2];
	FILE *stream;

	if (pipe(fds) || fcntl(fds[0], F_SETFL, O_NONBLOCK) ||
			write(fds[1], partial, sizeof partial - 1) != sizeof partial - 1) {
		perror("inject read failure");
		return 99;
	}
	stream = fdopen(fds[0], "r");
	if (!stream) {
		perror("fdopen read failure");
		return 99;
	}
	/* A drained, nonblocking pipe with its writer still open returns EAGAIN. */
	status = jaro_legacy_read(stream, record, NULL, &dump, &error);
	if (!ferror(stream) || feof(stream) || fclose(stream) || close(fds[1])) {
		fputs("expected a read error and an open borrowed stream\n", stderr);
		return 99;
	}
	return report(status, &error);
}

int
main(int argc, char **argv)
{
	struct jaro_legacy_error error;
	int dump = argc > 1 && !strcmp(argv[1], "--dump");
	int offset = dump ? 2 : 1;
	int exit_code = 0;

	if (argc <= offset)
		return 99;
	if (!strcmp(argv[offset], "--read-failure"))
		return read_failure(dump);
	for (int i = offset; i < argc; i++) {
		int status = report(jaro_legacy_read_at(AT_FDCWD, argv[i], record, &dump, &error), &error);
		if (status)
			exit_code = status;
	}
	return exit_code;
}
