/*
 * libr5q-bigmalloc.dylib (2026-11-20, Test 394): who in a process makes allocations of 30 MB or more. Injected
 * (kern.exec_inject, `r5qd_client.py reload`) into mediaserverd, which with the Camera open held one 39 MB malloc
 * block more every frame until the session ended -- 56 GB of its address space (wifi/vmq.c: 1511 MALLOC_LARGE
 * regions) and 2.7 GB of the phone's memory at the worst. Every malloc zone's malloc/calloc/realloc/memalign are
 * wrapped -- the zones there are at start, and again 2, 10 and 30 s later for zones made since (the default zone saw
 * none of these blocks). An allocation of 30 MB or more is said on the console (/dev/console: the USB live log,
 * `phone.py live`) with the zone and the caller's stack, each image and nearest symbol (dladdr) -- the first 12 in
 * full, then one line every 100th. Nothing else is changed.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BIG     (30u << 20)
#define ZONES   32

static int console = -1;
static struct wrapped {
	malloc_zone_t *zone;
	void *(*malloc)(malloc_zone_t *, size_t);
	void *(*calloc)(malloc_zone_t *, size_t, size_t);
	void *(*realloc)(malloc_zone_t *, void *, size_t);
	void *(*memalign)(malloc_zone_t *, size_t, size_t);
} zones[ZONES];
static volatile int nzones, seen;
static __thread int inside;

static void
say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static struct wrapped *
find(malloc_zone_t *z)
{
	for (int i = 0; i < nzones; i++) {
		if (zones[i].zone == z) {
			return &zones[i];
		}
	}
	abort();        /* (only wrapped zones come here) */
}

static void
big(malloc_zone_t *z, const char *how, size_t size, void *p)
{
	int k = __sync_add_and_fetch(&seen, 1);
	const char *zn = z != NULL ? malloc_get_zone_name(z) : "(logger)";

	if (inside) {
		return;
	}
	inside = 1;
	if (k <= 12) {
		void *frames[28];
		int n = backtrace(frames, 28);

		say("bigmalloc: #%d %s %zu bytes -> %p, zone %s, thread %p:\n", k, how, size, p, zn ? zn : "?",
		    (void *)pthread_self());
		for (int i = 2; i < n; i++) {
			Dl_info info;
			if (dladdr(frames[i], &info) != 0) {
				const char *image = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
				/* (the image's own offset too: a <redacted> symbol is looked up in the shared cache by it) */
				say("bigmalloc:   %2d %s %s+0x%lx (image+0x%lx)\n", i - 2, image ? image + 1 : "?",
				    info.dli_sname ? info.dli_sname : "?",
				    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)(info.dli_sname ? info.dli_saddr : info.dli_fbase)),
				    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
			} else {
				say("bigmalloc:   %2d %p\n", i - 2, frames[i]);
			}
		}
		/*
		 * (Test 394) CMCapture+0x37b90 sizes its block buffer as bytesPerFrame * ([x20+0x38] + [x20+0x14]); the
		 * frame record whose return address is CMCapture+0x37c4c is CMBlockBufferCreateWithMemoryBlock's, whose
		 * prologue keeps the caller's x20 at fp-0x10, x23 at fp-0x28, x24 at fp-0x30, x26 at fp-0x40.
		 */
		for (uintptr_t *fp = __builtin_frame_address(0); fp != NULL && k <= 4; fp = (uintptr_t *)fp[0]) {
			Dl_info info;
			uintptr_t lr = fp[1] & 0x0000000fffffffffULL;

			if (k == 1 && dladdr((void *)lr, &info) != 0) {
				say("bigmalloc:   fp %p lr %lx %s+0x%lx\n", (void *)fp, (unsigned long)fp[1],
				    info.dli_fname ? info.dli_fname : "?", (unsigned long)(lr - (uintptr_t)info.dli_fbase));
			}

			if (dladdr((void *)lr, &info) != 0 && info.dli_fname != NULL && strstr(info.dli_fname, "/CMCapture") != NULL &&
			    lr - (uintptr_t)info.dli_fbase == 0x37c4c) {
				const uint32_t *s = (const uint32_t *)fp[-2], *fmt = (const uint32_t *)fp[-5];

				say("bigmalloc:   x24 %u, x26 %u; x23 (format) %p:", (uint32_t)fp[-6], (uint32_t)fp[-8], (void *)fmt);
				for (int i = 0; fmt != NULL && i < 10; i++) {
					say(" %08x", fmt[i]);
				}
				say("\nbigmalloc:   x20 %p:", (void *)s);
				for (int i = 0; s != NULL && i < 20; i++) {
					say(" %08x", s[i]);
				}
				say("\n");
				break;
			}
		}
	} else if (k % 100 == 0) {
		say("bigmalloc: #%d %s %zu bytes, zone %s\n", k, how, size, zn ? zn : "?");
	}
	inside = 0;
}

