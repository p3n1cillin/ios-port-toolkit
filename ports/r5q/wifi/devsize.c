/*
 * devsize DEVICE... (2026-11-20): each disk device's block size and block count (DKIOCGETBLOCKSIZE,
 * DKIOCGETBLOCKCOUNT), and its bytes. Why: the HFS volume for /private/var (wifi/STORAGE-PLAN.md) is formatted on the
 * PC to the exact size of darwin past 16 GiB, and that size comes from here.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioccom.h>
/* (the iOS SDK has no sys/disk.h: its two ioctls, as bsd/sys/disk.h has them) */
#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		uint32_t bs = 0;
		uint64_t count = 0;
		int fd = open(argv[i], O_RDONLY);

		if (fd < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0) {
			perror(argv[i]);
		} else {
			printf("%s: %u-byte blocks, %llu of them, %llu bytes\n", argv[i], bs, (unsigned long long)count,
			    (unsigned long long)count * bs);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	return 0;
}
