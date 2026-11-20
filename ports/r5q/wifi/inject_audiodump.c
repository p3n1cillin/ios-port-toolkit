/*
 * libr5q-audiodump.dylib (2026-11-20, debugging only, injected with `r5qd_client.py reload` under a new name each time):
 * every ivar of each BWAudioSourceNode -- name, type encoding, offset and value (integers, doubles, CMTime-like
 * {qiIq} structs, pointers) -- said on the console 1 s and 4 s after the node's -start:, with mach_absolute_time
 * beside it. Why: the node's silence filling saw host time elapsed run a constant 468.7 s ahead of the audio's sample
 * time (wifi/MEMORY-PLAN.md); its start times and the callbacks' clocks are in these ivars.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int console = -1;
static BOOL (*orig_start)(id, SEL, id *);

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

static void
dump(void *node)
{
	unsigned n = 0;
	Ivar *all = class_copyIvarList(objc_getClass("BWAudioSourceNode"), &n);
	mach_timebase_info_data_t tb;

	mach_timebase_info(&tb);
	say("audiodump: node %p at mach %llu (%.3f s), %u ivars\n", node, (unsigned long long)mach_absolute_time(),
	    mach_absolute_time() * (double)tb.numer / tb.denom / 1e9, n);
	for (unsigned i = 0; i < n; i++) {
		const char *name = ivar_getName(all[i]), *type = ivar_getTypeEncoding(all[i]);
		const uint8_t *p = (const uint8_t *)node + ivar_getOffset(all[i]);
		char value[120] = "";

		switch (type[0]) {
		case 'c': case 'C': case 'B': snprintf(value, sizeof(value), "%u", *p); break;
		case 'i': case 'I': snprintf(value, sizeof(value), "%d", *(const int32_t *)p); break;
		case 'q': case 'Q': case 'l': case 'L':
			snprintf(value, sizeof(value), "%lld (%.3f s as host)", *(const long long *)p,
			    *(const long long *)p * (double)tb.numer / tb.denom / 1e9);
			break;
		case 'd': snprintf(value, sizeof(value), "%f", *(const double *)p); break;
		case 'f': snprintf(value, sizeof(value), "%f", *(const float *)p); break;
		case '@': case '^': snprintf(value, sizeof(value), "%p", *(void *const *)p); break;
		case '{':
			if (strncmp(type, "{?=qiIq}", 8) == 0 || strncmp(type, "{CMTime=qiIq}", 13) == 0) {
				int64_t v = *(const int64_t *)p;
				int32_t ts = *(const int32_t *)(p + 8);
				uint32_t fl = *(const uint32_t *)(p + 12);
				snprintf(value, sizeof(value), "CMTime %lld/%d flags %u (%.3f s)", (long long)v, ts, fl,
				    ts ? (double)v / ts : 0.0);
			} else {
				snprintf(value, sizeof(value), "%02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2], p[3], p[4], p[5],
				    p[6], p[7]);
			}
			break;
		}
		say("audiodump:   +%-5td %-50s %-24.24s %s\n", ivar_getOffset(all[i]), name, type, value);
	}
}

static void
later(void *node)
{
	dump(node);
}

static BOOL
my_start(id self, SEL cmd, id *error)
{
	BOOL ok = orig_start(self, cmd, error);
	void *node = (void *)self;
	dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);

	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), q, node, later);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), q, node, later);
	return ok;
}

static int tries;

static void
hook(void *unused)
{
	Class c = objc_getClass("BWAudioSourceNode");
	Method m;

	(void)unused;
	if (c == Nil) {
		if (++tries < 120) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
			    NULL, hook);
		}
		return;
	}
	m = class_getInstanceMethod(c, sel_registerName("start:"));
	if (m != NULL) {
		orig_start = (BOOL (*)(id, SEL, id *))method_setImplementation(m, (IMP)my_start);
		say("audiodump: BWAudioSourceNode -start: wrapped\n");
	}
}

__attribute__((constructor)) static void
start(void)
{
	console = open("/dev/console", O_WRONLY);
	hook(NULL);
}
