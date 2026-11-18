/*
 * libr5q-sbdiag.dylib (Test 149): why SpringBoard never auto-locks. 60 s after start, on a background queue: every
 * Objective-C class whose name has "IdleTimer" in it, with its methods; then, for each such class answering
 * +sharedInstance or +sharedCoordinator (or similar), the instance's -description and every argument-less method of it
 * whose name says what holds the timer off (Disabled, Assertion, Reason, Active, Expir, Duration, Timeout). Read only.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int console = -1;

static void
say(const char *format, ...)
{
	char line[700];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static const char *
describe(id object)
{
	id d;

	if (object == NULL) {
		return "nil";
	}
	d = ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	if (d == NULL) {
		return "?";
	}
	{
		/* one line: newlines and runs of blanks squeezed (Test 149d) */
		static char flat[8][1200];
		static unsigned next;
		char *out = flat[next++ % 8];
		const char *in = ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String"));
		unsigned k = 0;

		for (; in && *in && k < sizeof(flat[0]) - 1; in++) {
			char c = (*in == 10 || *in == 9) ? ' ' : *in;

			if (c == ' ' && k > 0 && out[k - 1] == ' ') {
				continue;
			}
			out[k++] = c;
		}
		out[k] = 0;
		return out;
	}
}

static bool
interesting(const char *name)
{
	static const char *const words[] = { "isabled", "ssertion", "eason", "ctive", "xpir", "uration", "imeout", "ttention",
		                             "dle", "Lock", "isable" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strstr(name, words[i]) != NULL) {
			return true;
		}
	}
	return false;
}

static void
probe_instance(const char *cls_name, id object)
{
	Class cls = object_getClass(object);
	unsigned count = 0;
	Method *methods = class_copyMethodList(cls, &count);

	say("sbdiag: %s instance: %.400s\n", cls_name, describe(object));
	for (unsigned i = 0; methods && i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));
		char type[64];

		if (strchr(name, ':') != NULL || !interesting(name)) {
			continue;
		}
		method_getReturnType(methods[i], type, sizeof(type));
		if (type[0] == 'B' || type[0] == 'c') {
			bool v = ((bool (*)(id, SEL))objc_msgSend)(object, method_getName(methods[i]));

			say("sbdiag:   -%s = %s\n", name, v ? "YES" : "NO");
		} else if (type[0] == 'd') {
			double v = ((double (*)(id, SEL))objc_msgSend)(object, method_getName(methods[i]));

			say("sbdiag:   -%s = %f\n", name, v);
		} else if (type[0] == 'q' || type[0] == 'Q' || type[0] == 'i' || type[0] == 'I') {
			long v = ((long (*)(id, SEL))objc_msgSend)(object, method_getName(methods[i]));

			say("sbdiag:   -%s = %ld\n", name, v);
		} else if (type[0] == '@') {
			id v = ((id (*)(id, SEL))objc_msgSend)(object, method_getName(methods[i]));

			say("sbdiag:   -%s = %.300s\n", name, describe(v));
		}
	}
	free(methods);
}

/* an object's object fields, each described, and the AW* (AttentionAwareness) ones probed one level down */
static void
dump_ivars(const char *what, id object, int depth)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(object), &count);

	for (unsigned i = 0; ivars && i < count; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);

		if (type == NULL || type[0] != '@') {
			continue;
		}
		id value = object_getIvar(object, ivars[i]);

		say("sbdiag: %s.%s = %.500s\n", what, ivar_getName(ivars[i]), describe(value));
		if (value != NULL && depth > 0) {
			const char *cn = object_getClassName(value);

			if (strncmp(cn, "AW", 2) == 0 || strstr(cn, "Attention") != NULL || strstr(cn, "IdleTimer") != NULL) {
				dump_ivars(cn, value, depth - 1);   /* (Test 149b: fields only -- a method called here crashed SpringBoard) */
			} else if (strstr(cn, "Dictionary") != NULL || strstr(cn, "Array") != NULL) {
				id values = strstr(cn, "Array") ? value :
				    ((id (*)(id, SEL))objc_msgSend)(value, sel_registerName("allValues"));
				long n = values ? ((long (*)(id, SEL))objc_msgSend)(values, sel_registerName("count")) : 0;

				for (long k = 0; k < n && k < 6; k++) {
					id v = ((id (*)(id, SEL, long))objc_msgSend)(values, sel_registerName("objectAtIndex:"), k);

					dump_ivars(object_getClassName(v), v, depth - 1);
				}
			}
		}
	}
	free(ivars);
}

static void run(void *unused);

