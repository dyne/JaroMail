#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "dedup/inventory.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
emit(void *context, const struct jd_root *root, unsigned sub,
    const unsigned char *name, size_t length, const struct stat *metadata)
{
	size_t *count = context;
#ifdef __APPLE__
	struct timespec modified = metadata->st_mtimespec, changed = metadata->st_ctimespec;
#else
	struct timespec modified = metadata->st_mtim, changed = metadata->st_ctim;
#endif
	if (printf("%zu%c%u%c", root->rank, 0, sub, 0) < 0 ||
	    fwrite(name, 1, length, stdout) != length || putchar(0) == EOF ||
	    printf("%jd%c%ju%c%ju%c%jd%c%ld%c%jd%c%ld%c",
	    (intmax_t)metadata->st_size, 0, (uintmax_t)metadata->st_dev, 0,
	    (uintmax_t)metadata->st_ino, 0, (intmax_t)modified.tv_sec, 0,
	    modified.tv_nsec, 0, (intmax_t)changed.tv_sec, 0, changed.tv_nsec, 0) < 0)
		return -1;
	(*count)++;
	return 0;
}

int
main(int argc, char **argv)
{
	struct jd_inventory inventory;
	const char *mail_root = getenv("JAROMAILDIR");
	size_t count = 0;
	int offset = 1, pause = 0, result;
	while (offset < argc) {
		if (!strcmp(argv[offset], "--")) {
			offset++;
			break;
		}
		if (!strcmp(argv[offset], "--pause")) {
			pause = 1;
			offset++;
		} else if (!strcmp(argv[offset], "--mail-root") && offset + 1 < argc) {
			mail_root = argv[offset + 1];
			offset += 2;
		} else if (argv[offset][0] == '-') {
			fprintf(stderr, "inventory driver: invalid option\n");
			return 2;
		} else {
			break;
		}
	}
	if (mail_root && !*mail_root)
		mail_root = NULL;
	if (jd_preflight(&inventory, mail_root, (size_t)(argc - offset), argv + offset))
		return 1;
	if (pause) {
		fputs("preflight complete\n", stderr);
		fflush(stderr);
		if (getchar() != 'x') {
			jd_inventory_free(&inventory);
			return 1;
		}
	}
	result = jd_scan(&inventory, emit, &count);
	jd_inventory_free(&inventory);
	if (fflush(stdout) || ferror(stdout))
		result = -1;
	if (!result)
		fprintf(stderr, "inventory complete: %zu\n", count);
	return result ? 1 : 0;
}
