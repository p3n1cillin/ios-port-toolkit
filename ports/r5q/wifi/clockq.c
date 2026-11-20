/*
 * clockq (2026-11-20): the phone's clocks side by side -- mach_absolute_time, mach_continuous_time (which also counts
 * time asleep), their difference, the timebase, and the kernel's boot and wake times (kern.boottime, kern.waketime,
 * kern.sleeptime). Why: CMCapture's audio source node saw the host time elapsed run a constant 468.7 s ahead of the
 * audio's sample time on every callback (wifi/MEMORY-PLAN.md); a clock pair that differs by a constant is the first
 * thing to look at.
 */
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/time.h>

static void
tv(const char *name)
{
	struct timeval t = { 0 };
	size_t len = sizeof(t);
	if (sysctlbyname(name, &t, &len, NULL, 0) == 0) {
		printf("%-16s %ld.%06d\n", name, (long)t.tv_sec, (int)t.tv_usec);
	} else {
		printf("%-16s (none)\n", name);
	}
}

int
main(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t a = mach_absolute_time(), c = mach_continuous_time();
	double s = (double)tb.numer / tb.denom / 1e9;
	printf("timebase %u/%u (%.3f MHz)\n", tb.numer, tb.denom, 1e3 / ((double)tb.numer / tb.denom) );
	printf("mach_absolute_time   %llu = %.3f s\n", (unsigned long long)a, a * s);
	printf("mach_continuous_time %llu = %.3f s\n", (unsigned long long)c, c * s);
	printf("continuous - absolute = %.3f s\n", (double)(int64_t)(c - a) * s);
	tv("kern.boottime");
	tv("kern.waketime");
	tv("kern.sleeptime");
	struct timeval now;
	gettimeofday(&now, NULL);
	printf("now              %ld.%06d\n", (long)now.tv_sec, (int)now.tv_usec);
	return 0;
}
