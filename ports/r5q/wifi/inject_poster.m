/*
 * libr5q-poster.dylib (2026-11-20, Test 399): a Metal device for PosterBoard alone. The lock screen's posters and the
 * wallpapers draw through CoreAnimation's Metal renderer (PosterBoard asks for a kCARendererMetalCommandQueue); with no
 * Metal device on this port they stayed black. Their shaders are AIR bitcode (QuartzCore's default.metallib: 141
 * functions; PhotosUICore 4, PridePoster 8), which the port's Metal driver compiles for the Adreno (RedwoodMetal.bundle
 * + libredwoodair, wifi/METAL-FEASIBILITY.md) -- unlike Maps' VectorKit, which ships Apple-GPU code only.
 *
 * Metal's system-wide discovery stays off (backboardd and SpringBoard keep their own road; a half-working device
 * aborts a process, and an aborting daemon loops). Instead, as the driver's own test does (metal_drv_test.m), this
 * opens the bundle, makes RedwoodMetalDevice on the IOAcceleratorES service and hands it to Metal's MTLAddDevice
 * (iOS 16.5 20F66: unslid 0x181a1875c, found against MTLCreateSystemDefaultDevice at 0x181a34b3c), in this process
 * only, at load, before PosterBoard asks. Each step is said on the console.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <unistd.h>

#define MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define MTL_ADD_DEVICE_UNSLID    0x181a1875cUL
#define BUNDLE                   "/redwood/lib/RedwoodMetal.bundle"

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

/*
 * (Test 399) The wallpapers. WallpaperKit picks a wallpaper's variant by the device's "logical screen class",
 * "%.0fw-%.0fh@%@x~%@" from the screen's points and scale: on this port 375w-812h@2x~iphone (iOS runs at 2x,
 * fbscale=2). Apple ships the iPhone X's variants as 375w-812h@3x~iphone only, so CollectionsPoster logged "No
 * compatible wallpaper found on disk (wallpaperIdentifier: '7400' ...)" and the lock screen stayed black. Every
 * deviceLogicalScreenClass (class or instance method, whichever class has it once WallpaperKit is loaded) is wrapped
 * to answer @3x for @2x; the .ca layers scale themselves.
 */
static IMP orig_class_imp, orig_inst_imp;

static NSString *
three_x(NSString *s)
{
	return [s isKindOfClass:[NSString class]] ? [s stringByReplacingOccurrencesOfString:@"@2x~" withString:@"@3x~"] : s;
}

static id
my_class_screen(id self, SEL cmd)
{
	return three_x(((id (*)(id, SEL))orig_class_imp)(self, cmd));
}

static id
my_inst_screen(id self, SEL cmd)
{
	return three_x(((id (*)(id, SEL))orig_inst_imp)(self, cmd));
}

static int screen_tries;

static void
wrap_screen_class(void *unused)
{
	SEL sel = sel_registerName("deviceLogicalScreenClass");
	int n = objc_getClassList(NULL, 0), wrapped = 0;
	Class *all = (Class *)calloc((size_t)n, sizeof(Class));

	(void)unused;
	n = objc_getClassList(all, n);
	for (int i = 0; i < n; i++) {
		Method m;
		/* (only the class itself, not one that inherits it: class_copyMethodList lists a class's own) */
		unsigned count = 0;
		Method *own = class_copyMethodList(object_getClass((id)all[i]), &count);
		for (unsigned k = 0; k < count && orig_class_imp == NULL; k++) {
			if (method_getName(own[k]) == sel) {
				orig_class_imp = method_setImplementation(own[k], (IMP)my_class_screen);
				say("r5q-poster: +[%s deviceLogicalScreenClass] answers @3x for @2x\n", class_getName(all[i]));
				wrapped++;
			}
		}
		free(own);
		own = class_copyMethodList(all[i], &count);
		for (unsigned k = 0; k < count && orig_inst_imp == NULL; k++) {
			if (method_getName(own[k]) == sel) {
				orig_inst_imp = method_setImplementation(own[k], (IMP)my_inst_screen);
				say("r5q-poster: -[%s deviceLogicalScreenClass] answers @3x for @2x\n", class_getName(all[i]));
				wrapped++;
			}
		}
		free(own);
		(void)m;
	}
	free(all);
	(void)screen_tries;
}

/*
 * WallpaperKit is loaded lazily, and resolves the default wallpaper (and skips the collections it finds no variant in)
 * at once: a wrapper put in half a second later was too late. So the wrapping is done in dyld's add-image callback for
 * WallpaperKit itself, after the runtime has its classes and before any of its code runs.
 */
