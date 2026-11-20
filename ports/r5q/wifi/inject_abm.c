/*
 * libr5q-abm.dylib (2026-11-20, Test 377): engage the baseband manager's IPC router by hand, inside CommCenter.
 *
 * With the fake boot (apfs_vnops.c, bbfake=1) the manager says "Booted", CommCenter's QMI clients send QMI CTL
 * GET_CLIENT_ID on qmux1 and every one is "stalled": the router (RouterIPCController) stays kDisengaged, because
 * nothing calls QMITransportService's engage (libBasebandManager +0x1865e8, slot 6 of its vtable at 0x1d90facb8 in
 * the 20F66 iPhone10,3 shared cache; the same method is slot 0 of its TransportService interface at +0x68, vtable
 * 0x1d90fad68). What normally calls it is not found yet (CELLULAR-PLAN.md, Tests 373-376).
 *
 * This library, injected into CommCenter (kern.exec_inject, `r5qd_client.py reload`), waits 20 s, finds the
 * QMITransportService object by scanning CommCenter's writable memory for its two vtable pointers (primary at +0,
 * interface at +0x68), and calls engage(this) on a background queue. It logs through os_log (subsystem
 * com.redwood.abm), read on the PC with toolkit/tools/ios/tracev3.py.
 * Only for the 20F66 cache (it checks the code bytes before calling anything).
 */
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdint.h>
#include <string.h>

#define CACHE_VTABLE_PRIMARY    0x1d90facb8ULL
#define CACHE_VTABLE_IFACE      0x1d90fad68ULL
#define CACHE_ENGAGE            0x1b7fec5e8ULL
#define IFACE_OFFSET            0x68

/* (in libsystem_kernel; the iOS SDK leaves mach_vm.h's declarations out) */
extern kern_return_t mach_vm_region(vm_map_read_t task, mach_vm_address_t *address, mach_vm_size_t *size,
    vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object_name);

static os_log_t lg;

static intptr_t
cache_slide(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		if (n != NULL && strstr(n, "/libBasebandManager.dylib") != NULL) {
			return _dyld_get_image_vmaddr_slide(i);
		}
	}
	return -1;
}

static void
engage_by_hand(void)
{
	intptr_t slide = cache_slide();
	uint64_t vp, vi, fn;
	mach_vm_address_t addr = 0;
	mach_vm_size_t size;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count;
	mach_port_t obj;
	void *found = NULL;
	int matches = 0;
	/* sub sp, sp, #0x120 ; stp d9, d8, [sp, #0xc0] : the method's first two instructions in 20F66 */
	static const uint32_t expect[2] = { 0xd10483ff, 0x6d0c23e9 };

	if (slide == -1) {
		os_log(lg, "r5q-abm: libBasebandManager is not loaded here");
		return;
	}
	vp = CACHE_VTABLE_PRIMARY + slide;
	vi = CACHE_VTABLE_IFACE + slide;
	fn = CACHE_ENGAGE + slide;
	if (memcmp((const void *)fn, expect, sizeof(expect)) != 0) {
		os_log(lg, "r5q-abm: engage's code is not the expected (another cache?): %08x %08x",
		    ((const uint32_t *)fn)[0], ((const uint32_t *)fn)[1]);
		return;
	}
	for (;;) {
		count = VM_REGION_BASIC_INFO_COUNT_64;
		if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
		    &obj) != KERN_SUCCESS) {
			break;
		}
		if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && !info.shared &&
		    size < (512ULL << 20)) {
			const uint64_t *p = (const uint64_t *)addr, *end = (const uint64_t *)(addr + size);
			for (; p + IFACE_OFFSET / 8 < end; p++) {
				if (*p == vp && p[IFACE_OFFSET / 8] == vi) {
					matches++;
					found = (void *)p;
				}
			}
		}
		addr += size;
	}
	os_log(lg, "r5q-abm: slide 0x%lx, %d QMITransportService object(s); using %p", (long)slide, matches, found);
	if (matches != 1) {
		return;
	}
	os_log(lg, "r5q-abm: calling engage(%p)", found);
	((void (*)(void *))fn)(found);
	os_log(lg, "r5q-abm: engage returned");
}

__attribute__((constructor))
static void
start(void)
{
	lg = os_log_create("com.redwood.abm", "inject");
	os_log(lg, "r5q-abm: loaded; engaging the router by hand in 20 s");
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
	    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ engage_by_hand(); });
}