static void *
my_malloc(malloc_zone_t *z, size_t size)
{
	void *p = find(z)->malloc(z, size);
	if (size >= BIG) {
		big(z, "malloc", size, p);
	}
	return p;
}

static void *
my_calloc(malloc_zone_t *z, size_t n, size_t size)
{
	void *p = find(z)->calloc(z, n, size);
	if (n * size >= BIG) {
		big(z, "calloc", n * size, p);
	}
	return p;
}

static void *
my_realloc(malloc_zone_t *z, void *old, size_t size)
{
	void *p = find(z)->realloc(z, old, size);
	if (size >= BIG) {
		big(z, "realloc", size, p);
	}
	return p;
}

static void *
my_memalign(malloc_zone_t *z, size_t align, size_t size)
{
	void *p = find(z)->memalign(z, align, size);
	if (size >= BIG) {
		big(z, "memalign", size, p);
	}
	return p;
}

static void
wrap(malloc_zone_t *z)
{
	vm_address_t page = (vm_address_t)z & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t len = ((vm_address_t)z + sizeof(*z) - page + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
	struct wrapped *w;
	const char *zn = malloc_get_zone_name(z);

	for (int i = 0; i < nzones; i++) {
		if (zones[i].zone == z) {
			return;
		}
	}
	if (nzones == ZONES || z->malloc == NULL) {
		return;
	}
	w = &zones[nzones];
	w->zone = z;
	w->malloc = z->malloc;
	w->calloc = z->calloc;
	w->realloc = z->realloc;
	w->memalign = z->version >= 5 ? z->memalign : NULL;
	if (vm_protect(mach_task_self(), page, len, 0, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
		say("bigmalloc: zone %s cannot be made writable: not wrapped\n", zn ? zn : "?");
		return;
	}
	__sync_synchronize();
	nzones++;               /* (the table entry is in place before the zone points here) */
	__sync_synchronize();
	z->malloc = my_malloc;
	z->calloc = my_calloc;
	z->realloc = my_realloc;
	if (w->memalign != NULL) {
		z->memalign = my_memalign;
	}
	(void)vm_protect(mach_task_self(), page, len, 0, VM_PROT_READ);
	say("bigmalloc: zone %s wrapped (%d)\n", zn ? zn : "?", nzones);
}

static void
wrap_all(void *unused)
{
	vm_address_t *list = NULL;
	unsigned count = 0;

	(void)unused;
	if (malloc_get_all_zones(mach_task_self(), NULL, &list, &count) != KERN_SUCCESS) {
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		wrap((malloc_zone_t *)list[i]);
	}
}

/*
 * The loggers libmalloc and libsystem_kernel call for every allocation when stack logging is on (malloc_logger for a
 * zone's, __syscall_logger for vm_allocate/mmap's): they see what the zone functions do not. type: 2 = allocate,
 * 0x10 = a VM allocation (its tag in the top byte); arg2 = the size, result = the address.
 */
typedef void (logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result, uint32_t skip);
extern logger_t *malloc_logger;
extern logger_t *__syscall_logger;

static void
logged(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result, uint32_t skip)
{
	(void)arg1;
	(void)arg3;
	(void)skip;
	if ((type & 2) && !(type & 4) && arg2 >= BIG) {
		big(NULL, type & 0x10 ? "vm_allocate" : "malloc (logger)", (size_t)arg2, (void *)result);
	} else if ((type & 6) == 6 && arg3 >= BIG) {
		big(NULL, "realloc (logger)", (size_t)arg3, (void *)result);
	}
}

__attribute__((constructor)) static void
start(void)
{
	console = open("/dev/console", O_WRONLY);
	say("bigmalloc: allocations of %u MB or more are said, in %s\n", BIG >> 20, getprogname());
	malloc_logger = logged;
	__syscall_logger = logged;
	wrap_all(NULL);
	dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), q, NULL, wrap_all);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), q, NULL, wrap_all);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), q, NULL, wrap_all);
}