static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	Dl_info info;

	(void)slide;
	if ((orig_class_imp == NULL && orig_inst_imp == NULL) && dladdr(mh, &info) != 0 && info.dli_fname != NULL &&
	    strstr(info.dli_fname, "/WallpaperKit.framework/") != NULL) {
		wrap_screen_class(NULL);
	}
}

/*
 * That getter was not the source: WKDefaultWallpaperManager keeps the class in an ivar, and the collections are
 * filtered with the same value, both from one internal function of WallpaperKit (+0x11c60, iOS 16.5) that formats
 * "%.0fw-%.0fh@%@x~%@" from [UIScreen mainScreen]'s bounds and -scale. Inside the poster extensions only -- PosterBoard,
 * SpringBoard and the rest stay at 2x -- -[UIScreen scale] answers 3, the iPhone X's own, so the @3x variants Apple
 * ships match; the poster's layers render at 3x and are scaled to the 2x display.
 */
static IMP orig_scale;

static CGFloat
my_scale(id self, SEL cmd)
{
	CGFloat s = ((CGFloat (*)(id, SEL))orig_scale)(self, cmd);
	return s == 2.0 ? 3.0 : s;
}

static void
three_x_screen(void)
{
	const char *me = getprogname();

	if (strcmp(me, "PosterBoard") == 0 || strcmp(me, "SpringBoard") == 0) {
		return;
	}
	Method m = class_getInstanceMethod(objc_getClass("UIScreen"), sel_registerName("scale"));
	if (m != NULL && orig_scale == NULL) {
		orig_scale = method_setImplementation(m, (IMP)my_scale);
		say("r5q-poster: %s: -[UIScreen scale] answers 3 for 2 (wallpaper variants)\n", me);
	}
}

__attribute__((constructor)) static void
start(void)
{
	@autoreleasepool {
		console = open("/dev/console", O_WRONLY);
		three_x_screen();
		_dyld_register_func_for_add_image(image_added);   /* (also called at once for the images already there) */
		NSBundle *bundle = [NSBundle bundleWithPath:@BUNDLE];
		Class cls = bundle != nil ? [bundle classNamed:@"RedwoodMetalDevice"] : Nil;
		if (cls == Nil) {
			cls = [bundle principalClass];
		}
		if (cls == Nil) {
			say("r5q-poster: no RedwoodMetalDevice in %s: no Metal device\n", BUNDLE);
			return;
		}
		io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
		if (svc == IO_OBJECT_NULL) {
			say("r5q-poster: no IOAcceleratorES service: no Metal device\n");
			return;
		}
		id dev = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc], sel_getUid("initWithAcceleratorPort:"), svc);
		if (dev == nil) {
			say("r5q-poster: RedwoodMetalDevice would not start on the GPU: no Metal device\n");
			return;
		}
		/*
		 * Metal's device list (_deviceArray, made with its "com.Metal.DeviceArray" queue by a one-time initializer at
		 * Metal+0x1f42c) must exist: MTLAddDevice asserts "_deviceArray != NULL". PosterBoard had it from its own first
		 * Metal call; the poster extensions load this before anything of theirs asks. A first
		 * MTLCreateSystemDefaultDevice() (nil: no device yet) makes it -- seen working in CollectionsPoster; Metal's
		 * MTLCopyAllDevices is not exported on iOS. If the list is still not there, nothing is added (no abort).
		 */
		(void)MTLCreateSystemDefaultDevice();
		uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - MTL_CREATE_DEVICE_UNSLID;
		const void *const *device_array = (const void *const *)(slide + 0x1e1efe9a0UL);   /* (_deviceArray, 20F66) */
		if (*device_array == NULL) {
			say("r5q-poster: Metal's device list was not made: no Metal device for %s\n", getprogname());
			return;
		}
		void (*add_device)(id) = (void (*)(id))(slide + MTL_ADD_DEVICE_UNSLID);
		@try {
			add_device(dev);
		} @catch (NSException *e) {
			say("r5q-poster: MTLAddDevice threw %s\n", [[e reason] UTF8String]);
			return;
		}
		id<MTLDevice> now = MTLCreateSystemDefaultDevice();
		say("r5q-poster: Metal device for %s: %s (%s)\n", getprogname(), now != nil ? [[now name] UTF8String] : "none",
		    now == dev ? "ours" : "not ours");
	}
}
