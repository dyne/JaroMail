#ifndef JARO_DEDUP_INVENTORY_H
#define JARO_DEDUP_INVENTORY_H

#include <stddef.h>
#include <sys/stat.h>

struct jd_root {
	char *path;
	struct stat identity;
	struct stat subdirs[2];
	size_t rank;
	int alias;
};

struct jd_inventory {
	struct jd_root *roots;
	size_t count;
};

/* name is borrowed, length-delimited, and valid only during the callback.
 * subdir 0 is cur, 1 is new. Nonzero callbacks abort the entire inventory. */
typedef int (*jd_emit)(void *, const struct jd_root *, unsigned,
    const unsigned char *, size_t, const struct stat *);

/* Bare names resolve below mail_root when supplied; paths containing '/' and
 * '.'/'..' are explicit. Roots/cur/new may alias directories, message symlinks
 * are never followed. The caller owns and must free the preflight result. */
int jd_preflight(struct jd_inventory *, const char *, size_t, char *const *);
int jd_scan(const struct jd_inventory *, jd_emit, void *);
/* Reopen and verify the root and cur/new binding. The caller owns both fds. */
int jd_open_subdir(const struct jd_root *, unsigned, int *);
int jd_verify_subdir(const struct jd_root *, unsigned, int);
void jd_inventory_free(struct jd_inventory *);

#endif
