/*
 * fps_test [SECONDS] [INTERVAL_MS]: the window server's frames per interval (CoreAnimation's render-server frame
 * counter and its "dirty" frame count -- frames that actually changed something), sampled for SECONDS (default 10)
 * every INTERVAL_MS (default 500). Run it over r5qd while the screen is being used: how smooth is the UI?
 */
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern mach_port_t CARenderServerGetServerPort(const char *name);
extern uint32_t CARenderServerGetFrameCounter(mach_port_t port);
extern uint32_t CARenderServerGetDirtyFrameCount(mach_port_t port);

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 10;
	int interval = argc > 2 ? atoi(argv[2]) : 500;
	mach_port_t port = CARenderServerGetServerPort(NULL);
	uint32_t f0 = CARenderServerGetFrameCounter(port), d0 = CARenderServerGetDirtyFrameCount(port);
	uint32_t first_f = f0, first_d = d0;
	double t0 = now(), start = t0;

	printf("fps_test: server port %#x, frame counter %u, dirty %u\n", port, f0, d0);
	while (now() - start < seconds) {
		usleep(interval * 1000);
		uint32_t f = CARenderServerGetFrameCounter(port), d = CARenderServerGetDirtyFrameCount(port);
		double t = now();
		printf("fps_test: +%5.1f s: %5.1f frames/s, %5.1f changed/s\n", t - start, (f - f0) / (t - t0), (d - d0) / (t - t0));
		fflush(stdout);
		f0 = f; d0 = d; t0 = t;
	}
	printf("fps_test: average %.1f frames/s, %.1f changed/s over %.1f s\n", (f0 - first_f) / (now() - start),
	    (d0 - first_d) / (now() - start), now() - start);
	return 0;
}
