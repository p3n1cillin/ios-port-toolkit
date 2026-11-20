/*
 * torchset LEVEL (2026-11-20): the back flashlight at LEVEL (0-1000, thousandths of Android's torch current; 0 off)
 * through /dev/redwood-torch, the device iOS's flashlight uses (kernel redwood_torch.c, torch=3: its own current cap,
 * 30 s limit and temperature guards). Prints what is lit afterwards. Why: a light next to the back camera, to tell
 * whether the IMX586's frames (camgrab) respond to light through the lens at all.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	uint32_t level = argc > 1 ? (uint32_t)atoi(argv[1]) : 0, now = 0;
	int fd = open("/dev/redwood-torch", O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "torchset: /dev/redwood-torch: %s\n", strerror(errno));
		return 1;
	}
	if (write(fd, &level, sizeof(level)) != (ssize_t)sizeof(level)) {
		fprintf(stderr, "torchset: write: %s\n", strerror(errno));
	}
	if (read(fd, &now, sizeof(now)) == (ssize_t)sizeof(now)) {
		printf("torch: asked %u, lit %u\n", level, now);
	}
	close(fd);
	return 0;
}
