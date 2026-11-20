/*
 * vmq NAME|PID [MIN_MB] (2026-11-20): a process's memory by kind, like vmmap's summary: every region of its address
 * space (mach_vm_region_recurse) with its resident and dirty pages, added up by the region's tag (VM_MEMORY_*:
 * MALLOC_LARGE, IOKit, CoreMedia ...) and protection; then the regions of MIN_MB (default 16) resident or more one by
 * one. Root, through proc_pidinfo. Why: mediaserverd was resident at 2.7 GB after a photo, its phys_footprint read
 * 56 GB, and memq could not say what that memory is.
 */
#include <mach/mach.h>
/* (the iOS SDK refuses mach/mach_vm.h; the call is in libsystem_kernel all the same) */
extern kern_return_t mach_vm_region_recurse(vm_map_read_t, mach_vm_address_t *, mach_vm_size_t *, natural_t *,
    vm_region_recurse_info_t, mach_msg_type_number_t *);
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

static const char *
tagname(unsigned t)
{
	switch (t) {
	case 0: return "untagged";
	case 1: return "MALLOC";
	case 2: return "MALLOC_SMALL";
	case 3: return "MALLOC_LARGE";
	case 4: return "MALLOC_HUGE";
	case 5: return "SBRK";
	case 6: return "REALLOC";
	case 7: return "MALLOC_TINY";
	case 8: return "MALLOC_LARGE_REUSABLE";
	case 9: return "MALLOC_LARGE_REUSED";
	case 10: return "ANALYSIS_TOOL";
	case 11: return "MALLOC_NANO";
	case 12: return "MALLOC_MEDIUM";
	case 13: return "MALLOC_PROB_GUARD";
	case 20: return "MACH_MSG";
	case 21: return "IOKIT";
	case 30: return "STACK";
	case 31: return "GUARD";
	case 32: return "SHARED_PMAP";
	case 33: return "DYLIB";
	case 34: return "OBJC_DISPATCHERS";
	case 35: return "UNSHARED_PMAP";
	case 40: return "APPKIT";
	case 41: return "FOUNDATION";
	case 42: return "COREGRAPHICS";
	case 43: return "CORESERVICES";
	case 44: return "JAVA";
	case 45: return "COREDATA";
	case 46: return "COREDATA_OBJECTIDS";
	case 50: return "ATS";
	case 51: return "LAYERKIT";
	case 52: return "CGIMAGE";
	case 53: return "TCMALLOC";
	case 54: return "COREGRAPHICS_DATA";
	case 55: return "COREGRAPHICS_SHARED";
	case 56: return "COREGRAPHICS_FRAMEBUFFERS";
	case 57: return "COREGRAPHICS_BACKINGSTORES";
	case 58: return "COREGRAPHICS_XALLOC";
	case 60: return "DYLD";
	case 61: return "DYLD_MALLOC";
	case 62: return "SQLITE";
	case 63: return "JAVASCRIPT_CORE";
	case 64: return "JAVASCRIPT_JIT_EXECUTABLE";
	case 65: return "JAVASCRIPT_JIT_REGISTER_FILE";
	case 66: return "GLSL";
	case 67: return "OPENCL";
	case 68: return "COREIMAGE";
	case 69: return "WEBCORE_PURGEABLE_BUFFERS";
	case 70: return "IMAGEIO";
	case 71: return "COREPROFILE";
	case 72: return "ASSETSD";
	case 73: return "OS_ALLOC_ONCE";
	case 74: return "LIBDISPATCH";
	case 75: return "ACCELERATE";
	case 76: return "COREUI";
	case 77: return "COREUIFILE";
	case 78: return "GENEALOGY";
	case 79: return "RAWCAMERA";
	case 80: return "CORPSEINFO";
	case 81: return "ASL";
	case 82: return "SWIFT_RUNTIME";
	case 83: return "SWIFT_METADATA";
	case 84: return "DHMM";
	case 86: return "SCENEKIT";
	case 87: return "SKYWALK";
	case 88: return "IOSURFACE";
	case 89: return "LIBNETWORK";
	case 90: return "AUDIO";
	case 91: return "VIDEOBITSTREAM";
	case 92: return "CM_XPC";
	case 93: return "CM_RPC";
	case 94: return "CM_MEMORYPOOL";
	case 95: return "CM_READCACHE";
	case 96: return "CM_CRABS";
	case 97: return "QUICKLOOK_THUMBNAILS";
	case 98: return "ACCOUNTS";
	case 99: return "SANITIZER";
	case 100: return "IOACCELERATOR";
	case 101: return "CM_REGWARP";
	case 102: return "EAR_DECODER";
	case 103: return "COREUI_CACHED_IMAGE_DATA";
	case 104: return "COLORSYNC";
	case 105: return "BTINFO";
	default: return t >= 240 ? "APPLICATION_SPECIFIC" : "?";
	}
}

