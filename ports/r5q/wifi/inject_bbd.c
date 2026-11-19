/*
 * libr5q-bbd.dylib: injected into backboardd (r5q).
 *
 * The 5 s ALS wait (Test 168). While starting the window server, backboardd's BrightnessSystem calls CoreBrightness's
 * CBU_ShouldWaitForALS(), which waits on a semaphore for a HID service of usage page 0xff00, usage 4 (Apple's ambient
 * light sensor) for at most dispatch_time(0, 5 s), then logs "timeout waiting for ALS arrival" and goes on. This phone
 * has no such service under iOS, so every boot paid 5.19 s there ("window server ready in 5.19142s"). The function is
 * not exported and is not called through anything interposable, so its timeout is patched in place: the three
 * instructions that make 5 000 000 000 ns in x1 become "mov x1, #0", and the wait returns at once with the same timeout
 * path as before. Each word is checked against what iOS 16.5 (20F66, iPhone10,3 cache) has there first; on any other
 * cache nothing is written.
 */
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_UNSLID_BASE       0x180000000ull
#define ALS_TIMEOUT_AT          0x1af0438e0ull  /* CoreBrightness+0xc98e0, in CBU_ShouldWaitForALS */

static void
say(const char *text)
{
	write(2, text, strlen(text));
}

static void
no_als_wait(void)
{
	static const uint32_t expected[3] = { 0xd29e4001, 0xf2a540a1, 0xf2c00021 };    /* mov x1,#0xf200; movk; movk */
	static const uint32_t patched[3] = { 0xd2800001, 0xd503201f, 0xd503201f };     /* mov x1,#0; nop; nop */
	size_t length = 0;
	uintptr_t start = (uintptr_t)_dyld_get_shared_cache_range(&length), slide;
	volatile uint32_t *code;
	vm_address_t page;
	char line[200];

	if (start == 0) {
		return;
	}
	slide = start - CACHE_UNSLID_BASE;
	code = (volatile uint32_t *)(ALS_TIMEOUT_AT + slide);
	for (unsigned i = 0; i < 3; i++) {
		if (code[i] != expected[i]) {
			snprintf(line, sizeof(line), "r5q-bbd: the ALS wait: found %#x where %#x was expected; left alone\n",
			    code[i], expected[i]);
			say(line);
			return;
		}
	}
	page = (vm_address_t)code & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		say("r5q-bbd: the ALS wait: cannot make the page writable; left alone\n");
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		code[i] = patched[i];
	}
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate((void *)code, sizeof(patched));
	say("r5q-bbd: CoreBrightness's 5 s wait for an ambient light sensor this phone does not have: made 0\n");
}

/*
 * (r5q, Test 223) The phone's motion sensors to iOS. The kernel streams the SLPI's sensors (redwood_adsp.c: Qualcomm SEE
 * over QRTR, its registry served through FastRPC) and keeps the last samples in the sysctl kern.redwood.motion. Here, in
 * backboardd -- the HID event server -- an accelerometer service of ours (Apple's vendor page 0xff00, usage 3, as the
 * iPhone's own) dispatches them as accelerometer events: backboardd's orientation manager turns them into the device
 * orientation (auto-rotate), and every other HID client (locationd's CoreMotion) sees the service too. Android's axes
 * match iOS's (x right, y up the screen, z out of it) but Android reports the reaction to gravity in m/s^2 (+9.8 on z
 * lying face up) and iOS gravity itself in g (-1): so iOS = -Android / 9.80665.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/sysctl.h>

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching,
	    uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateAccelerometerEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);
extern IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef allocator, uint64_t time, int32_t level, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateCompassEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateGyroEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);

#define SEE_KINDS               5
struct redwood_motion {                 /* as redwood_adsp.c */
	uint32_t version, seq;
	uint32_t count[SEE_KINDS];
	int32_t accel[3], gyro[3], mag[3];
	int32_t lux;
	int32_t prox[2];
	uint64_t time[SEE_KINDS];
};

struct sensor_service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
	const char *product;                    /* (Test 228) for the log */
};

