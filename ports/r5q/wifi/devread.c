/*
 * devread PATH [BYTES] [FLAGS]: open a device (or any file) and read from it, saying exactly what failed (2026-11-19:
 * Camera.app's frames, "no /dev/redwood-camera (Input/output error)" in mediaserverd -- the kernel's side or the
 * caller's?). FLAGS: r (O_RDONLY, default) or rw (O_RDWR). Prints the node's mode and device number, the open's
 * result, then up to BYTES bytes (default 64) as hex. Built by pipeline/build_phone_tools.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct stat st;
	unsigned char buf[4096];
	size_t want = argc > 2 ? strtoul(argv[2], NULL, 0) : 64;
	int flags = argc > 3 && strcmp(argv[3], "rw") == 0 ? O_RDWR : O_RDONLY, fd;
	ssize_t n;

	if (argc < 2) {
		printf("usage: devread PATH [BYTES] [r|rw]\n");
		return 1;
	}
	if (stat(argv[1], &st) != 0) {
		printf("stat %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	printf("%s: mode %o, device %d,%d, size %lld\n", argv[1], st.st_mode, (int)((st.st_rdev >> 24) & 0xff),
	    (int)(st.st_rdev & 0xffffff), (long long)st.st_size);
	fd = open(argv[1], flags);
	if (fd < 0) {
		printf("open (%s): %s (errno %d)\n", flags == O_RDWR ? "rw" : "r", strerror(errno), errno);
		return 1;
	}
	want = want > sizeof(buf) ? sizeof(buf) : want;
	n = read(fd, buf, want);
	if (n < 0) {
		printf("read: %s (errno %d)\n", strerror(errno), errno);
	} else {
		printf("read %zd bytes:", n);
		for (ssize_t i = 0; i < n && i < 64; i++) {
			printf(" %02x", buf[i]);
		}
		printf("\n");
	}
	close(fd);
	return 0;
}
