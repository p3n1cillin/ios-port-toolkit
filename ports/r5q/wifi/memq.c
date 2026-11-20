/*
 * memq [N] (2026-11-20): where the phone's memory is. hw.memsize, the memorystatus level (percent available), the
 * VM page counts (host_statistics64), and the N (default 25) processes with the biggest physical footprint
 * (proc_pid_rusage). Why: a 12 MP HEIC photo got mediaserverd killed by jetsam with no per-process limit set --
 * memorystatus_level read 13: the whole system was short of memory. Each process's resident size beside its footprint
 * (mediaserverd's footprint read 56 GB, which cannot be).
 */
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/mount.h>

/* (libproc; the iOS SDK ships no libproc.h) */
extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
struct rusage_info_v2_lite {
	uint8_t ri_uuid[16];
	uint64_t ri_user_time, ri_system_time, ri_pkg_idle_wkups, ri_interrupt_wkups, ri_pageins, ri_wired_size,
	    ri_resident_size, ri_phys_footprint;
	uint64_t rest[32];
};
extern int proc_pid_rusage(int pid, int flavor, void *buffer);

struct row { int pid; uint64_t fp, rss; char name[64]; };

static int
cmp(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	return x->fp < y->fp ? 1 : x->fp > y->fp ? -1 : 0;
}

int
main(int argc, char **argv)
{
	int top = argc > 1 ? atoi(argv[1]) : 25;
	uint64_t memsize = 0;
	int level = 0;
	size_t len = sizeof(memsize);
	sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
	len = sizeof(level);
	sysctlbyname("kern.memorystatus_level", &level, &len, NULL, 0);
	vm_statistics64_data_t vs;
	mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
	vm_size_t page = 0;
	host_page_size(mach_host_self(), &page);
	host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt);
	printf("memsize %llu MB, page %lu, memorystatus_level %d%%\n", memsize >> 20, (unsigned long)page, level);
	printf("pages (MB): free %llu, active %llu, inactive %llu, wired %llu, speculative %llu, compressed %llu, "
	    "purgeable %llu, file-backed %llu, anonymous %llu\n",
	    (uint64_t)vs.free_count * page >> 20, (uint64_t)vs.active_count * page >> 20,
	    (uint64_t)vs.inactive_count * page >> 20, (uint64_t)vs.wire_count * page >> 20,
	    (uint64_t)vs.speculative_count * page >> 20, (uint64_t)vs.compressor_page_count * page >> 20,
	    (uint64_t)vs.purgeable_count * page >> 20, (uint64_t)vs.external_page_count * page >> 20,
	    (uint64_t)vs.internal_page_count * page >> 20);
	/* the file systems: a RAM disk's size is memory the kernel holds (wired) */
	static const char *const mounts[] = { "/", "/private/var", "/private/preboot", "/System/Library/Caches", "/dev" };
	for (unsigned i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
		struct statfs sf;
		if (statfs(mounts[i], &sf) == 0) {
			printf("fs %-22s %-10s from %-24s size %llu MB, used %llu MB\n", mounts[i], sf.f_fstypename, sf.f_mntfromname,
			    (uint64_t)sf.f_blocks * sf.f_bsize >> 20, (uint64_t)(sf.f_blocks - sf.f_bfree) * sf.f_bsize >> 20);
		}
	}
	static int pids[4096];
	int n = proc_listallpids(pids, sizeof(pids));
	static struct row rows[4096];
	int m = 0;
	uint64_t total = 0;
	for (int i = 0; i < n; i++) {
		struct rusage_info_v2_lite ri;
		if (pids[i] <= 0 || proc_pid_rusage(pids[i], 2, &ri) != 0) {
			continue;
		}
		rows[m].pid = pids[i];
		rows[m].fp = ri.ri_phys_footprint;
		rows[m].rss = ri.ri_resident_size;
		proc_name(pids[i], rows[m].name, sizeof(rows[m].name));
		total += ri.ri_phys_footprint;
		m++;
	}
	qsort(rows, m, sizeof(rows[0]), cmp);
	printf("%d processes, footprints together %llu MB\n", m, total >> 20);
	for (int i = 0; i < m && i < top; i++) {
		printf("%6llu MB  (resident %5llu MB)  %5d %s\n", rows[i].fp >> 20, rows[i].rss >> 20, rows[i].pid,
		    rows[i].name);
	}
	return 0;
}
