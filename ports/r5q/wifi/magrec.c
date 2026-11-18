/*
 * magrec SECONDS: every new magnetometer sample the kernel has (kern.redwood.motion, as the SLPI gives it: uT x1000),
 * with the accelerometer beside it, one line each, for SECONDS -- to fit the hard-iron offset on the PC (Test 243).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SEE_KINDS 5

struct redwood_motion {                 /* as redwood_adsp.c */
	uint32_t version, seq;
	uint32_t count[SEE_KINDS];
	int32_t accel[3], gyro[3], mag[3];
	int32_t lux;
	int32_t prox[2];
	uint64_t time[SEE_KINDS];
};

int
main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 60;
	uint32_t last = 0, n = 0;

	for (int ticks = 0; ticks < seconds * 50; ticks++) {
		struct redwood_motion m;
		size_t size = sizeof(m);

		if (sysctlbyname("kern.redwood.motion", &m, &size, NULL, 0) == 0 && m.count[2] != last) {
			last = m.count[2];
			printf("%u %d %d %d %d %d %d\n", n++, m.mag[0], m.mag[1], m.mag[2], m.accel[0], m.accel[1], m.accel[2]);
			fflush(stdout);
		}
		usleep(20000);
	}
	return 0;
}
