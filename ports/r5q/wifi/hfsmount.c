/*
 * hfsmount DEVICE DIR [ro] | hfsmount -u DIR (2026-11-20): an HFS volume mounted at DIR with mount(2) (no mount_hfs on
 * this phone), read-write unless "ro"; -u unmounts. Then statfs of DIR: the filesystem type and its free space. Why:
 * the first mounts of the HFS volume for /private/var on the flash (kernel t406+, /dev/ufs_var; wifi/STORAGE-PLAN.md).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/* hfs/hfs_mount.h (hfs-627.100.6), as the kernel's HFS reads it */
struct hfs_mount_args {
	char *fspec;
	uid_t hfs_uid;
	gid_t hfs_gid;
	mode_t hfs_mask;
	uint32_t hfs_encoding;
	struct timezone hfs_timezone;
	int flags;
	int journal_tbuffer_size;
	int journal_flags;
	int journal_disable;
};
#define VNOVAL (-1)

int
main(int argc, char **argv)
{
	struct statfs s;

	if (argc == 3 && strcmp(argv[1], "-u") == 0) {
		if (unmount(argv[2], 0) != 0) {
			fprintf(stderr, "hfsmount: unmount %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
		printf("hfsmount: %s unmounted\n", argv[2]);
		return 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: hfsmount DEVICE DIR [ro] | hfsmount -u DIR\n");
		return 2;
	}
	struct hfs_mount_args a;
	memset(&a, 0, sizeof(a));
	a.fspec = argv[1];
	a.hfs_uid = (uid_t)VNOVAL;
	a.hfs_gid = (gid_t)VNOVAL;
	a.hfs_mask = (mode_t)VNOVAL;
	a.hfs_encoding = (uint32_t)VNOVAL;
	(void)mkdir(argv[2], 0755);       /* (no mkdir on this phone) */
	if (mount("hfs", argv[2], argc > 3 && strcmp(argv[3], "ro") == 0 ? MNT_RDONLY : 0, &a) != 0) {
		fprintf(stderr, "hfsmount: mount %s on %s: %s\n", argv[1], argv[2], strerror(errno));
		return 1;
	}
	if (statfs(argv[2], &s) == 0) {
		printf("hfsmount: %s on %s: %s, %.2f GB free of %.2f GB, %llu files free\n", s.f_mntfromname, s.f_mntonname,
		    s.f_fstypename, (double)s.f_bavail * s.f_bsize / 1e9, (double)s.f_blocks * s.f_bsize / 1e9,
		    (unsigned long long)s.f_ffree);
	}
	return 0;
}
