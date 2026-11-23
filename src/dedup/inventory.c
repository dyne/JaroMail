#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64

#include "inventory.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const subdirs[] = {"cur", "new"};

static int
same_directory(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int
fail(const char *operation)
{
	fprintf(stderr, "dedup inventory: %s: %s\n", operation, strerror(errno));
	return -1;
}

static int
open_directory(int parent, const char *path, const struct stat *expected,
    struct stat *observed)
{
	int fd = openat(parent, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return fail("open directory");
	if (fstat(fd, observed) < 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return fail("stat directory");
	}
	if (expected && !same_directory(expected, observed)) {
		close(fd);
		errno = ESTALE;
		return fail("directory binding changed");
	}
	return fd;
}

void
jd_inventory_free(struct jd_inventory *inventory)
{
	size_t i;
	for (i = 0; i < inventory->count; i++)
		free(inventory->roots[i].path);
	free(inventory->roots);
	inventory->roots = NULL;
	inventory->count = 0;
}

static char *
resolve_input(const char *mail_root, const char *input)
{
	char *joined, *resolved;
	size_t prefix, length;
	if (!*input) {
		errno = EINVAL;
		return NULL;
	}
	if (!mail_root || strchr(input, '/') || !strcmp(input, ".") ||
	    !strcmp(input, ".."))
		return realpath(input, NULL);
	prefix = strlen(mail_root);
	length = strlen(input);
	if (prefix > SIZE_MAX - length - 2) {
		errno = ENOMEM;
		return NULL;
	}
	joined = malloc(prefix + length + 2);
	if (!joined)
		return NULL;
	memcpy(joined, mail_root, prefix);
	joined[prefix] = '/';
	memcpy(joined + prefix + 1, input, length + 1);
	resolved = realpath(joined, NULL);
	free(joined);
	return resolved;
}

int
jd_preflight(struct jd_inventory *inventory, const char *mail_root,
    size_t count, char *const *inputs)
{
	size_t i, j;
	int rootfd = -1, subfd = -1;
	struct jd_root root = {0};
	memset(inventory, 0, sizeof(*inventory));
	if (!count || count > SIZE_MAX / sizeof(*inventory->roots)) {
		errno = EINVAL;
		return fail("root count");
	}
	inventory->roots = calloc(count, sizeof(*inventory->roots));
	if (!inventory->roots)
		return fail("allocate roots");
	for (i = 0; i < count; i++) {
		int alias = 0;
		root.path = resolve_input(mail_root, inputs[i]);
		root.rank = i;
		if (!root.path) {
			fail("resolve root");
			goto error;
		}
		rootfd = open_directory(AT_FDCWD, root.path, NULL, &root.identity);
		if (rootfd < 0)
			goto error;
		for (j = 0; j < 2; j++) {
			subfd = open_directory(rootfd, subdirs[j], NULL, &root.subdirs[j]);
			if (subfd < 0)
				goto error;
			close(subfd);
			subfd = -1;
		}
		close(rootfd);
		rootfd = -1;
		if (same_directory(&root.subdirs[0], &root.subdirs[1]) ||
		    same_directory(&root.identity, &root.subdirs[0]) ||
		    same_directory(&root.identity, &root.subdirs[1]))
			goto overlap;
		for (j = 0; j < inventory->count; j++) {
			const struct jd_root *prior = &inventory->roots[j];
			unsigned a, b;
			for (a = 0; a < 2; a++)
				if (same_directory(&root.identity, &prior->subdirs[a]) ||
				    same_directory(&root.subdirs[a], &prior->identity))
					goto overlap;
			if (same_directory(&root.subdirs[0], &prior->subdirs[0]) &&
			    same_directory(&root.subdirs[1], &prior->subdirs[1])) {
				alias = 1;
				continue;
			}
			if (same_directory(&root.identity, &prior->identity))
				goto overlap;
			for (a = 0; a < 2; a++) {
				if (same_directory(&root.identity, &prior->subdirs[a]) ||
				    same_directory(&root.subdirs[a], &prior->identity))
					goto overlap;
				for (b = 0; b < 2; b++)
					if (same_directory(&root.subdirs[a], &prior->subdirs[b]))
						goto overlap;
			}
		}
		/* Retain aliases' root identities for all subsequent overlap checks. */
		root.alias = alias;
		inventory->roots[inventory->count++] = root;
		root.path = NULL;
	}
	return 0;
overlap:
	errno = EINVAL;
	fail("ambiguous directory overlap");
error:
	if (subfd >= 0)
		close(subfd);
	if (rootfd >= 0)
		close(rootfd);
	free(root.path);
	jd_inventory_free(inventory);
	return -1;
}

static int
verify_bindings(const struct jd_root *root)
{
	struct stat observed;
	unsigned sub;
	int rootfd = open_directory(AT_FDCWD, root->path, &root->identity, &observed);
	if (rootfd < 0)
		return -1;
	for (sub = 0; sub < 2; sub++) {
		int fd = open_directory(rootfd, subdirs[sub], &root->subdirs[sub], &observed);
		if (fd < 0) {
			close(rootfd);
			return -1;
		}
		close(fd);
	}
	close(rootfd);
	return 0;
}

int
jd_open_subdir(const struct jd_root *root, unsigned sub, int *parent)
{
	struct stat observed;
	int fd, rootfd = open_directory(AT_FDCWD, root->path, &root->identity, &observed);
	if (rootfd < 0)
		return -1;
	fd = open_directory(rootfd, subdirs[sub], &root->subdirs[sub], &observed);
	if (fd < 0)
		close(rootfd);
	else
		*parent = rootfd;
	return fd;
}

int
jd_verify_subdir(const struct jd_root *root, unsigned sub, int parent)
{
	struct stat observed;
	if (stat(root->path, &observed) < 0)
		return fail("stat root binding");
	if (!same_directory(&root->identity, &observed))
		goto changed;
	if (fstatat(parent, subdirs[sub], &observed, 0) < 0)
		return fail("stat subdirectory binding");
	if (same_directory(&root->subdirs[sub], &observed))
		return 0;
changed:
	errno = ESTALE;
	return fail("directory binding changed");
}

int
jd_scan(const struct jd_inventory *inventory, jd_emit emit, void *context)
{
	size_t i;
	for (i = 0; i < inventory->count; i++) {
		const struct jd_root *root = &inventory->roots[i];
		struct stat observed;
		unsigned sub;
		if (root->alias) {
			if (verify_bindings(root))
				return -1;
			continue;
		}
		int rootfd = open_directory(AT_FDCWD, root->path, &root->identity, &observed);
		if (rootfd < 0)
			return -1;
		for (sub = 0; sub < 2; sub++) {
			DIR *directory;
			struct dirent *entry;
			int fd = open_directory(rootfd, subdirs[sub], &root->subdirs[sub], &observed);
			if (fd < 0) {
				close(rootfd);
				return -1;
			}
			directory = fdopendir(fd);
			if (!directory) {
				int saved = errno;
				close(fd);
				close(rootfd);
				errno = saved;
				return fail("open directory stream");
			}
			for (;;) {
				errno = 0;
				entry = readdir(directory);
				if (!entry) {
					if (errno)
						goto scan_error;
					break;
				}
				if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
					continue;
				if (fstatat(fd, entry->d_name, &observed, AT_SYMLINK_NOFOLLOW) < 0)
					goto scan_error;
				if (!S_ISREG(observed.st_mode))
					continue;
				if (observed.st_size < 0) {
					errno = EOVERFLOW;
					goto scan_error;
				}
				if (emit(context, root, sub, (const unsigned char *)entry->d_name,
				    strlen(entry->d_name), &observed)) {
					closedir(directory);
					close(rootfd);
					return -1;
				}
			}
			if (closedir(directory) < 0) {
				close(rootfd);
				return fail("close directory stream");
			}
			continue;
scan_error:
			{
				int saved = errno;
				closedir(directory);
				close(rootfd);
				errno = saved;
				return fail("enumerate entry");
			}
		}
		close(rootfd);
		if (verify_bindings(root))
			return -1;
	}
	return 0;
}
