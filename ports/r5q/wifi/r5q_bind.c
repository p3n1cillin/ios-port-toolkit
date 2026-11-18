/*
 * r5q_bind FROM TO [PROGRAM ARGS...]: show directory FROM at TO, read only (bindfs), making TO first as a synthetic
 * directory when the volume has none (kern.apfs_synthetic, this port's kernel). What init does for its own mounts,
 * from a program run later (init itself is a static binary this machine's linker cannot make).
 * (r5q, Test 169: the ringtones, left out of the system image, shown on /Library/Ringtones.)
 * The port's bindfs takes a mount only from process 1 (or Apple's entitlement): so init's rc runs this with "exec",
 * keeping process number 1, and this then becomes PROGRAM (launchd) -- whether or not the mount was made (Test 170).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int
bind_dir(const char *from, const char *to)
{
	struct stat st;

	if (stat(from, &st) != 0) {
		printf("r5q_bind: %s: %s\n", from, strerror(errno));
		return 1;
	}
	if (stat(to, &st) != 0 && sysctlbyname("kern.apfs_synthetic", NULL, NULL, (void *)to, strlen(to)) != 0) {
		printf("r5q_bind: synthetic directory %s: %s\n", to, strerror(errno));
		return 1;
	}
	if (mount("bindfs", to, MNT_RDONLY, (void *)from) != 0) {
		printf("r5q_bind: bindfs %s on %s: %s\n", from, to, strerror(errno));
		return 1;
	}
	printf("r5q_bind: %s shown on %s\n", from, to);
	return 0;
}

int
main(int argc, char **argv)
{
	int status;

	if (argc < 3) {
		fprintf(stderr, "usage: r5q_bind FROM TO [PROGRAM ARGS...]\n");
		return 2;
	}
	status = bind_dir(argv[1], argv[2]);
	if (argc > 3) {
		fflush(stdout);
		execv(argv[3], argv + 3);
		printf("r5q_bind: cannot start %s: %s\n", argv[3], strerror(errno));
		return 1;
	}
	return status;
}
