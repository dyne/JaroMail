#ifndef JARO_FILTER_POOL_H
#define JARO_FILTER_POOL_H
#include "storage.h"
#define JF_POOL_WORKERS 16u
#define JF_POOL_WINDOW (2u * JF_POOL_WORKERS)
struct jf_pool;
typedef void (*jf_pool_run)(void *,struct jf_storage *);
/* A failed start joins all threads and restores the caller's signal mask.
 * It has no filesystem effects, so the caller can use singleton execution. */
struct jf_pool *jf_pool_start(void);
void jf_pool_bind(struct jf_pool *,const struct jf_storage *);
void jf_pool_window(struct jf_pool *,void *const *,size_t,jf_pool_run);
int jf_pool_stopped(void *);
void jf_pool_stop(struct jf_pool *);
int jf_pool_finish(struct jf_pool *);
#endif
