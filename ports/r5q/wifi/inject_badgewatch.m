/*
 * libr5q-badgewatch.dylib (2026-11-20, debugging only, injected into imagent with `r5qd_client.py reload`): why
 * Messages' icon gets a red "!" at every boot with no messages at all. Wraps IMDBadgeUtilities' -_postBadgeString:,
 * -_postBadgeNumber:, -_compareLastFailureDateAndUpdateBadge: and -_clearFailureBadge, and says each call with its
 * argument and the caller's stack (dladdr) on the console. Nothing is changed.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <unistd.h>

static int console = -1;

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static void
stack(void)
{
	void *f[12];
	int n = backtrace(f, 12);
	for (int i = 2; i < n; i++) {
		Dl_info d;
		if (dladdr(f[i], &d) != 0) {
			const char *img = d.dli_fname ? strrchr(d.dli_fname, '/') : NULL;
			say("badgewatch:    %s %s (image+0x%lx)\n", img ? img + 1 : "?", d.dli_sname ? d.dli_sname : "?",
			    (unsigned long)((uintptr_t)f[i] - (uintptr_t)d.dli_fbase));
		}
	}
}

static IMP o_str, o_num, o_cmp, o_clr, o_should;

static BOOL
w_should(id self, SEL cmd)
{
	BOOL r = ((BOOL (*)(id, SEL))o_should)(self, cmd);
	say("badgewatch: _shouldShowFailureString -> %d\n", r);
	return r;
}

static void
w_str(id self, SEL cmd, id s)
{
	say("badgewatch: _postBadgeString: %s\n", [[s description] UTF8String]);
	stack();
	((void (*)(id, SEL, id))o_str)(self, cmd, s);
}

static void
w_num(id self, SEL cmd, id n)
{
	say("badgewatch: _postBadgeNumber: %s\n", [[n description] UTF8String]);
	((void (*)(id, SEL, id))o_num)(self, cmd, n);
}

static void
w_cmp(id self, SEL cmd, id a)
{
	say("badgewatch: _compareLastFailureDateAndUpdateBadge: %s\n", [[a description] UTF8String]);
	stack();
	((void (*)(id, SEL, id))o_cmp)(self, cmd, a);
}

static void
w_clr(id self, SEL cmd)
{
	say("badgewatch: _clearFailureBadge\n");
	((void (*)(id, SEL))o_clr)(self, cmd);
}

static IMP
wrap(Class c, const char *sel, IMP w)
{
	Method m = class_getInstanceMethod(c, sel_registerName(sel));
	if (m == NULL) {
		say("badgewatch: no -%s\n", sel);
		return NULL;
	}
	say("badgewatch: -%s %s\n", sel, method_getTypeEncoding(m));
	return method_setImplementation(m, w);
}

static int tries;

static void
hook(void *unused)
{
	Class c = objc_getClass("IMDBadgeUtilities");
	(void)unused;
	if (c == Nil) {
		if (++tries < 600) {   /* (up to 150 s: imagent loads IMDaemonCore later) */
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_global_queue(0, 0), NULL, hook);
		}
		return;
	}
	o_str = wrap(c, "_postBadgeString:", (IMP)w_str);
	o_num = wrap(c, "_postBadgeNumber:", (IMP)w_num);
	o_cmp = wrap(c, "_compareLastFailureDateAndUpdateBadge:", (IMP)w_cmp);
	o_clr = wrap(c, "_clearFailureBadge", (IMP)w_clr);
	{
		Method m = class_getInstanceMethod(c, sel_registerName("isUnexpectedlyLoggedOut"));
		Dl_info d;
		if (m != NULL && dladdr((void *)method_getImplementation(m), &d) != 0) {
			say("badgewatch: _shouldShowFailureString at IMDaemonCore+0x%lx\n",
			    (unsigned long)((uintptr_t)method_getImplementation(m) - (uintptr_t)d.dli_fbase));
			o_should = method_setImplementation(m, (IMP)w_should);
		}
	}
	/* every method of the class (instance, then class), with where its code is, for reading it in the shared cache */
	for (int meta = 0; meta < 2; meta++) {
		unsigned n = 0;
		Method *all = class_copyMethodList(meta ? object_getClass((id)c) : c, &n);
		for (unsigned i = 0; i < n; i++) {
			Dl_info d;
			IMP imp = method_getImplementation(all[i]);
			if (dladdr((void *)imp, &d) != 0) {
				const char *img = d.dli_fname ? strrchr(d.dli_fname, '/') : NULL;
				say("badgewatch: %c%s at %s+0x%lx\n", meta ? '+' : '-', sel_getName(method_getName(all[i])),
				    img ? img + 1 : "?", (unsigned long)((uintptr_t)imp - (uintptr_t)d.dli_fbase));
			}
		}
		free(all);
	}
}

__attribute__((constructor)) static void
start(void)
{
	console = open("/dev/console", O_WRONLY);
	say("badgewatch: loaded in %s\n", getprogname());
	hook(NULL);
}
