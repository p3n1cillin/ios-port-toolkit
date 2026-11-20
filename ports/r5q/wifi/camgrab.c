/*
 * camgrab [SENSOR] [SKIP] [OUT] [LINES GAIN] (2026-11-20): one raw frame from /dev/redwood-camera into OUT (default
 * /private/var/tmp/camgrab.raw; r5qd relays only 64 KB of output, pull it with wifi/r5q_pull.py), as mediaserverd reads
 * it -- the struct redwood_camera_frame header, then the frame's bytes (RAW10, 4 pixels in 5 bytes). SENSOR (0 macro,
 * 1 wide, 2 front, 3 ultra-wide; default: whatever the kernel has) is asked for first; SKIP frames (default 30, a
 * second for the exposure to settle) are read and thrown away. The header's numbers go to standard error. Run with
 * the Camera app closed (the device is mediaserverd's while it is open). Why: Camera.app's preview and photos showed
 * flat purple lines with noise at one edge, and the question is whether the sensor's frame or the rendering is wrong.
 * The PC side: wifi/camraw.py makes it a picture.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "redwood_camera.h"

int
main(int argc, char **argv)
{
	int sensor = argc > 1 ? atoi(argv[1]) : -1, skip = argc > 2 ? atoi(argv[2]) : 30;
	int fd = open(REDWOOD_CAMERA_DEVICE, O_RDWR);
	struct redwood_camera_frame h;

	if (fd < 0) {
		fprintf(stderr, "camgrab: %s: %s\n", REDWOOD_CAMERA_DEVICE, strerror(errno));
		return 1;
	}
	if (read(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) {
		fprintf(stderr, "camgrab: no header: %s\n", strerror(errno));
		return 1;
	}
	if (sensor >= 0 && (int)h.sensor != sensor) {
		struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = (uint32_t)sensor, .b = ~0u };
		(void)write(fd, &c, sizeof(c));
		for (int i = 0; i < 50 && (read(fd, &h, sizeof(h)) != (ssize_t)sizeof(h) || (int)h.sensor != sensor); i++) {
		}
	}
	if (argc > 5) {
		/* an exposure of our own (LINES, GAIN in the sensor's units; unity is in the header): e.g. 1 line at 1x, where what is
		 * left of the picture is the sensor's black level */
		struct redwood_camera_command c = { .command = REDWOOD_CAMERA_EXPOSURE, .a = (uint32_t)atoi(argv[4]),
		    .b = (uint32_t)atoi(argv[5]) };
		(void)write(fd, &c, sizeof(c));
	}
	size_t bytes = h.header_bytes + (size_t)h.stride * h.height;
	unsigned char *buf = malloc(bytes);
	ssize_t n = 0;
	for (int i = 0; i <= skip; i++) {
		n = read(fd, buf, bytes);
	}
	if (n != (ssize_t)bytes) {
		fprintf(stderr, "camgrab: frame read %zd of %zu: %s\n", n, bytes, strerror(errno));
		return 1;
	}
	struct redwood_camera_frame *f = (void *)buf;
	fprintf(stderr, "camgrab: sensor %u, %ux%u, stride %u, format %u, bayer %u, black %u, frame %llu, exposure %u lines "
	    "x %u ns, gain %u/%u, header %u bytes\n", f->sensor, f->width, f->height, f->stride, f->format, f->bayer, f->black,
	    (unsigned long long)f->sequence, f->exposure_lines, f->line_ns, f->gain, f->gain_unity, f->header_bytes);
	FILE *out = fopen(argc > 3 ? argv[3] : "/private/var/tmp/camgrab.raw", "wb");
	if (out == NULL || fwrite(buf, 1, bytes, out) != bytes) {
		fprintf(stderr, "camgrab: cannot write the frame: %s\n", strerror(errno));
		return 1;
	}
	fclose(out);
	close(fd);
	return 0;
}
