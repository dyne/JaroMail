/* Test-only barrier immediately before the private driver's no-replace publish. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

int renameat2(int oldfd, const char *oldname, int newfd, const char *newname, unsigned int flags)
{
	static int held;
	if (flags != RENAME_NOREPLACE || held)
		return syscall(SYS_renameat2, oldfd, oldname, newfd, newname, flags);

	char release;
	held = 1;
	int ready = atoi(getenv("SIEVE_TEST_READY_FD"));
	int go = atoi(getenv("SIEVE_TEST_GO_FD"));
	const char *after = getenv("SIEVE_TEST_AFTER_PUBLISH");
	bool published = after != NULL && *after == '1';
	if (published && syscall(SYS_renameat2, oldfd, oldname, newfd, newname, flags) < 0)
		return -1;
	if (dprintf(ready, "%ld\n", (long)getpid()) < 0 || read(go, &release, 1) != 1) {
		if (published)
			return 0;
		errno = EINTR;
		return -1;
	}
	return published ? 0 : syscall(SYS_renameat2, oldfd, oldname, newfd, newname, flags);
}

int unlink(const char *path)
{
	size_t len = strlen(path);
	const char *fail = getenv("SIEVE_TEST_EXPUNGE_ERROR");
	if (fail != NULL && *fail == '1' && len >= 6 && strcmp(path + len - 6, "/cur/0") == 0) {
		errno = EIO;
		return -1;
	}
	return syscall(SYS_unlinkat, AT_FDCWD, path, 0);
}
