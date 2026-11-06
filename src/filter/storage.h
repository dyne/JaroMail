#ifndef JARO_FILTER_STORAGE_H
#define JARO_FILTER_STORAGE_H
#include <sys/stat.h>
#include <signal.h>
#include "generation.h"

struct jf_storage {
	int root,engine,lock,random;
	struct stat root_identity,lock_identity;
	unsigned char nonce[16];
	size_t sequence;
	volatile sig_atomic_t *interrupted;
	const char *root_path; /* Borrowed until close. */
	int (*should_stop)(void *);
	int (*destination_allowed)(void *,int,int);
	void *context;
	int borrowed; /* Worker views never own/close the shared descriptors. */
};
struct jf_storage_recovery { char *source,*destination,*temporary,*published; };
/* Opens the same root/generation as the pinned reader, then obtains a persistent
 * batch lock. No lock inode is unlinked. Returns global 3/5/6 before mail writes. */
int jf_storage_begin(const char *,const struct jf_generation *,volatile sig_atomic_t *,
	struct jf_storage *,const char **);
void jf_storage_close(struct jf_storage *);
/* Uses pinned parent/file descriptors, never reopens source through its path.
 * 0=moved, 1=noop, 3=resource failure, 7=recoverable message error. Recovery paths are allocated
 * exact raw paths to retained source/destination/tmp entries; caller frees each.
 * Maildir messages are immutable after delivery; concurrent namespace changes
 * are supported, and observed in-place edits fail rather than delete that data. */
int jf_storage_move(struct jf_storage *,int,const char *,int,const struct stat *,
	size_t,const char *,const char *,const char *,struct jf_storage_recovery *,const char **);
int jf_storage_directory(const char *);
int jf_storage_same(const struct stat *,const struct stat *);
#endif
