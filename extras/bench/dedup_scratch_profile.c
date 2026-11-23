#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "dedup/profile.h"
#include "sqlite3.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Only the private benchmark connection uses SQLite's Unix VFS. Between
 * close, truncate and unlink calls the logical total can only grow. Observe
 * it BEFORE each possible decrease, including anonymous sort/temp files. */
uint64_t jd_scratch_peak;
static const char *scratch;
static int (*original_close)(int);
static int (*original_unlink)(const char *);
static int (*original_truncate)(int, off_t);

static void
sample_before_shrink(void)
{
	struct dirent *entry;
	struct stat st;
	struct identity { dev_t device; ino_t inode; };
	struct identity *anonymous = NULL;
	size_t count = 0;
	uint64_t bytes = 0;
	int saved = errno;
	size_t prefix = strlen(scratch);
	DIR *dir = opendir(scratch);
	if (!dir)
		abort();
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW))
			abort();
		if (S_ISREG(st.st_mode))
			bytes += (uint64_t)st.st_size;
	}
	closedir(dir);
	dir = opendir("/proc/self/fd");
	if (!dir)
		abort();
	while ((entry = readdir(dir))) {
		char target[PATH_MAX];
		ssize_t len;
		if (entry->d_name[0] == '.')
			continue;
		len = readlinkat(dirfd(dir), entry->d_name, target, sizeof(target));
		if (len < 0 || len == (ssize_t)sizeof(target))
			abort();
		if ((size_t)len <= prefix || memcmp(target, scratch, prefix) || target[prefix] != '/')
			continue;
		if (fstatat(dirfd(dir), entry->d_name, &st, 0))
			abort();
		if (!S_ISREG(st.st_mode) || st.st_nlink)
			continue;
		size_t i;
		for (i = 0; i < count; i++)
			if (anonymous[i].device == st.st_dev && anonymous[i].inode == st.st_ino)
				break;
		if (i == count) {
			struct identity *more = realloc(anonymous, (count + 1) * sizeof(*anonymous));
			if (!more)
				abort();
			anonymous = more;
			anonymous[count++] = (struct identity){st.st_dev, st.st_ino};
			bytes += (uint64_t)st.st_size;
		}
	}
	closedir(dir);
	free(anonymous);
	if (bytes > jd_scratch_peak)
		jd_scratch_peak = bytes;
	errno = saved;
}

static int watched_close(int fd)
{
	sample_before_shrink();
	return original_close(fd);
}

static int watched_unlink(const char *path)
{
	sample_before_shrink();
	return original_unlink(path);
}

static int watched_truncate(int fd, off_t length)
{
	sample_before_shrink();
	return original_truncate(fd, length);
}

void
jd_watch_scratch(const char *directory)
{
	sqlite3_vfs *vfs = sqlite3_vfs_find("unix");
	if (!vfs || vfs->iVersion < 3)
		abort();
	scratch = directory;
	original_close = (int (*)(int))vfs->xGetSystemCall(vfs, "close");
	original_unlink = (int (*)(const char *))vfs->xGetSystemCall(vfs, "unlink");
	original_truncate = (int (*)(int, off_t))vfs->xGetSystemCall(vfs, "ftruncate");
	if (!original_close || !original_unlink || !original_truncate ||
	    vfs->xSetSystemCall(vfs, "close", (sqlite3_syscall_ptr)watched_close) != SQLITE_OK ||
	    vfs->xSetSystemCall(vfs, "unlink", (sqlite3_syscall_ptr)watched_unlink) != SQLITE_OK ||
	    vfs->xSetSystemCall(vfs, "ftruncate", (sqlite3_syscall_ptr)watched_truncate) != SQLITE_OK)
		abort();
}