struct sum { uint64_t resident, dirty, virt; uint32_t regions; };

/* struct proc_regioninfo (sys/proc_info.h) */
struct regioninfo {
	uint32_t pri_protection, pri_max_protection, pri_inheritance, pri_flags;
	uint64_t pri_offset;
	uint32_t pri_behavior, pri_user_wired_count, pri_user_tag, pri_pages_resident, pri_pages_shared_now_private,
	    pri_pages_swapped_out, pri_pages_dirtied, pri_ref_count, pri_shadow_depth, pri_share_mode,
	    pri_private_pages_resident, pri_shared_pages_resident, pri_obj_id, pri_depth;
	uint64_t pri_address, pri_size;
};
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: vmq NAME|PID [MIN_MB]\n");
		return 1;
	}
	int pid = atoi(argv[1]);
	uint64_t min = (uint64_t)(argc > 2 ? atoi(argv[2]) : 16) << 20;
	if (pid <= 0) {
		static int pids[4096];
		int n = proc_listallpids(pids, sizeof(pids));
		for (int i = 0; i < n; i++) {
			char nm[256] = "";
			proc_name(pids[i], nm, sizeof(nm));
			if (strcmp(nm, argv[1]) == 0) {
				pid = pids[i];
				break;
			}
		}
	}
	if (pid <= 0) {
		printf("vmq: no process %s\n", argv[1]);
		return 1;
	}
	/*
	 * proc_pidinfo(PROC_PIDREGIONINFO) as root, not task_for_pid: on this port task_for_pid answers
	 * KERN_SUCCESS with a port that is no use ("invalid destination port" at the first call).
	 */
	vm_size_t page = 0;
	host_page_size(mach_host_self(), &page);
	static struct sum by_tag[256];
	struct sum total = { 0 };
	uint64_t addr = 0;
	printf("regions of %s (pid %d) with %llu MB or more resident:\n", argv[1], pid, min >> 20);
	for (;;) {
		struct regioninfo info;
		if (proc_pidinfo(pid, 7 /* PROC_PIDREGIONINFO */, addr, &info, sizeof(info)) != (int)sizeof(info)) {
			if (total.virt == 0) {
				perror("vmq: proc_pidinfo(PROC_PIDREGIONINFO)");
			}
			break;
		}
		addr = info.pri_address;
		uint64_t size = info.pri_size;
		uint64_t res = (uint64_t)info.pri_pages_resident * page, dirty = (uint64_t)info.pri_pages_dirtied * page;
		unsigned tag = info.pri_user_tag & 0xff;
		by_tag[tag].resident += res;
		by_tag[tag].dirty += dirty;
		by_tag[tag].virt += size;
		by_tag[tag].regions++;
		total.resident += res;
		total.virt += size;
		if (res >= min) {
			printf("  %012llx-%012llx %7llu MB virtual %6llu MB resident %6llu MB dirty  %c%c%c  %s (%u)  share %u\n",
			    addr, addr + size, size >> 20, res >> 20, dirty >> 20, info.pri_protection & 1 ? 'r' : '-',
			    info.pri_protection & 2 ? 'w' : '-', info.pri_protection & 4 ? 'x' : '-', tagname(tag), tag,
			    info.pri_share_mode);
		}
		addr += size;
	}
	printf("by tag (resident MB, dirty MB, virtual MB, regions):\n");
	for (int t = 0; t < 256; t++) {
		if (by_tag[t].resident >= (1 << 20) || by_tag[t].virt >= (256ULL << 20)) {
			printf("  %7llu %7llu %9llu %6u  %s (%d)\n", by_tag[t].resident >> 20, by_tag[t].dirty >> 20,
			    by_tag[t].virt >> 20, by_tag[t].regions, tagname(t), t);
		}
	}
	printf("all: %llu MB resident, %llu MB virtual\n", total.resident >> 20, total.virt >> 20);
	return 0;
}
