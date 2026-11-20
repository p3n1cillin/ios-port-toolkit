/*
 * libr5q-media.dylib (2026-11-20, Test 395): into mediaserverd. CMCapture's audio source node (BWAudioSourceNode)
 * fills a gap in the audio it receives with silence: the host time elapsed since the node started, less the sample
 * time elapsed, made into frames (CMCapture+0xa900..0xab3c, iOS 16.5). On this port those two disagree by a constant
 * 468.7 s, so every 941-frame callback came with 20.67 million frames of silence -- a 39 MB CMBlockBuffer some 40 times
 * a second, held until the Camera's session ended: mediaserverd at 2.7-3.7 GB, killed by jetsam making a photo
 * (wifi/MEMORY-PLAN.md). The node has a switch of its own for this, the ivar _silenceFramesFillingDisabled (another
 * init path logs "Audio source node created with silence frame filling disbaled"); this sets it on every
 * BWAudioSourceNode before its audio flows, by wrapping the node's -start: (or, without one,
 * -prepareForCurrentConfigurationToBecomeLive). Said on the console: the hook, and each node it is set on.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int console = -1;
static ptrdiff_t disabled_at = -1;
static BOOL (*orig_start)(id, SEL, id *);
static void (*orig_live)(id, SEL);
static int tries;

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
no_silence(id node)
{
	if (node != nil && disabled_at >= 0) {
		uint8_t *flag = (uint8_t *)node + disabled_at;
		if (*flag == 0) {
			*flag = 1;
			say("r5q-media: silence frame filling off for audio source node %p\n", node);
		}
	}
}

static BOOL
my_start(id self, SEL cmd, id *error)
{
	no_silence(self);
	return orig_start(self, cmd, error);
}

static void
my_live(id self, SEL cmd)
{
	no_silence(self);
	orig_live(self, cmd);
}

static void
hook(void *unused)
{
	Class c = objc_getClass("BWAudioSourceNode");
	(void)unused;

	if (c == Nil) {
		if (++tries < 120) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
			    NULL, hook);
		} else {
			say("r5q-media: no BWAudioSourceNode in 2 minutes: nothing done\n");
		}
		return;
	}
	Ivar v = class_getInstanceVariable(c, "_silenceFramesFillingDisabled");
	if (v == NULL) {
		say("r5q-media: BWAudioSourceNode has no _silenceFramesFillingDisabled: nothing done\n");
		return;
	}
	disabled_at = ivar_getOffset(v);
	Method m = class_getInstanceMethod(c, sel_registerName("start:"));
	if (m != NULL && strcmp(method_getTypeEncoding(m), "B24@0:8^@16") == 0) {
		orig_start = (BOOL (*)(id, SEL, id *))method_setImplementation(m, (IMP)my_start);
		say("r5q-media: BWAudioSourceNode -start: wrapped (ivar at %td)\n", disabled_at);
		return;
	}
	m = class_getInstanceMethod(c, sel_registerName("prepareForCurrentConfigurationToBecomeLive"));
	if (m != NULL && strcmp(method_getTypeEncoding(m), "v16@0:8") == 0) {
		orig_live = (void (*)(id, SEL))method_setImplementation(m, (IMP)my_live);
		say("r5q-media: BWAudioSourceNode -prepareForCurrentConfigurationToBecomeLive wrapped (ivar at %td)\n", disabled_at);
		return;
	}
	/* neither: say what there is, for the next try */
	unsigned n = 0;
	Method *all = class_copyMethodList(c, &n);
	say("r5q-media: BWAudioSourceNode has neither -start: nor -prepareForCurrentConfigurationToBecomeLive; its %u methods:\n", n);
	for (unsigned i = 0; i < n; i++) {
		say("r5q-media:   %s %s\n", sel_getName(method_getName(all[i])), method_getTypeEncoding(all[i]));
	}
}

__attribute__((constructor)) static void
start(void)
{
	console = open("/dev/console", O_WRONLY);
	hook(NULL);
}