static void
attention(void *unused)
{
	Class cls = objc_getClass("LKAttentionAwareIdleTimer");
	SEL sel = sel_registerName("sharedInstance");

	(void)unused;
	if (cls != NULL && class_getClassMethod(cls, sel) != NULL) {
		id timer = ((id (*)(Class, SEL))objc_msgSend)(cls, sel);

		if (timer != NULL) {
			say("sbdiag: ---- the attention-aware idle timer, in depth\n");
			dump_ivars("LKAttentionAwareIdleTimer", timer, 2);
		}
	}
	{
		Class g = objc_getClass("SBIdleTimerGlobalCoordinator");
		id c = g ? ((id (*)(Class, SEL))objc_msgSend)(g, sel_registerName("sharedInstance")) : NULL;

		if (c != NULL) {
			id t = ((id (*)(id, SEL))objc_msgSend)(c, sel_registerName("idleTimer"));

			say("sbdiag: the idle timer now: %.400s\n", describe(t));
			if (t != NULL) {
				dump_ivars("SBBacklightIdleTimer", t, 3);
			}
		}
	}
}

static void
run(void *unused)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	static const char *const getters[] = { "sharedInstance", "sharedCoordinator", "sharedIdleTimerCoordinator", "shared",
		                               "defaultCoordinator", "sharedController" };

	(void)unused;
	for (unsigned i = 0; classes && i < count; i++) {
		const char *name = class_getName(classes[i]);
		unsigned m = 0;
		Method *methods;

		if (strstr(name, "IdleTimer") == NULL && strstr(name, "AutoLock") == NULL) {
			continue;
		}
		say("sbdiag: class %s\n", name);
		methods = class_copyMethodList(object_getClass((id)classes[i]), &m);
		for (unsigned k = 0; methods && k < m; k++) {
			say("sbdiag:   + %s\n", sel_getName(method_getName(methods[k])));
		}
		free(methods);
		for (unsigned g = 0; g < sizeof(getters) / sizeof(getters[0]); g++) {
			SEL sel = sel_registerName(getters[g]);

			if (class_getClassMethod(classes[i], sel) != NULL) {
				id object = ((id (*)(Class, SEL))objc_msgSend)(classes[i], sel);

				if (object != NULL) {
					probe_instance(name, object);
				}
				break;
			}
		}
	}
	free(classes);
}

/* (Test 149c) in backboardd, which hosts com.apple.AttentionAwareness: the AW* classes and their shared objects' fields */
static void
attention_server(void *unused)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	static const char *const getters[] = { "sharedInstance", "sharedServer", "sharedScheduler", "sharedManager", "shared" };

	(void)unused;
	for (unsigned i = 0; classes && i < count; i++) {
		const char *name = class_getName(classes[i]);

		if (strncmp(name, "AW", 2) != 0) {
			continue;
		}
		say("bbdiag: class %s\n", name);
		for (unsigned g = 0; g < sizeof(getters) / sizeof(getters[0]); g++) {
			SEL sel = sel_registerName(getters[g]);

			if (class_getClassMethod(classes[i], sel) != NULL) {
				id object = ((id (*)(Class, SEL))objc_msgSend)(classes[i], sel);

				say("bbdiag: +[%s %s] = %.300s\n", name, getters[g], describe(object));
				if (object != NULL) {
					dump_ivars(name, object, 2);
				}
				break;
			}
		}
	}
	free(classes);
}

/* each AWScheduler client: identifier and last event, one line each */
static void
attention_events(void *unused)
{
	Class s = objc_getClass("AWScheduler");
	id scheduler = s ? ((id (*)(Class, SEL))objc_msgSend)(s, sel_registerName("sharedScheduler")) : NULL;
	Ivar clients_ivar = scheduler ? class_getInstanceVariable(object_getClass(scheduler), "_clients") : NULL;
	id clients = clients_ivar ? object_getIvar(scheduler, clients_ivar) : NULL;
	long n = clients ? ((long (*)(id, SEL))objc_msgSend)(clients, sel_registerName("count")) : 0;
	uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);

	(void)unused;
	for (long i = 0; i < n; i++) {
		id c = ((id (*)(id, SEL, long))objc_msgSend)(clients, sel_registerName("objectAtIndex:"), i);
		Ivar idv = class_getInstanceVariable(object_getClass(c), "_identifier");
		Ivar ev = class_getInstanceVariable(object_getClass(c), "_lastEvent");

		const char *ident = idv ? describe(object_getIvar(c, idv)) : "?";

		say("aw: now %llu.%01llu s: %s: %.200s\n", now / 1000000000ull, now / 100000000ull % 10, ident,
		    ev ? describe(object_getIvar(c, ev)) : "?");
		if (strstr(ident, "GlobalBacklight") != NULL) {
			/* (Test 150) every field of the backlight's client, numbers too: which one keeps moving */
			unsigned count = 0;
			Ivar *ivars = class_copyIvarList(object_getClass(c), &count);

			for (unsigned k = 0; ivars && k < count; k++) {
				const char *t = ivar_getTypeEncoding(ivars[k]);
				ptrdiff_t off = ivar_getOffset(ivars[k]);
				const uint8_t *base = (const uint8_t *)(void *)c;

				if (t == NULL) {
					continue;
				}
				if (t[0] == 'd') {
					say("aw:   %s = %f\n", ivar_getName(ivars[k]), *(const double *)(base + off));
				} else if (t[0] == 'Q' || t[0] == 'q') {
					say("aw:   %s = %lld\n", ivar_getName(ivars[k]), *(const long long *)(base + off));
				} else if (t[0] == 'I' || t[0] == 'i' || t[0] == 'B' || t[0] == 'c' || t[0] == 'C') {
					say("aw:   %s = %d\n", ivar_getName(ivars[k]), t[0] == 'B' || t[0] == 'c' || t[0] == 'C' ?
					    *(const uint8_t *)(base + off) : *(const int *)(base + off));
				} else if (t[0] == '@') {
					say("aw:   %s = %.160s\n", ivar_getName(ivars[k]), describe(object_getIvar(c, ivars[k])));
				} else {
					say("aw:   %s (%s)\n", ivar_getName(ivars[k]), t);
				}
			}
			free(ivars);
		}
	}
}

