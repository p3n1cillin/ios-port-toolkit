/*
 * blkbench A B [N] (2026-11-20): two block devices read at the same places and compared, and timed. A and B are raw
 * devices (e.g. /dev/rmd0 and /dev/rufs_darwin); the places are N (default 200) random 16 KB pieces within A's size,
 * then 64 MB from the start in 1 MB reads. Prints, for each device, MB/s and microseconds a read, and how many pieces
 * differ. Why: the iOS system image the firmware copies into RAM (md0, 2.3 GB of the phone's 6) comes from USERDATA
 * block 0; if the flash holds the same bytes and reads are fast enough, md0 can be read from the flash on demand.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* (sys/disk.h is not in the iOS SDK) */
#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
#include <sys/ioctl.h>
#include <sys/ioccom.h>
#include <time.h>
#include <unistd.h>

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t
devsize(int fd)
{
	uint32_t bs = 0;
	uint64_t n = 0;
	if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &n) != 0) {
		return 0;
	}
	return (uint64_t)bs * n;
}

int
main(int argc, char **argv)
{
	int n = argc > 3 ? atoi(argv[3]) : 200;
	if (argc < 3) {
		printf("usage: blkbench A B [N]\n");
		return 1;
	}
	int a = open(argv[1], O_RDONLY), b = open(argv[2], O_RDONLY);
	if (a < 0 || b < 0) {
		printf("open: %s\n", strerror(errno));
		return 1;
	}
	uint64_t size = devsize(a);
	printf("%s: %llu MB; %s: %llu MB\n", argv[1], size >> 20, argv[2], devsize(b) >> 20);
	if (size == 0) {
		return 1;
	}
	static uint8_t x[1 << 20], y[1 << 20];
	double ta = 0, tb = 0, t;
	int differ = 0, bad = 0;
	uint64_t seed = (uint64_t)time(NULL) * 2654435761u;
	for (int i = 0; i < n; i++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		off_t off = (off_t)((seed >> 20) % (size / 16384)) * 16384;
		t = now();
		ssize_t ga = pread(a, x, 16384, off);
		ta += now() - t;
		t = now();
		ssize_t gb = pread(b, y, 16384, off);
		tb += now() - t;
		if (ga != 16384 || gb != 16384) {
			bad++;
		} else if (memcmp(x, y, 16384) != 0) {
			if (differ++ < 5) {
				printf("  differ at %lld\n", (long long)off);
			}
		}
	}
	printf("random 16 KB x %d: %s %.0f us a read, %s %.0f us a read; %d differ, %d short\n", n, argv[1], ta * 1e6 / n,
	    argv[2], tb * 1e6 / n, differ, bad);
	ta = tb = 0;
	differ = 0;
	for (int i = 0; i < 64; i++) {
		off_t off = (off_t)i << 20;
		t = now();
		ssize_t ga = pread(a, x, 1 << 20, off);
		ta += now() - t;
		t = now();
		ssize_t gb = pread(b, y, 1 << 20, off);
		tb += now() - t;
		differ += ga != gb || memcmp(x, y, 1 << 20) != 0;
	}
	printf("sequential 1 MB x 64: %s %.0f MB/s, %s %.0f MB/s; %d differ\n", argv[1], 64 / ta, argv[2], 64 / tb, differ);
	return 0;
}
