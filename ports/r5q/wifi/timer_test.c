/*
 * timer_test (Test 150): do long dispatch timers fire here? (backboardd's AttentionAwareness scheduler fires 0.1 s
 * timeouts but never its 40/60 s ones, so the phone never auto-locks.) Arms a 45 s timer on each clock dispatch has:
 * DISPATCH_TIME_NOW (mach_absolute_time), the continuous clock (DISPATCH_MONOTONICTIME_NOW -- mach_continuous_time) and
 * the wall clock (dispatch_walltime), plus a repeating 1 s timer, and says the clocks every 10 s.
 */
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t start_abs, start_cont;
static mach_timebase_info_data_t tb;

static double
secs(uint64_t ticks)
{
	return (double)ticks * tb.numer / tb.denom / 1e9;
}

static void
clocks(const char *what)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	printf("timer_test: %s: absolute +%.3f s, continuous +%.3f s, wall %ld.%03d, raw %llu ms\n", what,
	    secs(mach_absolute_time() - start_abs), secs(mach_continuous_time() - start_cont), (long)tv.tv_sec,
	    (int)(tv.tv_usec / 1000), clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000ull);
}

static void
fired(void *what)
{
	clocks((const char *)what);
}

int
main(void)
{
	dispatch_queue_t q = dispatch_queue_create("timer_test", NULL);
	static int ticks;

	setvbuf(stdout, NULL, _IOLBF, 0);
	mach_timebase_info(&tb);
	start_abs = mach_absolute_time();
	start_cont = mach_continuous_time();
	printf("timer_test: timebase %u/%u; continuous - absolute = %.3f s\n", tb.numer, tb.denom, secs(start_cont) - secs(start_abs));
	clocks("start");
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC), q, "45 s on DISPATCH_TIME_NOW FIRED", fired);
#ifdef DISPATCH_MONOTONICTIME_NOW
	dispatch_after_f(dispatch_time(DISPATCH_MONOTONICTIME_NOW, 45 * NSEC_PER_SEC), q, "45 s on MONOTONICTIME FIRED", fired);
#endif
	dispatch_after_f(dispatch_walltime(NULL, 45 * NSEC_PER_SEC), q, "45 s on walltime FIRED", fired);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC / 10), q, "0.2 s on DISPATCH_TIME_NOW FIRED", fired);
	{
		dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);

		dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
		dispatch_source_set_event_handler_f(t, (dispatch_function_t)(void *)0);
		dispatch_set_context(t, NULL);
		dispatch_source_set_event_handler(t, ^{
			if (++ticks % 10 == 0) {
				clocks("1 s repeating, 10 more ticks");
			}
		});
		dispatch_resume(t);
	}
	sleep(70);
	clocks("end (70 s of sleep())");
	return 0;
}
