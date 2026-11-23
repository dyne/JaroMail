#ifndef JARO_DEDUP_PROFILE_H
#define JARO_DEDUP_PROFILE_H

/* Optional measurements on an optimized regression driver. Production builds
 * compile the hooks away, including the per-message monotonic clock reads. */
#if defined(JARO_DEDUP_PROFILE) && defined(JARO_DEDUP_TESTING)
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

enum jd_timer { JD_INVENTORY, JD_SCAN, JD_INSERT, JD_SAMPLE, JD_HASH, JD_COMPARE, JD_UNLINK, JD_TIMERS };
extern uint64_t jd_elapsed[JD_TIMERS], jd_message_opens;
extern uint64_t jd_scratch_peak;
void jd_watch_scratch(const char *directory);

static inline uint64_t
jd_clock_ns(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		abort();
	return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

#define JD_START(name) uint64_t name = jd_clock_ns()
#define JD_END(name, timer) (jd_elapsed[(timer)] += jd_clock_ns() - (name))
#define JD_OPENED() (++jd_message_opens)
#else
#define JD_START(name) ((void)0)
#define JD_END(name, timer) ((void)0)
#define JD_OPENED() ((void)0)
#endif
#endif
