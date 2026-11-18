/*
 * orient_test [SECONDS]: what backboardd says the device orientation is (BKSHIDGetCurrentDeviceOrientation: 0 unknown,
 * 1 portrait, 2 upside down, 3 landscape left, 4 landscape right, 5 face up, 6 face down), once a second, with the kernel's
 * last accelerometer sample (kern.redwood.motion). Over r5qd: does the accelerometer reach iOS's orientation?
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <dlfcn.h>

struct redwood_motion {
	uint32_t version, seq;
	uint32_t count[5];
	int32_t accel[3], gyro[3], mag[3];
	int32_t lux;
	int32_t prox[2];
	uint64_t time[5];
};

int
main(int argc, char **argv)
{
	static const char *const names[] = { "unknown", "portrait", "upside down", "landscape left", "landscape right", "face up",
		                             "face down" };
	int seconds = argc > 1 ? atoi(argv[1]) : 5;
	void *bbs = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
	long (*BKSHIDGetCurrentDeviceOrientation)(void) = bbs ? (long (*)(void))dlsym(bbs, "BKHIDServicesGetCurrentDeviceOrientation") : NULL;
	long (*BKSHIDGetNonFlatDeviceOrientation)(void) = bbs ? (long (*)(void))dlsym(bbs, "BKHIDServicesGetNonFlatDeviceOrientation") : NULL;

	if (BKSHIDGetCurrentDeviceOrientation == NULL || BKSHIDGetNonFlatDeviceOrientation == NULL) {
		printf("orient_test: BackBoardServices' orientation calls not found\n");
		return 1;
	}

	for (int i = 0; i < seconds; i++) {
		struct redwood_motion m = { 0 };
		size_t size = sizeof(m);
		long o = BKSHIDGetCurrentDeviceOrientation(), nf = BKSHIDGetNonFlatDeviceOrientation();

		sysctlbyname("kern.redwood.motion", &m, &size, NULL, 0);
		printf("orient_test: device %ld (%s), non-flat %ld (%s); accel %u samples, last %d %d %d (m/s^2 x1000); mag %u "
		    "(%d %d %d uT x1000); light %u (%d lux x1000); proximity %u (%d)\n", o, o >= 0 && o <= 6 ? names[o] : "?", nf,
		    nf >= 0 && nf <= 6 ? names[nf] : "?", m.count[0], m.accel[0], m.accel[1], m.accel[2], m.count[2], m.mag[0], m.mag[1],
		    m.mag[2], m.count[3], m.lux, m.count[4], m.prox[0]);
		fflush(stdout);
		sleep(1);
	}
	return 0;
}
