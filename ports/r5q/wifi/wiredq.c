/*
 * wiredq [MIN_MB] (2026-11-20): who holds the kernel's wired memory, from kern.redwood.wired (redwood_ufs.c) -- the
 * records mach_memory_info() gives zprint on a Mac: the totals (wired, reserved at boot, stolen, kernelcache), every
 * tag (VM_KERN_MEMORY_*) and allocation site, and every zone, the ones of MIN_MB (default 4) or more shown, biggest
 * first. Why: 3.9 GB of the phone's 6 were wired, the /var RAM filesystem only 0.9 GB of it, and jetsam was killing
 * mediaserverd for a HEIC photo at 13% memory free.
 */
#include <mach_debug/zone_info.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#define SITE_TYPE       0xff
#define SITE_TAG        0
#define SITE_KMOD       1
#define SITE_KERNEL     2
#define SITE_COUNTER    3
#define SITE_ZONE       0x800
#define SITE_ZONE_VIEW  0x1000
#define SITE_KALLOC     0x2000

static const char *const tags[] = {
	"none", "osfmk", "bsd", "iokit", "libkern", "oskext", "kext", "ipc", "stack", "cpu", "pmap", "pte", "zone",
	"kalloc", "compressor", "compressed_data", "phantom_cache", "waitq", "diag", "log", "file", "mbuf", "ubc",
	"security", "mlock", "reason", "skywalk", "ltable", "hv", "kalloc_data", "retired", "kalloc_type", "triage",
	"recount",
};
static const char *const counters[] = {
	"managed (all pages)", "reserved (wired at boot, less stolen)", "wired", "wired managed", "stolen", "lopage",
	"kernel map", "zone map", "kalloc map", "wired at boot", "booter", "kernelcache", "kalloc large data map",
	"kernel data map",
};

static int
cmp(const void *a, const void *b)
{
	const mach_memory_info_t *x = a, *y = b;
	return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

int
main(int argc, char **argv)
{
	uint64_t min = (uint64_t)(argc > 1 ? atoi(argv[1]) : 4) << 20;
	size_t len = 0;
	mach_memory_info_t *info;
	unsigned n;

	if (sysctlbyname("kern.redwood.wired", NULL, &len, NULL, 0) != 0 || len == 0) {
		perror("wiredq: kern.redwood.wired (root, and a kernel from Test 393 on)");
		return 1;
	}
	info = calloc(1, len);
	if (sysctlbyname("kern.redwood.wired", info, &len, NULL, 0) != 0) {
		perror("wiredq: read");
		return 1;
	}
	n = (unsigned)(len / sizeof(*info));
	printf("%u records (%zu bytes each)\n", n, sizeof(*info));
	/* the totals first, in their own order */
	for (unsigned i = 0; i < n; i++) {
		if ((info[i].flags & SITE_TYPE) == SITE_COUNTER && info[i].site < sizeof(counters) / sizeof(counters[0])) {
			printf("  total %-40s %8llu MB", counters[info[i].site], info[i].size >> 20);
			if (info[i].free != 0) {
				printf("  (free %llu MB, largest %llu MB)", info[i].free >> 20, info[i].largest >> 20);
			}
			printf("\n");
		}
	}
	qsort(info, n, sizeof(*info), cmp);
	uint64_t tagged = 0, zoned = 0;
	for (unsigned i = 0; i < n; i++) {
		unsigned type = info[i].flags & SITE_TYPE;
		if (type == SITE_COUNTER) {
			continue;
		}
		if (info[i].flags & (SITE_ZONE | SITE_ZONE_VIEW)) {
			zoned += info[i].size;
		} else {
			tagged += info[i].size;
		}
		if (info[i].size < min) {
			continue;
		}
		const char *kind = info[i].flags & (SITE_ZONE | SITE_ZONE_VIEW) ? "zone" : type == SITE_TAG ? "tag" :
		    type == SITE_KMOD ? "kmod" : type == SITE_KERNEL ? "site" : "?";
		const char *name = info[i].name[0] ? info[i].name :
		    info[i].tag < sizeof(tags) / sizeof(tags[0]) ? tags[info[i].tag] : "";
		printf("%8llu MB  %-4s tag %3u  %s", info[i].size >> 20, kind, info[i].tag, name);
		if (info[i].mapped != 0 && info[i].mapped != info[i].size) {
			printf("  (mapped %llu MB)", info[i].mapped >> 20);
		}
		if (info[i].peak > info[i].size) {
			printf("  (peak %llu MB)", info[i].peak >> 20);
		}
		printf("\n");
	}
	printf("tags and sites together %llu MB; zones together %llu MB (the zones are inside the zone tags)\n",
	    tagged >> 20, zoned >> 20);
	return 0;
}