static CFTypeRef
sensor_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct sensor_service *)refcon)->properties, key);

	return value ? CFRetain(value) : NULL;
}

static Boolean
sensor_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value)
{
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(value)) {
		((struct sensor_service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		char name[64] = "?", line[160];
		int n = 0;

		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		CFNumberGetValue(value, kCFNumberIntType, &n);
		snprintf(line, sizeof(line), "r5q-bbd: sensors: the event server sets %s = %d on %s\n", name, n,
		    ((struct sensor_service *)refcon)->product);
		say(line);
	}
	return true;
}

static void
sensor_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, __unused uint32_t type,
    __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks sensor_callbacks = {
	.version = 2, .notification = sensor_notification, .setProperty = sensor_set_property, .copyProperty = sensor_copy_property,
};

static CFNumberRef
cf_int(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* a built-in service of ours with one usage, made again until the event server has opened it (as the donor's make_service) */
static IOHIDServiceClientRef
sensor_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage)
{
	struct sensor_service *made = calloc(1, sizeof(*made));
	CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef page_n = cf_int(page), usage_n = cf_int(usage), zero = cf_int(0);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFArrayRef pairs;
	IOHIDServiceClientRef service = NULL;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_n);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_n);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(p, CFSTR("PrimaryUsagePage"), page_n);
	CFDictionarySetValue(p, CFSTR("PrimaryUsage"), usage_n);
	CFDictionarySetValue(p, CFSTR("DeviceUsagePage"), page_n);
	CFDictionarySetValue(p, CFSTR("DeviceUsage"), usage_n);
	CFDictionarySetValue(p, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(p, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(p, CFSTR("Transport"), CFSTR("SPU"));
	CFDictionarySetValue(p, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(p, CFSTR("Product"), name);
	CFDictionarySetValue(p, CFSTR("VendorID"), zero);
	CFDictionarySetValue(p, CFSTR("ProductID"), zero);
	made->properties = p;
	made->product = product;
	/*
	 * (Test 234) Kept even when nobody has opened it yet: with CoreMotion fusing device motion itself (Test 233's
	 * capability patch), backboardd's own CoreMotion no longer opens the accelerometer at once, and giving up after
	 * 20 tries left the phone with no sensor services at all. A few tries (a service the server did not see), then
	 * the last one is kept and opened by whoever asks for it.
	 */
	for (int attempt = 1; attempt <= 3; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, p, &sensor_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened || attempt == 3) {
			if (!made->opened) {
				char line[160];

				snprintf(line, sizeof(line), "r5q-bbd: sensors: %s: not opened yet; kept\n", product);
				say(line);
			}
			break;
		}
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/*
 * (Test 224) backboardd takes the device orientation from CoreMotion (-[CMDeviceOrientationManager
 * startDeviceOrientationUpdatesToQueue:withHandler:], "Updating device orientation from CoreMotion"), whose orientation
 * comes from the motion coprocessor's own HID device -- none here ("[Orientation] fHidDeviceInterface NULL"). So that call
 * is taken over: backboardd's queue and handler kept (CoreMotion's own start still made), and the orientation, worked out
 * here from the accelerometer, handed to the handler as a CMDeviceOrientation of CoreMotion's own class whenever it changes.
 */
#include <Block.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>

typedef void (^orientation_handler_t)(id orientation, id error);
static dispatch_queue_t orientation_queue;
static orientation_handler_t orientation_handler;
static id orientation_object;           /* (Test 225) CoreMotion's own, kept */
static id orientation_manager;          /* (Test 226) backboardd's CMDeviceOrientationManager */

/* (Test 226) a CoreMotion orientation object of CoreMotion's own making: its manager's deviceOrientationBlocking, if that
 * returns an object (checked from the method's type first) */
static void
orientation_object_get(void)
{
	SEL sel = sel_registerName("deviceOrientationBlocking");
	Method m;
	const char *types;
	char line[160];
	id got;

	if (orientation_object != NULL || orientation_manager == NULL) {
		return;
	}
	m = class_getInstanceMethod(object_getClass(orientation_manager), sel);
	types = m ? method_getTypeEncoding(m) : NULL;
	snprintf(line, sizeof(line), "r5q-bbd: orientation: deviceOrientationBlocking is %s\n", types ? types : "not there");
	say(line);
	if (types == NULL || types[0] != '@') {
		return;
	}
	got = ((id (*)(id, SEL))objc_msgSend)(orientation_manager, sel);
	if (got != NULL && object_getClass(got) == objc_getClass("CMDeviceOrientation")) {
		orientation_object = (id)CFRetain((CFTypeRef)got);
		say("r5q-bbd: orientation: CoreMotion's own orientation object taken from deviceOrientationBlocking\n");
	} else {
		snprintf(line, sizeof(line), "r5q-bbd: orientation: deviceOrientationBlocking gave %p (%s)\n", (void *)got,
		    got ? class_getName(object_getClass(got)) : "nil");
		say(line);
	}
}
static void (*original_start)(id, SEL, id, id);
static ptrdiff_t orientation_ivar = -1;
static char orientation_ivar_type;

static void
start_updates(id self, SEL cmd, id queue, orientation_handler_t handler)
{
	char line[160];
	orientation_handler_t wrapper = handler;

	orientation_manager = self;

	if (queue != NULL && handler != NULL) {
		orientation_handler_t kept = Block_copy(handler);

		orientation_queue = (dispatch_queue_t)queue;
		CFRetain((CFTypeRef)queue);
		orientation_handler = kept;
		/*
		 * (Test 225) CoreMotion's own orientation objects are what is handed on: the first one it delivers is kept, and
		 * later only its orientation field is changed (Test 224's object of our own making crashed backboardd).
		 */
		wrapper = Block_copy(^(id orientation, id error) {
			if (orientation != NULL && orientation_object == NULL) {
				orientation_object = (id)CFRetain((CFTypeRef)orientation);
				say("r5q-bbd: orientation: CoreMotion's own orientation object kept\n");
			}
			kept(orientation, error);
		});
	}
	snprintf(line, sizeof(line), "r5q-bbd: orientation: backboardd asks CoreMotion for orientation updates (queue %p): ours too\n",
	    (void *)queue);
	say(line);
	original_start(self, cmd, queue, (id)(void *)wrapper);
}

static void
orientation_hook(void)
{
	Class manager = objc_getClass("CMDeviceOrientationManager"), value = objc_getClass("CMDeviceOrientation");
	Method m = manager ? class_getInstanceMethod(manager, sel_registerName("startDeviceOrientationUpdatesToQueue:withHandler:")) : NULL;
	unsigned count = 0;
	Ivar *ivars = value ? class_copyIvarList(value, &count) : NULL;
	char line[200];

	for (unsigned i = 0; i < count; i++) {
		const char *name = ivar_getName(ivars[i]), *type = ivar_getTypeEncoding(ivars[i]);

		snprintf(line, sizeof(line), "r5q-bbd: orientation: CMDeviceOrientation ivar %s (%s) at %td\n", name, type,
		    ivar_getOffset(ivars[i]));
		say(line);
		if (strstr(name, "rientation") != NULL && type != NULL && strchr("qilQIL", type[0]) != NULL && orientation_ivar < 0) {
			orientation_ivar = ivar_getOffset(ivars[i]);
			orientation_ivar_type = type[0];
		}
	}
	free(ivars);
	if (m == NULL || orientation_ivar < 0) {
		say("r5q-bbd: orientation: CoreMotion's orientation manager or value not as expected: left alone\n");
		return;
	}
	original_start = (void (*)(id, SEL, id, id))method_setImplementation(m, (IMP)start_updates);
	say("r5q-bbd: orientation: CoreMotion's orientation updates taken over\n");
}

/* UIDeviceOrientation from an accelerometer sample in g (iOS's axes) */
static long
orientation_of(double x, double y, double z)
{
	if (z < -0.8) {
		return 5;               /* face up */
	}
	if (z > 0.8) {
		return 6;               /* face down */
	}
	if (fabs(y) >= fabs(x) + 0.2) {
		return y < 0 ? 1 : 2;   /* portrait / upside down */
	}
	if (fabs(x) >= fabs(y) + 0.2) {
		return x < 0 ? 3 : 4;   /* landscape left (home side right) / right */
	}
	return 0;
}

static void
orientation_send(long orientation)
{
	char line[120];

	orientation_object_get();
	if (orientation_handler == NULL || orientation_queue == NULL || orientation_object == NULL) {
		return;
	}
	snprintf(line, sizeof(line), "r5q-bbd: orientation: now %ld\n", orientation);
	say(line);
	/*
	 * (Test 227) on backboardd's own queue -- an NSOperationQueue, as CoreMotion's ...ToQueue:withHandler: take (Test 226's
	 * dispatch_async onto it crashed in libdispatch): CoreMotion's object, its orientation field set, handed on
	 */
	void (^work)(void) = Block_copy(^{
		if (orientation_ivar_type == 'q' || orientation_ivar_type == 'l' || orientation_ivar_type == 'Q' || orientation_ivar_type == 'L') {
			*(long *)((char *)orientation_object + orientation_ivar) = orientation;
		} else {
			*(int *)((char *)orientation_object + orientation_ivar) = (int)orientation;
		}
		orientation_handler(orientation_object, NULL);
	});
	((void (*)(id, SEL, id))objc_msgSend)((id)orientation_queue, sel_registerName("addOperationWithBlock:"), (id)(void *)work);
}

/*
 * (Test 232) Auto-brightness from the phone's own light sensor. A light service of ours makes CoreBrightness crash (Test
 * 228: it builds a model of the sensor from calibration properties a real iPhone ALS publishes, and dereferences it), so
 * the brightness is set here instead, through backboardd's own BKSDisplayBrightnessSet: the lux smoothed (log scale),
 * mapped to a level (0.08 dark, ~0.55 at 100 lux, ~0.77 at 1000, 1.0 at 10000). A change of the brightness by the user
 * (the slider) is respected: no more automatic changes until the light changes about fourfold. Off when
 * com.apple.backboardd's BKEnableALS is set false.
 *
 * (Test 241) Smooth and settling (the owner: "not smooth and doesn't seem to settle" -- 0.05 jumps once a second, a new
 * target at every flicker of the reading): run every 40 ms; the light smoothed over ~1.5 s (log scale); the goal moved
 * only when the light calls for a level more than 0.05 away (a dead band: no hunting); the screen eased towards the
 * goal -- 6% of the way each step, at most 0.006 (0.15 a second) -- and left alone once within 0.002.
 */
#include <dlfcn.h>

static float (*brightness_get)(void);
static void (*brightness_set)(float level, int options);

static void
auto_brightness(const struct redwood_motion *m)
{
	static bool ready, override;
	static double smooth = -1, override_at, goal = -1, level = -1;
	static float last_set = -1;
	static unsigned changes, ticks;
	double target, step;
	char line[160];

	if (!ready) {
		void *bbs = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);

		ready = true;
		brightness_get = bbs ? (float (*)(void))dlsym(bbs, "BKSDisplayBrightnessGetCurrent") : NULL;
		brightness_set = bbs ? (void (*)(float, int))dlsym(bbs, "BKSDisplayBrightnessSet") : NULL;
		snprintf(line, sizeof(line), "r5q-bbd: brightness: automatic, from the light sensor: %s\n",
		    brightness_get && brightness_set ? "on" : "NOT POSSIBLE (no BackBoardServices calls)");
		say(line);
	}
	if (brightness_get == NULL || brightness_set == NULL || m->count[3] == 0) {
		return;
	}
	{
		Boolean valid = false, enabled = CFPreferencesGetAppBooleanValue(CFSTR("BKEnableALS"), CFSTR("com.apple.backboardd"), &valid);

		if (valid && !enabled) {
			return;
		}
	}
	smooth = smooth < 0 ? log10(m->lux / 1000.0 + 1) : 0.973 * smooth + 0.027 * log10(m->lux / 1000.0 + 1);
	target = 0.08 + 0.23 * smooth;
	target = target < 0.05 ? 0.05 : target > 1.0 ? 1.0 : target;
	if (ticks++ % 25 == 0) {                /* once a second: has the user moved the slider? */
		float now = brightness_get();

		if (last_set >= 0 && fabsf(now - last_set) > 0.04f && !override) {
			override = true;
			override_at = smooth;
			say("r5q-bbd: brightness: set by hand: automatic changes wait for a change of light\n");
		}
		if (override) {
			if (fabs(smooth - override_at) < 0.6) {
				last_set = now;
				return;
			}
			override = false;
			level = now;
			goal = -1;
			say("r5q-bbd: brightness: the light has changed: automatic again\n");
		}
		if (level < 0) {
			level = now;
		}
	}
	if (override || level < 0) {
		return;
	}
	if (goal < 0 || fabs(target - goal) > 0.05) {
		goal = target;
		if (changes++ < 20 || changes % 100 == 0) {
			snprintf(line, sizeof(line), "r5q-bbd: brightness: %d lux: towards %.2f (now %.2f)\n", m->lux / 1000, goal, level);
			say(line);
		}
	}
	if (fabs(goal - level) < 0.002) {
		return;                         /* settled */
	}
	step = (goal - level) * 0.06;
	if (fabs(step) < 0.0015) {
		step = goal > level ? 0.0015 : -0.0015;
	}
	step = step > 0.006 ? 0.006 : step < -0.006 ? -0.006 : step;
	level = fabs(goal - level) <= fabs(step) ? goal : level + step;
	brightness_set((float)level, 1);
	last_set = (float)level;
}

/*
 * (Test 243) The magnetometer hard-iron calibrated here, as Android does in the sensors DSP (sns_mag_cal), before iOS
 * sees it. The raw field carries the phone's own magnets: owner's figure-8 recording (wifi/magrec.c, scratchpad
 * magfit.py) fits a sphere of radius 50.5 uT (the Earth's field: units right) around (-95.1, 39.0, -425.8) uT, axes
 * as the accelerometer's (no swap). iOS's own calibrator never came out of "uncalibrated" with the raw field. Start
 * from that offset; refine it as the phone is turned (a sphere fit over up to 48 samples spread at least 12 uT apart,
 * taken when the fit is round enough: radius 25..70 uT, residual under 3 uT).
 */
#define MAGCAL_SAMPLES 48

static double magcal_offset[3] = { -95.1, 39.0, -425.8 };

static bool
solve4(double a[4][5], double x[4])
{
	for (int c = 0; c < 4; c++) {
		int p = c;

		for (int r = c + 1; r < 4; r++) {
			if (fabs(a[r][c]) > fabs(a[p][c])) {
				p = r;
			}
		}
		if (fabs(a[p][c]) < 1e-9) {
			return false;
		}
		for (int k = 0; k < 5; k++) {
			double t = a[c][k];

			a[c][k] = a[p][k];
			a[p][k] = t;
		}
		for (int r = 0; r < 4; r++) {
			if (r != c) {
				double f = a[r][c] / a[c][c];

				for (int k = c; k < 5; k++) {
					a[r][k] -= f * a[c][k];
				}
			}
		}
	}
	for (int c = 0; c < 4; c++) {
		x[c] = a[c][4] / a[c][c];
	}
	return true;
}

static void
magcal_feed(const double raw[3])
{
	static double kept[MAGCAL_SAMPLES][3];
	static unsigned n, next, fresh, fits;
	double a[4][5] = { { 0 } }, x[4], r, res = 0;
	char line[200];

	for (unsigned i = 0; i < n; i++) {
		double d = 0;

		for (int k = 0; k < 3; k++) {
			d += (raw[k] - kept[i][k]) * (raw[k] - kept[i][k]);
		}
		if (d < 12.0 * 12.0) {
			return;                         /* too near one already kept */
		}
	}
	memcpy(kept[next], raw, sizeof(kept[0]));
	next = (next + 1) % MAGCAL_SAMPLES;
	n = n < MAGCAL_SAMPLES ? n + 1 : n;
	if (++fresh < 8 || n < 16) {
		return;
	}
	fresh = 0;
	for (unsigned i = 0; i < n; i++) {
		double row[5] = { 2 * kept[i][0], 2 * kept[i][1], 2 * kept[i][2], 1,
			kept[i][0] * kept[i][0] + kept[i][1] * kept[i][1] + kept[i][2] * kept[i][2] };

		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 5; k++) {
				a[j][k] += row[j] * row[k];
			}
		}
	}
	if (!solve4(a, x)) {
		return;
	}
	r = sqrt(x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
	for (unsigned i = 0; i < n; i++) {
		double d = sqrt((kept[i][0] - x[0]) * (kept[i][0] - x[0]) + (kept[i][1] - x[1]) * (kept[i][1] - x[1]) +
		    (kept[i][2] - x[2]) * (kept[i][2] - x[2])) - r;

		res += d * d;
	}
	res = sqrt(res / n);
	if (r >= 25 && r <= 70 && res < 3) {
		for (int k = 0; k < 3; k++) {
			magcal_offset[k] = 0.7 * magcal_offset[k] + 0.3 * x[k];
		}
	}
	if (fits++ < 10 || fits % 50 == 0) {
		snprintf(line, sizeof(line), "r5q-bbd: compass: fit of %u samples: centre %.1f %.1f %.1f, radius %.1f uT, residual %.1f: %s\n",
		    n, x[0], x[1], x[2], r, res, r >= 25 && r <= 70 && res < 3 ? "taken" : "not taken");
		say(line);
	}
}

static void *
sensors_thread(__unused void *arg)
{
	struct redwood_motion m;
	size_t size = sizeof(m);
	IOHIDEventSystemClientRef client;
	IOHIDServiceClientRef accel, light, compass, gyro;
	uint32_t last = 0, sent = 0, steady = 0, last_light = 0, last_mag = 0, lights = 0, last_gyro = 0;
	long shown = -1, seen = -1;
	char line[200];

	pthread_setname_np("r5q sensors");
	for (int waited = 0; waited < 120; waited++) {          /* the SLPI is brought up beside the system's start */
		size = sizeof(m);
		if (sysctlbyname("kern.redwood.motion", &m, &size, NULL, 0) == 0 && m.count[0] > 0) {
			break;
		}
		sleep(2);
	}
	if (sysctlbyname("kern.redwood.motion", &m, &size, NULL, 0) != 0 || m.count[0] == 0) {
		say("r5q-bbd: sensors: no accelerometer samples from the kernel (kern.redwood.motion): no service\n");
		return NULL;
	}
	client = IOHIDEventSystemClientCreateWithType(NULL, 4, NULL);      /* a client that only sends */
	if (client == NULL) {
		say("r5q-bbd: sensors: no HID client\n");
		return NULL;
	}
	IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("r5q.sensors", NULL));
	accel = sensor_service(client, "Redwood accelerometer", 0xff00, 3);
	snprintf(line, sizeof(line), "r5q-bbd: sensors: accelerometer service %p (the kernel has %u samples)\n", (void *)accel, m.count[0]);
	say(line);
	if (accel == NULL) {
		return NULL;
	}
	/*
	 * (Test 228) the ambient light sensor (Apple's 0xff00/4: CoreBrightness's auto-brightness takes it) and the compass
	 * (0xff00/0xa), as the iPhone's own; light as whole lux, the compass in microtesla on the same axes as Android's
	 */
	/*
	 * NOT MADE for now: with this light service CoreBrightness crashed backboardd at once, every start (Test 228: SIGSEGV
	 * reading 0x1c8 in CoreBrightness -- it wants what a real ALS service publishes, calibration among it, not yet known).
	 * The compass is left out with it until it can be tried on its own. The image in use is Test 227's.
	 */
	light = NULL;
	/* (Test 229) the compass alone */
	compass = sensor_service(client, "Redwood compass", 0xff00, 0x0a);
	/* (Test 230) the gyroscope (0xff00/9), rad/s on the same axes as Android's: CoreMotion's gyro and device motion */
	gyro = sensor_service(client, "Redwood gyroscope", 0xff00, 0x09);
	snprintf(line, sizeof(line), "r5q-bbd: sensors: ambient light service %p, compass service %p\n", (void *)light, (void *)compass);
	say(line);
	/*
	 * (Test 348) Every 5 ms, not 40: the kernel now gets ~100 accelerometer and gyroscope samples a second and 50 from
	 * the compass (Test 347, see_rate), but a 40 ms poll that sends only the newest sample capped what iOS saw at 25 a
	 * second (cm_rate: 23.2/s in an app), too few for CoreMotion's fusion and the compass. Auto-brightness keeps its
	 * 40 ms step (every eighth round).
	 */
	for (unsigned round = 0;; round++) {
		usleep(5000);
		size = sizeof(m);
		if (sysctlbyname("kern.redwood.motion", &m, &size, NULL, 0) != 0) {
			continue;
		}
		if (round % 8 == 0) {
			auto_brightness(&m);            /* (Test 241) every 40 ms: eased, see there */
		}
		if (light != NULL && m.count[3] != last_light) {
			IOHIDEventRef event = IOHIDEventCreateAmbientLightSensorEvent(NULL, mach_absolute_time(), (m.lux + 500) / 1000, 0);

			last_light = m.count[3];
			if (event != NULL) {
				IOHIDEventSetIntegerValue(event, 0x4, 1);
				IOHIDVirtualServiceClientDispatchEvent(light, event);
				CFRelease(event);
			}
			if (lights++ < 5) {
				snprintf(line, sizeof(line), "r5q-bbd: sensors: ambient light %d lux\n", (m.lux + 500) / 1000);
				say(line);
			}
		}
		if (compass != NULL && m.count[2] != last_mag) {
			double raw[3] = { m.mag[0] / 1000.0, m.mag[1] / 1000.0, m.mag[2] / 1000.0 };
			IOHIDEventRef event;

			magcal_feed(raw);                       /* (Test 243) hard iron off, see magcal_feed */
			event = IOHIDEventCreateCompassEvent(NULL, mach_absolute_time(), raw[0] - magcal_offset[0],
			    raw[1] - magcal_offset[1], raw[2] - magcal_offset[2], 0);

			last_mag = m.count[2];
			if (event != NULL) {
				IOHIDEventSetIntegerValue(event, 0x4, 1);
				IOHIDVirtualServiceClientDispatchEvent(compass, event);
				CFRelease(event);
			}
		}
		if (gyro != NULL && m.count[1] != last_gyro) {
			IOHIDEventRef event = IOHIDEventCreateGyroEvent(NULL, mach_absolute_time(), m.gyro[0] / 1000.0, m.gyro[1] / 1000.0,
			    m.gyro[2] / 1000.0, 0);

			last_gyro = m.count[1];
			if (event != NULL) {
				IOHIDEventSetIntegerValue(event, 0x4, 1);
				IOHIDVirtualServiceClientDispatchEvent(gyro, event);
				CFRelease(event);
			}
		}
		if (m.count[0] == last) {
			continue;
		}
		last = m.count[0];
		{
			double x = -m.accel[0] / 9806.65, y = -m.accel[1] / 9806.65, z = -m.accel[2] / 9806.65;
			IOHIDEventRef event = IOHIDEventCreateAccelerometerEvent(NULL, mach_absolute_time(), x, y, z, 0);

			if (event != NULL) {
				IOHIDEventSetIntegerValue(event, 0x4 /* is built-in */, 1);
				IOHIDVirtualServiceClientDispatchEvent(accel, event);
				CFRelease(event);
			}
			{
				/* (Test 224) the orientation, once it has held for 0.2 s (Test 348: 20 samples at ~100 a second) */
				long now = orientation_of(x, y, z);

				steady = now == seen ? steady + 1 : 0;
				seen = now;
				if (steady >= 20 && now != 0 && now != shown) {
					shown = now;
					orientation_send(now);
				}
			}
			if (sent++ < 3 || sent % 6000 == 0) {
				snprintf(line, sizeof(line), "r5q-bbd: sensors: accelerometer event %u: %.3f %.3f %.3f g\n", sent, x, y, z);
				say(line);
			}
		}
	}
	return NULL;
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t thread;

	no_als_wait();
	orientation_hook();
	if (pthread_create(&thread, NULL, sensors_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}
