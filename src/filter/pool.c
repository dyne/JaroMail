#define _POSIX_C_SOURCE 200809L
#include "pool.h"
#include <pthread.h>
#include <sys/resource.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
struct worker { struct jf_pool *pool; unsigned index; struct jf_storage storage; };
struct jf_pool {
	pthread_mutex_t mutex;
	pthread_cond_t ready,settled;
	pthread_t workers[JF_POOL_WORKERS],signals;
	struct worker views[JF_POOL_WORKERS];
	sigset_t signals_set,old_mask;
	void *const *jobs;
	jf_pool_run run;
	size_t next,count,remaining;
	int stop,quit,signal;
	int (*destination_allowed)(void *,int,int);
	void *destination_context;
};

/* Caller owns mutex: consumption and stop publication form one observation.
 * A worker cannot pass its boundary while the manager has consumed a signal
 * but has not published cancellation. Zero-time waits never park this mutex.
 * Four standard managed signals can be pending both per-thread and process;
 * bound the drain even if a sender continually replenishes them. */
static void observe_signals(struct jf_pool *p)
{
	const struct timespec zero={0,0}; unsigned n; int saved=errno,received=0;
	for (n=0;n<8;n++) {
		int signal=sigtimedwait(&p->signals_set,NULL,&zero);
		if (signal<0) break;
		p->signal=signal; p->stop=1; received=1;
	}
	if (received) pthread_cond_broadcast(&p->ready);
	errno=saved;
}

int jf_pool_stopped(void *context)
{
	struct jf_pool *p=context; int result;
	pthread_mutex_lock(&p->mutex); observe_signals(p); result=p->stop; pthread_mutex_unlock(&p->mutex);
	return result;
}

static int destination_allowed(void *context,int cur,int newfd)
{
	struct jf_pool *p=context;
	return !p->destination_allowed || p->destination_allowed(p->destination_context,cur,newfd);
}

void jf_pool_stop(struct jf_pool *p)
{
	pthread_mutex_lock(&p->mutex); p->stop=1; pthread_cond_broadcast(&p->ready); pthread_mutex_unlock(&p->mutex);
}

static void *signals(void *context)
{
	struct jf_pool *p=context;
	const struct timespec interval={0,10000000};
	for (;;) {
		int quit;
		pthread_mutex_lock(&p->mutex);
		observe_signals(p);
		quit=p->quit;
		pthread_mutex_unlock(&p->mutex);
		if (quit) return NULL;
		(void)nanosleep(&interval,NULL);
	}
}

static void *worker(void *context)
{
	struct worker *w=context; struct jf_pool *p=w->pool;
	for (;;) {
		void *job; jf_pool_run run;
		pthread_mutex_lock(&p->mutex);
		while (!p->quit && p->next==p->count) pthread_cond_wait(&p->ready,&p->mutex);
		if (p->quit) { pthread_mutex_unlock(&p->mutex); return NULL; }
		job=p->jobs[p->next++]; run=p->run;
		pthread_mutex_unlock(&p->mutex);
		/* Cancelled jobs still run their cooperative entry/cleanup, which never
		 * starts a storage move after observing stop. Every job settles once. */
		run(job,&w->storage);
		pthread_mutex_lock(&p->mutex);
		if (!--p->remaining) pthread_cond_signal(&p->settled);
		pthread_mutex_unlock(&p->mutex);
	}
}

