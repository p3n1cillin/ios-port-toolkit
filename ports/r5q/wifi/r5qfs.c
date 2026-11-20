/*
 * r5qfs: the few file operations the phone has no program for (iOS 16 has no shell, no ls), over r5qd:
 *   r5qfs ls PATH...            entries with mode, owner, size
 *   r5qfs stat PATH...          one line each (or "missing")
 *   r5qfs mkdir PATH UID GID MODE   every missing directory on the way made, the last owned UID:GID, MODE octal
 *   r5qfs chown PATH UID GID    one path
 *   r5qfs rm PATH               one file (not a directory)
 *   r5qfs cat PATH              a file's bytes to standard output (2026-11-20: CommCenter's logs and state files)
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
line(const char *path, const struct stat *st)
{
	char kind = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : '-';
	printf("%c%04o %5u:%-5u %10lld %s\n", kind, st->st_mode & 07777, st->st_uid, st->st_gid, (long long)st->st_size, path);
}

static int
do_stat(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		printf("missing %s (%s)\n", path, strerror(errno));
		return 1;
	}
	line(path, &st);
	return 0;
}

static int
do_ls(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	char full[1024];
	if (d == NULL) {
		return do_stat(path);
	}
	while ((e = readdir(d)) != NULL) {
		struct stat st;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (lstat(full, &st) == 0) {
			line(full, &st);
		}
	}
	closedir(d);
	return 0;
}

static int
do_mkdir(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) == 0) {
				chown(buf, uid, gid);
				printf("made %s\n", buf);
			}
			*p = '/';
		}
	}
	if (mkdir(buf, mode) == 0) {
		printf("made %s\n", buf);
	} else if (errno != EEXIST) {
		printf("mkdir %s: %s\n", buf, strerror(errno));
		return 1;
	}
	if (chown(buf, uid, gid) != 0 || chmod(buf, mode) != 0) {
		printf("chown/chmod %s: %s\n", buf, strerror(errno));
		return 1;
	}
	return do_stat(buf);
}

int
main(int argc, char **argv)
{
	int bad = 0;
	if (argc < 3) {
		printf("usage: r5qfs ls|stat PATH... | mkdir PATH UID GID MODE | chown PATH UID GID | rm PATH\n");
		return 1;
	}
	if (strcmp(argv[1], "ls") == 0 || strcmp(argv[1], "stat") == 0) {
		for (int i = 2; i < argc; i++) {
			bad |= argv[1][0] == 'l' ? do_ls(argv[i]) : do_stat(argv[i]);
		}
	} else if (strcmp(argv[1], "mkdir") == 0 && argc == 6) {
		bad = do_mkdir(argv[2], (uid_t)atoi(argv[3]), (gid_t)atoi(argv[4]), (mode_t)strtol(argv[5], NULL, 8));
	} else if (strcmp(argv[1], "chown") == 0 && argc == 5) {
		bad = chown(argv[2], (uid_t)atoi(argv[3]), (gid_t)atoi(argv[4])) != 0;
		bad = bad ? (printf("chown: %s\n", strerror(errno)), 1) : do_stat(argv[2]);
	} else if (strcmp(argv[1], "cat") == 0 && argc == 3) {
		FILE *f = fopen(argv[2], "rb");
		char buf[4096];
		size_t n;

		bad = f == NULL;
		if (bad) {
			printf("cat %s: %s\n", argv[2], strerror(errno));
		} else {
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
				fwrite(buf, 1, n, stdout);
			}
			fclose(f);
		}
	} else if (strcmp(argv[1], "rm") == 0) {
		bad = unlink(argv[2]) != 0;
		printf("rm %s: %s\n", argv[2], bad ? strerror(errno) : "done");
	} else {
		printf("r5qfs: what?\n");
		bad = 1;
	}
	return bad;
}
