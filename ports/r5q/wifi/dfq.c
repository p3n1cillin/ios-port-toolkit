/*
 * dfq [clean] (2026-11-20): free space of / and /private/var (statfs), and the size of camgrab's raw frames in
 * /private/var/tmp. With "clean": those *.raw frames (camgrab's, and only those) are deleted. Why: iOS showed
 * "iPhone Storage Full" while the back camera's stripes were being measured with dozens of 3.7 MB raw frames.
 * dfq rm FILE...: those regular files deleted (r5qd has no rm, and the phone no shell; Test 412 left a 50 MB test
 * file on the flash volume). Only regular files, never a directory.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *mounts[] = { "/", "/private/var" };
	int clean = argc > 1 && strcmp(argv[1], "clean") == 0;
	long long total = 0;
	int count = 0;

	if (argc > 2 && strcmp(argv[1], "rm") == 0) {
		int bad = 0;

		for (int i = 2; i < argc; i++) {
			struct stat st;

			if (lstat(argv[i], &st) != 0 || !S_ISREG(st.st_mode) || unlink(argv[i]) != 0) {
				printf("dfq: %s: not removed (not a regular file, or unlink failed)\n", argv[i]);
				bad = 1;
			} else {
				printf("dfq: %s removed (%.1f MB)\n", argv[i], st.st_size / 1e6);
			}
		}
		return bad;
	}

	for (int i = 0; i < 2; i++) {
		struct statfs s;
		if (statfs(mounts[i], &s) == 0) {
			printf("%-14s %s: %.2f GB free of %.2f GB\n", mounts[i], s.f_mntfromname,
			    (double)s.f_bavail * s.f_bsize / 1e9, (double)s.f_blocks * s.f_bsize / 1e9);
		}
	}
	DIR *d = opendir("/private/var/tmp");
	struct dirent *e;
	while (d != NULL && (e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		char path[1100];
		struct stat st;

		if (n < 5 || strcmp(e->d_name + n - 4, ".raw") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/private/var/tmp/%s", e->d_name);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			total += st.st_size;
			count++;
			if (clean) {
				unlink(path);
			}
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	printf("%d raw frames in /private/var/tmp, %.1f MB%s\n", count, total / 1e6, clean ? ": deleted" : "");
	return 0;
}