/*
 * (Test 151) in SpringBoard: who re-sends the backlight idle timer's configuration once a second. Every
 * -[AWAttentionAwarenessClient setConfiguration...] (object and BOOL/pointer arguments only: forwarded as pointers)
 * is logged with its caller stack (dladdr names), the first 6 times.
 */
#include <execinfo.h>
#include <dlfcn.h>
static IMP aw_set_originals[8];
static SEL aw_set_selectors[8];
static unsigned aw_set_count, aw_calls;

static void *
aw_set_hook(id self, SEL sel, void *a, void *b, void *c, void *d)
{
	IMP original = NULL;

	for (unsigned i = 0; i < aw_set_count; i++) {
		if (aw_set_selectors[i] == sel) {
			original = aw_set_originals[i];
		}
	}
	if (aw_calls++ < 2) {
		void *frames[24];
		int n = backtrace(frames, 24);

		say("swhy: %s call %u\n", sel_getName(sel), aw_calls);
		for (int i = 1; i < n; i++) {
			Dl_info info;

			if (dladdr(frames[i], &info) != 0) {
				const char *image = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
				char method[200] = "";

				if (info.dli_fname != NULL) {
					/* (the nearest Objective-C method at or below the address, among the image's classes) */
					unsigned nclasses = 0;
					const char **names = objc_copyClassNamesForImage(info.dli_fname, &nclasses);
					uintptr_t best = 0;

					for (unsigned k = 0; names && k < nclasses; k++) {
						Class cls = objc_getClass(names[k]);

						for (int meta = 0; cls && meta < 2; meta++) {
							Class c = meta ? object_getClass((id)cls) : cls;
							unsigned nm = 0;
							Method *ms = class_copyMethodList(c, &nm);

							for (unsigned m = 0; ms && m < nm; m++) {
								uintptr_t imp = (uintptr_t)method_getImplementation(ms[m]);

								if (imp <= (uintptr_t)frames[i] && imp > best) {
									best = imp;
									snprintf(method, sizeof(method), "%c[%s %s]+%#lx", meta ? '+' : '-', names[k],
									    sel_getName(method_getName(ms[m])), (unsigned long)((uintptr_t)frames[i] - imp));
								}
							}
							free(ms);
						}
					}
					free(names);
				}
				say("swhy:   #%d %s+%#lx (%s) %s\n", i, info.dli_sname ? info.dli_sname : "?",
				    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_saddr), image ? image + 1 : "?", method);
			}
		}
	}
	return original ? ((void *(*)(id, SEL, void *, void *, void *, void *))original)(self, sel, a, b, c, d) : NULL;
}

static void
watch_aw_configuration(void *unused)
{
	Class cls = objc_getClass("AWAttentionAwarenessClient");
	unsigned count = 0;
	Method *methods = cls ? class_copyMethodList(cls, &count) : NULL;

	(void)unused;
	for (unsigned i = 0; methods && i < count && aw_set_count < 8; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		say("swhy: -[AWAttentionAwarenessClient %s]\n", name);
		if (strncmp(name, "setConfiguration", 16) == 0 || strncmp(name, "resetAttentionLostTimer", 23) == 0) {
			aw_set_selectors[aw_set_count] = method_getName(methods[i]);
			aw_set_originals[aw_set_count++] = method_setImplementation(methods[i], (IMP)aw_set_hook);
			say("swhy: hooked %s\n", name);
		}
	}
	free(methods);
}

__attribute__((constructor)) static void
start(void)
{
	if (strcmp(getprogname(), "SpringBoard") == 0 && getenv("R5Q_SBDIAG_OLD") == NULL) {
		console = open("/dev/console", O_WRONLY);
		dlopen("/System/Library/PrivateFrameworks/AttentionAwareness.framework/AttentionAwareness", RTLD_LAZY);
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 70 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
		    watch_aw_configuration);
		return;
	}
	console = open("/dev/console", O_WRONLY);
	if (strcmp(getprogname(), "backboardd") == 0) {
		/* (Test 149e) every 5 s from 40 s on, for two minutes: each attention client's last event */
		(void)attention_server;
		for (int t = 40; t <= 120; t += 20) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)t * NSEC_PER_SEC), dispatch_get_main_queue(), NULL,
			    attention_events);
		}
		return;
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, attention);
	(void)probe_instance;
	(void)run;
}
