#define _POSIX_C_SOURCE 200809L
#include "filter/generation.h"
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

/* Phase-gated fault controls preserve input-job order, while unconfigured
 * phases and the production algorithm remain concurrent. Never hold this
 * mutex during external pipe I/O or storage work. */
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t advanced=PTHREAD_COND_INITIALIZER;
static pthread_key_t job_key;
static pthread_once_t job_once=PTHREAD_ONCE_INIT;
static size_t completed;
static unsigned char settled[32];
static int parallel_controls,failed;
static unsigned matches;
static void make_key(void) { if (pthread_key_create(&job_key,NULL)) abort(); }
void jf_generation_test_parallel(int enabled)
{
	pthread_mutex_lock(&control); parallel_controls=enabled; pthread_mutex_unlock(&control);
}
void jf_generation_test_job(size_t index)
{
	pthread_once(&job_once,make_key);
	if (pthread_setspecific(job_key,(void *)(uintptr_t)index)) abort();
}
void jf_generation_test_done(size_t index)
{
	pthread_mutex_lock(&control);
	settled[index%32]=1;
	while (settled[(completed+1)%32]) { settled[(completed+1)%32]=0; completed++; }
	pthread_cond_broadcast(&advanced); pthread_mutex_unlock(&control);
	pthread_setspecific(job_key,NULL);
}
int jf_generation_test_hook(const char *phase)
{
	const char *hold=getenv("JF_TEST_HOLD"),*fail=getenv("JF_TEST_FAIL"),*crash=getenv("JF_TEST_CRASH");
	const char *at=getenv("JF_TEST_FAIL_AT"),*match=hold ? strstr(hold,phase) : NULL;
	const char *target=getenv("JF_TEST_JOB");
	int holding=match && (match==hold || match[-1]==',') && (match[strlen(phase)]==0 || match[strlen(phase)]==',');
	int failing=fail && !strcmp(fail,phase),crashing=crash && !strcmp(crash,phase),inject=0;
	size_t job;
	pthread_once(&job_once,make_key); job=(size_t)(uintptr_t)pthread_getspecific(job_key);
	pthread_mutex_lock(&control);
	if (target && job!=(size_t)strtoull(target,NULL,10)) holding=failing=crashing=0;
	while (job && parallel_controls && !target && (holding || failing || crashing) && completed+1<job)
		pthread_cond_wait(&advanced,&control);
	if (getenv("JF_TEST_TRACE")) {
		fprintf(stderr,"TRACE:%s\n",phase);
		if (job) fprintf(stderr,"TRACE_JOB:%zu:%s\n",job,phase);
	}
	if (!failed && failing && ++matches==(at ? strtoul(at,NULL,10) : 1)) { failed=1; inject=1; }
	pthread_mutex_unlock(&control);
	if (holding) {
		const char *a=getenv("JF_TEST_NOTIFY_FD"),*b=getenv("JF_TEST_RELEASE_FD"); char byte='1'; ssize_t n;
		if (!a || !b) return -1;
		do { n=write(atoi(a),&byte,1); } while (n<0 && errno==EINTR);
		if (n!=1) return -1;
		do { n=read(atoi(b),&byte,1); } while (n<0 && errno==EINTR);
		if (n!=1) return -1;
	}
	if (crashing) raise(SIGKILL);
	if (inject) { fprintf(stderr,"INJECTED:%s\n",phase); errno=EIO; return -1; }
	return 0;
}

#ifdef JF_PIN_DRIVER
int main(int argc,char **argv)
{
	struct jf_generation g={0}; struct jf_error error={0}; int rc,fd; char buffer[4096]; ssize_t n;
	g.directory=-1;
	if (argc<2 || argc>3) return 2;
	rc=jf_generation_pin(argv[1],argc==3 ? argv[2] : NULL,&g,&error); if (rc) return rc;
	if (jf_generation_test_hook("pinned")) { jf_generation_close(&g); return 3; }
	fwrite(g.id,1,65,stdout);
	fd=jf_generation_open(&g,"filters.zsh"); if (fd<0) { jf_generation_close(&g); return 3; }
	while ((n=read(fd,buffer,sizeof buffer))>0) if (fwrite(buffer,1,(size_t)n,stdout)!=(size_t)n) break;
	rc=n<0 || ferror(stdout) ? 3 : 0;
	close(fd); jf_generation_close(&g); return rc;
}
#endif
