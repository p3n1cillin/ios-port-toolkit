/*
 * libr5q-safari.dylib (Test 130): inside MobileSafari, what WebKit thinks of its web views. Safari gets its page
 * over Wi-Fi (Test 127) and never draws it; nothing is suspended (Test 128); WebKit's shared memory, semaphores and
 * IOSurface drawing work between processes (Test 129). So: does Safari's main thread run, what state does the
 * application believe it is in, and for each WKWebView whether WebKit holds it to be in the background (where it
 * stops drawing), its web process, whether that answers, the load's progress. Said on the console every 10 s.
 * Built with the iPhoneOS 16.5 SDK (no compiler of Apple's), injected by the kernel's exec_inject (etc/inject.conf).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SEND(type, obj, sel) ((type (*)(id, SEL))objc_msgSend)((id)(obj), sel_registerName(sel))
#define SEND1(type, obj, sel, t1, a1) ((type (*)(id, SEL, t1))objc_msgSend)((id)(obj), sel_registerName(sel), a1)

static int console = -1;
static volatile unsigned long main_ticks;

static void
say(const char *format, ...)
{
	char line[600];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static bool
responds(id o, const char *sel)
{
	return o != NULL && class_respondsToSelector(object_getClass(o), sel_registerName(sel));
}

static const char *
cstr(id s)
{
	return s != NULL && responds(s, "UTF8String") ? SEND(const char *, s, "UTF8String") : "(nil)";
}

static void
views(id view, int depth, int *found)
{
	Class web = objc_getClass("WKWebView");
	bool is_web = false;
	id subs;

	if (view == NULL || depth > 60) {
		return;
	}
	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		id url = SEND(id, view, "URL");

		(*found)++;
		say("r5q-safari: WKWebView %p: background %s, web process %d, responsive %s, loading %s, progress %.2f, "
		    "in a window %s, hidden %s, URL %s\n", (void *)view,
		    responds(view, "_isBackground") ? (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?",
		    responds(view, "_webProcessIdentifier") ? SEND(int, view, "_webProcessIdentifier") : -1,
		    responds(view, "_webProcessIsResponsive") ? (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?",
		    SEND(BOOL, view, "isLoading") ? "yes" : "no", SEND(double, view, "estimatedProgress"),
		    SEND(id, view, "window") != NULL ? "yes" : "NO", SEND(BOOL, view, "isHidden") ? "YES" : "no",
		    url != NULL ? cstr(SEND(id, url, "absoluteString")) : "(none)");
	}
	subs = SEND(id, view, "subviews");
	for (long i = 0; subs != NULL && i < SEND(long, subs, "count"); i++) {
		views(SEND1(id, subs, "objectAtIndex:", unsigned long, (unsigned long)i), depth + 1, found);
	}
}

static void
report(void *context)
{
	id app = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id scenes, all;
	int found = 0;

	main_ticks++;
	if (app == NULL) {
		say("r5q-safari: no UIApplication yet\n");
	} else {
		say("r5q-safari: main thread runs; application state %ld (0 active, 1 inactive, 2 background)\n",
		    SEND(long, app, "applicationState"));
		scenes = SEND(id, app, "connectedScenes");
		all = scenes != NULL ? SEND(id, scenes, "allObjects") : NULL;
		for (long i = 0; all != NULL && i < SEND(long, all, "count"); i++) {
			id scene = SEND1(id, all, "objectAtIndex:", unsigned long, (unsigned long)i), windows;

			say("r5q-safari: scene %s: activation state %ld (0 foreground active, 1 foreground inactive, 2 background)\n",
			    class_getName(object_getClass(scene)), SEND(long, scene, "activationState"));
			windows = responds(scene, "windows") ? SEND(id, scene, "windows") : NULL;
			for (long w = 0; windows != NULL && w < SEND(long, windows, "count"); w++) {
				views(SEND1(id, windows, "objectAtIndex:", unsigned long, (unsigned long)w), 0, &found);
			}
		}
		say("r5q-safari: %d web views\n", found);
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, report);
	(void)context;
}

/*
 * (Test 131) Where the main thread waits, when it does: its registers from thread_get_state, then the frame-pointer
 * chain (arm64: [fp] = the caller's fp, [fp + 8] = the return address), each address named with dladdr (the image and
 * the nearest exported symbol: the dyld shared cache keeps no local names in memory).
 */
static mach_port_t main_port;

/*
 * (Test 133) The cache's private names are not in memory, but Objective-C's are: every method of every class of the
 * frameworks in Safari's stuck stacks, by its implementation's address, so a frame inside a method is named
 * -[Class selector] (a block or a C function inside one is named after the method before it: "~").
 */
struct imp_name {
	uint64_t imp;
	const char *cls, *sel;
	bool meta;
};
static struct imp_name *imps;
static unsigned imp_count;

static int
imp_cmp(const void *a, const void *b)
{
	uint64_t x = ((const struct imp_name *)a)->imp, y = ((const struct imp_name *)b)->imp;

	return x < y ? -1 : x > y;
}

