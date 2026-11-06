#ifndef JARO_FILTER_GENERATION_H
#define JARO_FILTER_GENERATION_H
#include "snapshot.h"
struct jf_generation {
	int directory;
	char id[65];
	struct jf_bytes cache,zsh,sieve;
};
struct jf_compile_result { char id[65]; char *directory; int changed; };
/* Read-only; takes shared lock, rejects unfinished journals and pins a verified
 * immutable directory. Every subsequent open is relative to directory. */
int jf_generation_pin(const char *, const char *, struct jf_generation *, struct jf_error *);
int jf_generation_open(const struct jf_generation *, const char *);
void jf_generation_close(struct jf_generation *);
int jf_compile(const char *, const char *, const char *, const char *, int,
	struct jf_compile_result *, struct jf_error *, char **);
int jf_compile_import(const char *, const char *, const char *, const char *,
	const char *, int, int, struct jf_compile_result *, struct jf_error *, char **);
void jf_compile_result_free(struct jf_compile_result *);
/* Private legacy-export bridge: recover under the same exclusive lock and
 * exec the fixed wrapper command with its lock descriptor still open. */
int jf_generation_legacy_run(const char *, char *const []);
#ifdef JF_TESTING
int jf_generation_test_hook(const char *);
void jf_generation_test_parallel(int);
void jf_generation_test_job(size_t);
void jf_generation_test_done(size_t);
#endif
#endif
