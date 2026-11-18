/* r5q_sysctl NAME: a sysctl's value as bytes on standard output (kern.redwood.slpi_log, the sensors' DSP's log), over r5qd */
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
		printf("usage: r5q_sysctl NAME\n");
		return 1;
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
