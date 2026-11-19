/*
 * klog: the kernel's message buffer (what printf in the kernel wrote, as `dmesg` shows it), its last N bytes
 * (default 32768), to stdout. Run as root through r5qd (`phone.py wlog` polls it over Wi-Fi every second).
 *
 * (2026-11-19, camera Tests 306-310) A second channel for the kernel log: the USB follower (`oem log`, a 2 MB ring
 * every ~2 s) went silent while iOS ran on for minutes, and the last seconds before a hang were lost. This one goes
 * over Wi-Fi, independent of the loader's USB.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int proc_kmsgbuf(void *buffer, uint32_t buffersize);     /* libSystem (libproc), as dmesg uses it */

int
main(int argc, char **argv)
{
	uint32_t size = 1024 * 1024, want = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 32768;
	char *buf = malloc(size + 1);
	int n;

	if (buf == NULL) {
		return 1;
	}
	n = proc_kmsgbuf(buf, size);
	if (n <= 0) {
		fprintf(stderr, "klog: proc_kmsgbuf: %d\n", n);
		return 1;
	}
	/* (the buffer comes back in order, with NULs where it was never written) */
	int start = n > (int)want ? n - (int)want : 0;
	for (int i = start; i < n; i++) {
		if (buf[i] != 0) {
			putchar(buf[i]);
		}
	}
	return 0;
}
