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
#include <sys/file.h>
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
	int saved = errno;
	fprintf(stderr, "dedup inventory: %s: %s\n", operation, strerror(saved));
	errno = saved;
	return -1;
}

int
jd_entry_conflict(int error)
{
	return error == ENOENT || error == ENOTDIR || error == ELOOP ||
	    error == ESTALE || error == EACCES || error == EPERM || error == ENXIO ||
	    error == ENODEV || error == EISDIR;
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
	for (i = 0; i < inventory->count; i++) {
		free(inventory->roots[i].path);
		free(inventory->roots[i].operand);
	}
	free(inventory->roots);
	inventory->roots = NULL;
	inventory->count = 0;
}

static char *
input_path(const char *mail_root, const char *input)
{
	char *joined;
	size_t prefix, length;
	if (!*input) {
		errno = EINVAL;
		return NULL;
	}
	if (!mail_root || strchr(input, '/') || !strcmp(input, ".") ||
	    !strcmp(input, ".."))
		return strdup(input);
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
	return joined;
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
		root.operand = input_path(mail_root, inputs[i]);
		root.path = root.operand ? realpath(root.operand, NULL) : NULL;
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
		root.operand = NULL;
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
	free(root.operand);
	jd_inventory_free(inventory);
	return -1;
}

static int
verify_operand(const struct jd_root *root)
{
	struct stat observed;
	if (!strcmp(root->operand, root->path))
		return 0;
	if (stat(root->operand, &observed) < 0)
		return fail("stat input binding");
	if (same_directory(&root->identity, &observed))
		return 0;
	errno = ESTALE;
	return fail("input binding changed");
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
	return verify_operand(root);
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
		return verify_operand(root);
changed:
	errno = ESTALE;
	return fail("directory binding changed");
}

struct lock_target {
	const struct jd_root *root;
	const struct stat *identity;
	int sub; /* -1 selects the root itself */
};

/* Check the physical ancestors of scratch, including cur/new aliases. stat
 * needs search permission on ancestors, without requiring directory reads. */
int
jd_check_scratch_parent(const struct jd_inventory *inventory, const char *path)
{
	struct stat current;
	char *resolved = realpath(path, NULL), *slash;
	int result = -1;
	if (!resolved)
		return fail("resolve scratch directory");
	for (;;) {
		size_t i;
		if (stat(resolved, &current) < 0) {
			fail("stat scratch ancestor");
			break;
		}
		for (i = 0; i < inventory->count; i++) {
			const struct jd_root *root = &inventory->roots[i];
			if (same_directory(&current, &root->identity) ||
			    same_directory(&current, &root->subdirs[0]) ||
			    same_directory(&current, &root->subdirs[1])) {
				fputs("dedup: choose a scratch directory outside the input Maildirs\n", stderr);
				goto done;
			}
		}
		if (!strcmp(resolved, "/")) {
			result = 0;
			break;
		}
		slash = strrchr(resolved, '/');
		if (slash == resolved)
			slash++;
		*slash = '\0';
	}
done:
	free(resolved);
	return result;
}

static int
physical_order(const void *left, const void *right)
{
	const struct lock_target *a = left, *b = right;
	dev_t ad = a->identity->st_dev, bd = b->identity->st_dev;
	ino_t ai = a->identity->st_ino, bi = b->identity->st_ino;
	if (ad != bd)
		return ad < bd ? -1 : 1;
	return (ai > bi) - (ai < bi);
}

void
jd_unlock_inventory(struct jd_locks *locks)
{
	while (locks->count)
		close(locks->fds[--locks->count]);
	free(locks->fds);
	locks->fds = NULL;
}

int
jd_lock_inventory(const struct jd_inventory *inventory, struct jd_locks *locks)
{
	struct lock_target *targets;
	size_t i, count;
	*locks = (struct jd_locks){0};
	if (inventory->count > SIZE_MAX / 3 / sizeof(*targets)) {
		errno = ENOMEM;
		return fail("allocate lock targets");
	}
	count = 3 * inventory->count;
	targets = calloc(count, sizeof(*targets));
	locks->fds = calloc(count, sizeof(*locks->fds));
	if (!targets || !locks->fds) {
		fail("allocate lock targets");
		goto error;
	}
	for (i = 0; i < inventory->count; i++) {
		const struct jd_root *root = &inventory->roots[i];
		targets[3*i] = (struct lock_target){root, &root->identity, -1};
		targets[3*i+1] = (struct lock_target){root, &root->subdirs[0], 0};
		targets[3*i+2] = (struct lock_target){root, &root->subdirs[1], 1};
	}
	qsort(targets, count, sizeof(*targets), physical_order);
	for (i = 0; i < count; i++) {
		struct lock_target *target = &targets[i];
		struct stat observed;
		int fd, parent;
		if (i && physical_order(target-1, target) == 0)
			continue;
		if (target->sub < 0) {
			fd = open_directory(AT_FDCWD, target->root->path, target->identity, &observed);
		} else {
			fd = jd_open_subdir(target->root, (unsigned)target->sub, &parent);
			if (fd >= 0)
				close(parent);
		}
		if (fd < 0)
			goto error;
		locks->fds[locks->count++] = fd;
		if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
			fail("acquire apply lock");
			goto error;
		}
	}
	free(targets);
	return 0;
error:
	free(targets);
	jd_unlock_inventory(locks);
	return -1;
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