static void
imps_make(void)
{
	static const char *const frameworks[] = { "SafariShared", "MobileSafari", "AppleMediaServices", "PowerLog",
		"PersonalizationPortrait", "SocialLayer", "SafariSafeBrowsing", "WebBookmarks", "SafariCore", "SafariFoundation" };
	int n = objc_getClassList(NULL, 0);
	Class *all;
	unsigned cap = 65536;

	if (imps != NULL || n <= 0 || (all = malloc(sizeof(Class) * (size_t)n)) == NULL ||
	    (imps = malloc(sizeof(*imps) * cap)) == NULL) {
		return;
	}
	n = objc_getClassList(all, n);
	for (int i = 0; i < n; i++) {
		const char *image = class_getImageName(all[i]);
		bool want = false;

		for (unsigned f = 0; image != NULL && f < sizeof(frameworks) / sizeof(frameworks[0]); f++) {
			want |= strstr(image, frameworks[f]) != NULL;
		}
		for (int m = 0; want && m < 2; m++) {
			Class c = m ? object_getClass((id)all[i]) : all[i];
			unsigned count = 0;
			Method *methods = class_copyMethodList(c, &count);

			for (unsigned k = 0; k < count && imp_count < cap; k++) {
				imps[imp_count].imp = (uint64_t)method_getImplementation(methods[k]) & 0x0000000fffffffffULL;
				imps[imp_count].cls = class_getName(all[i]);
				imps[imp_count].sel = sel_getName(method_getName(methods[k]));
				imps[imp_count].meta = m;
				imp_count++;
			}
			free(methods);
		}
	}
	free(all);
	qsort(imps, imp_count, sizeof(*imps), imp_cmp);
	say("r5q-safari: %u Objective-C methods of %d classes looked at\n", imp_count, n);
}

static void
frame_say(int i, uint64_t a)
{
	Dl_info info;

	a &= 0x0000000fffffffffULL;
	if (a != 0 && dladdr((void *)a, &info) && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/');
		const struct imp_name *best = NULL;

		for (unsigned lo = 0, hi = imp_count; lo < hi;) {      /* the last method at or before a */
			unsigned mid = (lo + hi) / 2;

			if (imps[mid].imp <= a) {
				best = &imps[mid];
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (best != NULL && a - best->imp < 0x2000 && (info.dli_sname == NULL || strstr(info.dli_sname, "redacted"))) {
			say("r5q-safari:   #%02d %#llx %s`~%c[%s %s] + %llu\n", i, a, image ? image + 1 : info.dli_fname,
			    best->meta ? '+' : '-', best->cls, best->sel, a - best->imp);
		} else {
			say("r5q-safari:   #%02d %#llx %s`%s + %llu\n", i, a, image ? image + 1 : info.dli_fname,
			    info.dli_sname ? info.dli_sname : "?", info.dli_saddr ? a - (uint64_t)info.dli_saddr : 0);
		}
	} else {
		say("r5q-safari:   #%02d %#llx\n", i, a);
	}
}

static void
thread_stack_say(mach_port_t th, const char *what)
{
	arm_thread_state64_t st;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	uint64_t fp, frames[48];
	int n = 0;

	if (th == MACH_PORT_NULL || thread_suspend(th) != KERN_SUCCESS) {
		return;
	}
	if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
		frames[n++] = arm_thread_state64_get_pc(st);
		frames[n++] = arm_thread_state64_get_lr(st);
		fp = arm_thread_state64_get_fp(st) & 0x0000000fffffffffULL;
		while (fp != 0 && (fp & 7) == 0 && n < 48) {
			uint64_t next = ((uint64_t *)fp)[0] & 0x0000000fffffffffULL, ret = ((uint64_t *)fp)[1];

			frames[n++] = ret;
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	thread_resume(th);
	say("r5q-safari: %s's stack (%d frames):\n", what, n);
	for (int i = 0; i < n; i++) {
		frame_say(i, frames[i]);
	}
}

/* (Test 132) every thread of Safari: the main thread waits on a queue that one of them holds */
static void
main_stack_say(void)
{
	thread_act_array_t list;
	mach_msg_type_number_t n = 0;
	mach_port_t self = mach_thread_self();
	Dl_info info;

	imps_make();
	if (dladdr((void *)main_stack_say, &info)) {
		say("r5q-safari: this library at %p; the shared cache's libsystem_kernel at %p\n", info.dli_fbase,
		    dladdr((void *)mach_msg, &info) ? info.dli_fbase : NULL);
	}
	thread_stack_say(main_port, "the main thread");
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) {
		return;
	}
	for (mach_msg_type_number_t i = 0; i < n; i++) {
		char what[96], name[64] = "";
		pthread_t pt = pthread_from_mach_thread_np(list[i]);

		if (list[i] != self && list[i] != main_port) {
			if (pt != NULL) {
				pthread_getname_np(pt, name, sizeof(name));
			}
			snprintf(what, sizeof(what), "thread %u \"%s\"", i, name);
			thread_stack_say(list[i], what);
		}
		mach_port_deallocate(mach_task_self(), list[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(list[0]));
	mach_port_deallocate(mach_task_self(), self);
}

static void *
watchdog(void *arg)
{
	unsigned long seen = 0;
	int stuck = 0;

	for (int i = 0; i < 60; i++) {
		sleep(10);
		if (strcmp(getprogname(), "MobileSafari") != 0) {
			/* (Test 134) a daemon of Safari's (the history service): every thread at 40 s and at 120 s */
			if (i == 3 || i == 11) {
				main_stack_say();
			}
			continue;
		}
		if (main_ticks == seen) {
			say("r5q-safari: the main thread has not run the report for 10 s (it is blocked)\n");
			if (++stuck == 2) {             /* (20 s: not the launch's own slow start) */
				main_stack_say();
			}
		} else {
			stuck = 0;
		}
		seen = main_ticks;
	}
	return arg;
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t t;

	main_port = pthread_mach_thread_np(pthread_self());     /* (a constructor runs on the main thread) */
	say("r5q-safari: in pid %d\n", getpid());
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, report);
	pthread_create(&t, NULL, watchdog, NULL);
	pthread_detach(t);
}
