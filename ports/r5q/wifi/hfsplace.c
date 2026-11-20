/*
 * hfsplace PLACEMENT [DEVICE] (2026-11-20): a volume formatted on the PC (storage/mkplace.py's placement) written
 * onto DEVICE (default /dev/rufs_var, the part of darwin past 16 GiB: kernel t406+, ufs_write=1 ufs_var=1): its zero
 * ranges as zeros, its content blocks as they are, then every content block read back and compared. The device must
 * be exactly the volume's size. Why: /private/var on an HFS volume on the flash (wifi/STORAGE-PLAN.md) -- newfs_hfs
 * needs macOS frameworks, and a 93 GB volume is ~200 MB of metadata, nearly all zeros.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* (the iOS SDK has no sys/disk.h) */
#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
#define ZBUF               (4u << 20)

struct rec {
	uint64_t offset, length;
	uint32_t kind;
} __attribute__((packed));

int
main(int argc, char **argv)
{
	const char *dev = argc > 2 ? argv[2] : "/dev/rufs_var";
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
	char magic[9];
	uint64_t volume, count = 0, wrote = 0;
	uint32_t bs = 0;
	struct rec r;
	int fd, records = 0, content = 0, bad = 0;
	uint8_t *zeros = calloc(1, ZBUF), block[4096], back[4096];

	if (f == NULL || fread(magic, 1, 9, f) != 9 || memcmp(magic, "HFSPLACE1", 9) != 0 || fread(&volume, 8, 1, f) != 1) {
		fprintf(stderr, "hfsplace: not a placement: %s\n", argc > 1 ? argv[1] : "(none)");
		return 1;
	}
	fd = open(dev, O_RDWR);
	if (fd < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0) {
		fprintf(stderr, "hfsplace: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if ((uint64_t)bs * count != volume) {
		fprintf(stderr, "hfsplace: %s is %llu bytes, the volume %llu: refused\n", dev,
		    (unsigned long long)bs * count, (unsigned long long)volume);
		return 1;
	}
	while (fread(&r, sizeof(r), 1, f) == 1) {
		records++;
		if (r.offset % 4096 || r.length % 4096 || r.offset + r.length > volume) {
			fprintf(stderr, "hfsplace: record %d out of bounds\n", records);
			return 1;
		}
		if (r.kind == 1) {
			if (r.length != 4096 || fread(block, 1, 4096, f) != 4096) {
				fprintf(stderr, "hfsplace: record %d: short content\n", records);
				return 1;
			}
			if (pwrite(fd, block, 4096, (off_t)r.offset) != 4096) {
				fprintf(stderr, "hfsplace: write at %llu: %s\n", (unsigned long long)r.offset, strerror(errno));
				return 1;
			}
			content++;
			wrote += 4096;
			continue;
		}
		for (uint64_t at = 0; at < r.length;) {
			uint64_t n = r.length - at > ZBUF ? ZBUF : r.length - at;

			if (pwrite(fd, zeros, (size_t)n, (off_t)(r.offset + at)) != (ssize_t)n) {
				fprintf(stderr, "hfsplace: zeros at %llu: %s\n", (unsigned long long)(r.offset + at), strerror(errno));
				return 1;
			}
			at += n;
			wrote += n;
		}
	}
	/* every content block read back */
	fseek(f, 17, SEEK_SET);
	while (fread(&r, sizeof(r), 1, f) == 1) {
		if (r.kind != 1) {
			continue;
		}
		if (fread(block, 1, 4096, f) != 4096 || pread(fd, back, 4096, (off_t)r.offset) != 4096 || memcmp(block, back, 4096) != 0) {
			bad++;
		}
	}
	printf("hfsplace: %d records, %d content blocks, %llu MB written to %s; read back: %s\n", records, content,
	    (unsigned long long)(wrote >> 20), dev, bad ? "MISMATCH" : "all match");
	close(fd);
	return bad ? 1 : 0;
}
