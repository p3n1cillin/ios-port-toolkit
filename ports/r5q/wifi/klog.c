/*
 * klog: the kernel's message buffer (what printf in the kernel wrote, as `dmesg` shows it).
 *
 *   klog [N]        its last N bytes (default 32768), then exit
 *   klog -f SECS    follow: the last 4 KB, then every 50 ms only what is new, for SECS seconds or until ~60 KB have
 *                   gone out (r5qd relays a program's output as it comes, up to 64 KB a run); `phone.py wlog` starts
 *                   the next run at once, so a hang loses at most ~50 ms of the log
 *
 * Run as root through r5qd. (2026-11-19, camera Tests 306-325) A second channel for the kernel log, independent of the
 * loader's USB: the USB follower (a 2 MB ring every ~2 s) lost the last seconds before every hang.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int proc_kmsgbuf(void *buffer, uint32_t buffersize);     /* libSystem (libproc), as dmesg uses it */

#define SIZE    (1024 * 1024)
#define KEY     256             /* the last bytes sent, looked for in the next snapshot */

static char buf[SIZE + 1];

static int
snapshot(void)
{
	int n = proc_kmsgbuf(buf, SIZE), w = 0;

	for (int i = 0; i < n; i++) {           /* (NULs where it was never written) */
		if (buf[i] != 0) {
			buf[w++] = buf[i];
		}
	}
	return w;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && strcmp(argv[1], "-f") == 0) {
		int secs = atoi(argv[2]), n = snapshot(), sent = 0;
		char key[KEY];
		int keylen;

		setvbuf(stdout, NULL, _IONBF, 0);
		if (n <= 0) {
			fprintf(stderr, "klog: proc_kmsgbuf: %d\n", n);
			return 1;
		}
		int start = n > 4096 ? n - 4096 : 0;
		fwrite(buf + start, 1, (size_t)(n - start), stdout);
		sent += n - start;
		keylen = n < KEY ? n : KEY;
		memcpy(key, buf + n - keylen, (size_t)keylen);
		for (int t = 0; t < secs * 20 && sent < 60000; t++) {
			usleep(50000);
			n = snapshot();
			if (n < keylen) {
				continue;
			}
			/* the newest place of the key: what follows it is new */
			char *at = NULL;
			for (char *p = buf + n - keylen; p >= buf; p--) {
				if (memcmp(p, key, (size_t)keylen) == 0) {
					at = p + keylen;
					break;
				}
			}
			if (at == NULL) {               /* (wrapped past the key: everything is new, after a mark) */
				printf("\n== [klog: the buffer wrapped between two looks] ==\n");
				at = buf + (n > 8192 ? n - 8192 : 0);
			}
			if (at < buf + n) {
				fwrite(at, 1, (size_t)(buf + n - at), stdout);
				sent += (int)(buf + n - at);
				memcpy(key, buf + n - keylen, (size_t)keylen);
			}
		}
		return 0;
	}
	uint32_t want = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 32768;
	int n = snapshot();

	if (n <= 0) {
		fprintf(stderr, "klog: proc_kmsgbuf: %d\n", n);
		return 1;
	}
	fwrite(buf + (n > (int)want ? n - (int)want : 0), 1, (size_t)(n > (int)want ? (int)want : n), stdout);
	return 0;
}
