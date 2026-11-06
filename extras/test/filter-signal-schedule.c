#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Link-only schedule fixture; production helpers never link these wrappers. */
extern int __real_sigtimedwait(const sigset_t *,siginfo_t *,const struct timespec *);
extern int __real_nanosleep(const struct timespec *,struct timespec *);
extern int __real_pthread_mutex_lock(pthread_mutex_t *);
static _Thread_local int manager,initial_gate;
static atomic_int consumed,attempted;

static void notify(char value)
{
	const char *fd=getenv("JF_SIGNAL_NOTIFY_FD"); ssize_t n;
	if (!fd) _exit(125);
	do { n=write(atoi(fd),&value,1); } while (n<0 && errno==EINTR);
	if (n!=1) _exit(125);
}

static void gate(char value)
{
	const char *fd=getenv("JF_SIGNAL_RELEASE_FD"); char byte; ssize_t n;
	notify(value);
	if (!fd) _exit(125);
	do { n=read(atoi(fd),&byte,1); } while (n<0 && errno==EINTR);
	if (n!=1) _exit(125);
}

static void manager_entry(void)
{
	manager=1;
	if (!initial_gate) { initial_gate=1; gate('M'); }
}

int __wrap_sigtimedwait(const sigset_t *set,siginfo_t *info,const struct timespec *timeout)
{
	const char *mode=getenv("JF_SIGNAL_SCHEDULE"); int result;
	/* Old manager has a10ms wait; corrected manager has a zero-time poll and
	 * reaches its outside-lock sleep before this fixture's initial gate. */
	if (mode && timeout && (timeout->tv_sec || timeout->tv_nsec)) manager_entry();
	result=__real_sigtimedwait(set,info,timeout);
	if (mode && manager && result>0 && !strcmp(mode,"consumed")) {
		atomic_store(&consumed,1);
		gate('C'); /* Real signal consumed; caller has not published stop yet. */
	}
	return result;
}

int __wrap_nanosleep(const struct timespec *request,struct timespec *remaining)
{
	if (getenv("JF_SIGNAL_SCHEDULE")) manager_entry();
	return __real_nanosleep(request,remaining);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (!manager && atomic_load(&consumed) && !atomic_exchange(&attempted,1)) {
		/* Preserve ownership: a successful probe is this lock acquisition.
		 * It makes the old unlocked-consumer gap observable without a sleep. */
		if (!pthread_mutex_trylock(mutex)) { notify('L'); return 0; }
		notify('W'); /* Consumer owns this mutex until stop is published. */
	}
	return __real_pthread_mutex_lock(mutex);
}
