/*
 * jetsam_limit NAME|PID [MB] (2026-11-20): a process's jetsam memory limit shown, or set to MB (both the active and
 * inactive limits, fatal), with memorystatus_control as root. Why: this port's job builder leaves Apple's
 * com.apple.jetsamproperties.D22.plist out, so daemons run on launchd's default limit; mediaserverd's HEIC encoding
 * of a 12 MP photo (~150 MB) went over it and was killed (ReportMemoryException), while iOS gives it 1600 MB on an
 * iPhone X. A live test of a limit before it goes into a job's plist.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* (libsystem_kernel / libproc; the iOS SDK ships no libproc.h) */
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES 8
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES 7

typedef struct memorystatus_memlimit_properties {
	int32_t memlimit_active;
	uint32_t memlimit_active_attr;
	int32_t memlimit_inactive;
	uint32_t memlimit_inactive_attr;
} memorystatus_memlimit_properties_t;

#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL 0x1

static int
find(const char *name)
{
	int pids[4096];
	int n = proc_listallpids(pids, sizeof(pids)) ;
	for (int i = 0; i < n; i++) {
		char nm[256] = "";
		proc_name(pids[i], nm, sizeof(nm));
		if (strcmp(nm, name) == 0) {
			return pids[i];
		}
	}
	return -1;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: jetsam_limit NAME|PID [MB]\n");
		return 1;
	}
	int pid = atoi(argv[1]) > 0 ? atoi(argv[1]) : find(argv[1]);
	memorystatus_memlimit_properties_t p = { 0 };

	if (pid <= 0) {
		printf("jetsam_limit: no process %s\n", argv[1]);
		return 1;
	}
	if (memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, pid, 0, &p, sizeof(p)) != 0) {
		perror("jetsam_limit: get");
		return 1;
	}
	printf("%s[%d]: active %d MB (attr %u), inactive %d MB (attr %u)\n", argv[1], pid, p.memlimit_active,
	    p.memlimit_active_attr, p.memlimit_inactive, p.memlimit_inactive_attr);
	if (argc > 2) {
		p.memlimit_active = p.memlimit_inactive = atoi(argv[2]);
		p.memlimit_active_attr = p.memlimit_inactive_attr = MEMORYSTATUS_MEMLIMIT_ATTR_FATAL;
		if (memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, pid, 0, &p, sizeof(p)) != 0) {
			perror("jetsam_limit: set");
			return 1;
		}
		memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, pid, 0, &p, sizeof(p));
		printf("%s[%d]: now active %d MB, inactive %d MB\n", argv[1], pid, p.memlimit_active, p.memlimit_inactive);
	}
	return 0;
}
