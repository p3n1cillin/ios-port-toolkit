/*
 * r5q_sysctl NAME: a sysctl's value as bytes on standard output (kern.redwood.slpi_log, the sensors' DSP's log), over r5qd.
 * r5q_sysctl NAME VALUE (2026-11-20): an integer sysctl set (kern.redwood.bb_answer: the baseband stubs' answers), then
 * its value printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	size_t size = 0;
	char *buf;

	if (argc < 2) {
		printf("usage: r5q_sysctl NAME [INTEGER]\n");
		return 1;
	}
	if (argc > 2) {
		int value = (int)strtol(argv[2], NULL, 0), now = 0;
		size_t now_size = sizeof(now);

		if (sysctlbyname(argv[1], &now, &now_size, &value, sizeof(value)) != 0) {
			perror(argv[1]);
			return 1;
		}
		printf("%s: was %d, now %d\n", argv[1], now, value);
		return 0;
	}
	if (sysctlbyname(argv[1], NULL, &size, NULL, 0) != 0) {
		perror(argv[1]);
		return 1;
	}
	buf = malloc(size + 1);
	if (buf == NULL || sysctlbyname(argv[1], buf, &size, NULL, 0) != 0) {
		perror(argv[1]);
		return 1;
	}
	fwrite(buf, 1, size, stdout);
	return 0;
}