struct jf_pool *jf_pool_start(void)
{
	struct rlimit limit; DIR *fds; struct dirent *ent;
	struct jf_pool *p; pthread_attr_t attr; unsigned i,started=0; size_t open_count=0;
	if (getrlimit(RLIMIT_NOFILE,&limit)) return NULL;
	fds=opendir("/proc/self/fd"); if (!fds) return NULL;
	while ((ent=readdir(fds))) if (ent->d_name[0]!='.') open_count++;
	closedir(fds);
	/* 32 pinned source/parent pairs + 16 singleton moves (<=8 private FDs
	 * each), shared roots, and a 32 descriptor reserve. */
	if (limit.rlim_cur<open_count+JF_POOL_WINDOW*2+JF_POOL_WORKERS*8+32) return NULL;
	p=calloc(1,sizeof *p); if (!p) return NULL;
	if (pthread_mutex_init(&p->mutex,NULL)) { free(p); return NULL; }
	if (pthread_cond_init(&p->ready,NULL)) goto mutex_fail;
	if (pthread_cond_init(&p->settled,NULL)) goto ready_fail;
	sigemptyset(&p->signals_set); sigaddset(&p->signals_set,SIGINT); sigaddset(&p->signals_set,SIGTERM);
	sigaddset(&p->signals_set,SIGHUP); sigaddset(&p->signals_set,SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK,&p->signals_set,&p->old_mask)) goto settled_fail;
	if (pthread_attr_init(&attr)) goto mask_fail;
	/* Fixed 16 MiB total worker stack reservation; no per-message threads. */
	if (pthread_attr_setstacksize(&attr,1024u*1024u)) { pthread_attr_destroy(&attr); goto mask_fail; }
	for (i=0;i<JF_POOL_WORKERS;i++) {
		p->views[i].pool=p; p->views[i].index=i;
		if (pthread_create(&p->workers[i],&attr,worker,&p->views[i])) break;
		started++;
	}
	if (started==JF_POOL_WORKERS && !pthread_create(&p->signals,&attr,signals,p)) { pthread_attr_destroy(&attr); return p; }
	pthread_attr_destroy(&attr);
	pthread_mutex_lock(&p->mutex); p->quit=1; pthread_cond_broadcast(&p->ready); pthread_mutex_unlock(&p->mutex);
	for (i=0;i<started;i++) pthread_join(p->workers[i],NULL);
mask_fail:
	pthread_sigmask(SIG_SETMASK,&p->old_mask,NULL);
settled_fail:
	pthread_cond_destroy(&p->settled);
ready_fail:
	pthread_cond_destroy(&p->ready);
mutex_fail:
	pthread_mutex_destroy(&p->mutex); free(p); return NULL;
}

void jf_pool_bind(struct jf_pool *p,const struct jf_storage *storage)
{
	unsigned i;
	/* A managed signal can arrive during preflight, before it is blocked for
	 * the pool. The coordinator transfers that already-observed cancellation. */
	if (storage->interrupted && *storage->interrupted) jf_pool_stop(p);
	p->destination_allowed=storage->destination_allowed; p->destination_context=storage->context;
	for (i=0;i<JF_POOL_WORKERS;i++) {
		struct jf_storage *s=&p->views[i].storage;
		*s=*storage; s->borrowed=1; s->interrupted=NULL;
		/* Both private names (full nonce) and public alternatives (nonce prefix)
		 * are disjoint across workers; sequence is worker-owned and checked. */
		s->nonce[0]^=(unsigned char)i; s->nonce[15]^=(unsigned char)i;
		s->should_stop=jf_pool_stopped; s->context=p;
		s->destination_allowed=destination_allowed;
	}
}

void jf_pool_window(struct jf_pool *p,void *const *jobs,size_t count,jf_pool_run run)
{
	if (!count) return;
	pthread_mutex_lock(&p->mutex);
	p->jobs=jobs; p->next=0; p->count=p->remaining=count; p->run=run;
	pthread_cond_broadcast(&p->ready);
	while (p->remaining) pthread_cond_wait(&p->settled,&p->mutex);
	pthread_mutex_unlock(&p->mutex);
}

int jf_pool_finish(struct jf_pool *p)
{
	unsigned i; int signal;
	pthread_mutex_lock(&p->mutex); p->quit=1; pthread_cond_broadcast(&p->ready); pthread_mutex_unlock(&p->mutex);
	for (i=0;i<JF_POOL_WORKERS;i++) pthread_join(p->workers[i],NULL);
	pthread_join(p->signals,NULL);
	/* A synchronous write can leave SIGPIPE pending on the coordinator only. */
	pthread_mutex_lock(&p->mutex); observe_signals(p); signal=p->signal; pthread_mutex_unlock(&p->mutex);
	pthread_sigmask(SIG_SETMASK,&p->old_mask,NULL);
	pthread_cond_destroy(&p->settled); pthread_cond_destroy(&p->ready); pthread_mutex_destroy(&p->mutex);
	free(p); return signal;
}

#else
/* Unsupported runtimes retain the existing singleton path before effects. */
struct jf_pool *jf_pool_start(void) { return NULL; }
void jf_pool_bind(struct jf_pool *p,const struct jf_storage *s) { (void)p; (void)s; }
void jf_pool_window(struct jf_pool *p,void *const *jobs,size_t n,jf_pool_run run)
{ (void)p; (void)jobs; (void)n; (void)run; }
int jf_pool_stopped(void *p) { (void)p; return 1; }
void jf_pool_stop(struct jf_pool *p) { (void)p; }
int jf_pool_finish(struct jf_pool *p) { (void)p; return 0; }
#endif
