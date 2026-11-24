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
	struct {                        /* (Test 349, version 2) the last 16 samples of accelerometer, gyroscope, compass */
		int32_t v[3];
		uint32_t n;
		uint64_t time;
	} ring[3][16];
	int32_t mag_cal_bias[3];        /* (Test 434, version 3) SLPI sns_mag_cal bias (uT * 1000) & status */
	uint32_t mag_cal_status;
	uint32_t mag_cal_count;
};

/*
 * (Test 349) The samples of sensor `kind' that came since *last: from the kernel's ring (version 2), each with its own
 * time; at most 16 (older ones are gone); one whose entry was being written as we read it is left out. With an older
 * kernel (no ring) the newest one alone, as before. Returns how many were put in v/t.
 */
static unsigned
motion_new(const struct redwood_motion *m, int kind, uint32_t *last, int32_t v[16][3], uint64_t t[16])
{
	uint32_t count = m->count[kind], from = *last + 1, got = 0;

	if (count == *last) {
		return 0;
	}
	if (m->version < 2) {
		const int32_t *now = kind == 0 ? m->accel : kind == 1 ? m->gyro : m->mag;

		memcpy(v[0], now, sizeof(v[0]));
		t[0] = m->time[kind];
		*last = count;
		return 1;
	}
	if (count - from >= 16) {
		from = count - 15;
	}
	for (uint32_t n = from; n != count + 1; n++) {
		if (m->ring[kind][n % 16].n == n) {
			memcpy(v[got], m->ring[kind][n % 16].v, sizeof(v[0]));
			t[got] = m->ring[kind][n % 16].time;
			got++;
		}
	}
	*last = count;
	return got;
}

struct sensor_service {
	CFMutableDictionaryRef properties;      /* (2026-11-21) under lock: the event server's thread sets, clients copy */
	pthread_mutex_t lock;
	volatile bool opened;
	const char *product;                    /* (Test 228) for the log */
};

static CFTypeRef
sensor_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	struct sensor_service *s = refcon;
	CFTypeRef value;

	pthread_mutex_lock(&s->lock);
	value = CFDictionaryGetValue(s->properties, key);
	value = value ? CFRetain(value) : NULL;
	pthread_mutex_unlock(&s->lock);
	return value;
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
	/*
	 * (2026-11-21) Kept, so a client that reads a property back finds what it set. CoreMotion's CLIoHidInterface
	 * (locationd and every app's CoreMotion) sets ReportInterval, BatchInterval and the like on each motion service,
	 * then reads them back: with nothing kept it logged "Service for device with usage pair {65280, 10} is not ready"
	 * and "Could not set all properties after too many retries. Giving up." (Fault) -- the compass never started and
	 * the Compass app's heading stayed "--".
	 */
	if (key != NULL && value != NULL) {
		struct sensor_service *s = refcon;

		pthread_mutex_lock(&s->lock);
		CFDictionarySetValue(s->properties, key, value);
		pthread_mutex_unlock(&s->lock);
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
	pthread_mutex_init(&made->lock, NULL);
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
	target = target < 0.025 ? 0.025 : target > 1.0 ? 1.0 : target;
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
 * (Test 243 / 434) The magnetometer hard-iron calibrated before iOS sees it:
 *   1. Persist learned offset across boots in /private/var/tmp/r5q_magcal.dat.
 *   2. Reject transient single-sample glitches (> 80 uT jump from last good sample) unless >= 3 consecutive samples agree.
 *   3. Seed from the SLPI sns_mag_cal bias at status >= 1, and stop our own fit only at status >= 2 (Tests 434, 451),
 *      checking sign ambiguity automatically.
 *   4. Fallback 2-pass outlier-trimmed sphere fit over up to 64 samples (>= 4 uT spacing, >= 18 uT span on all 3 axes,
 *      radius 20..80 uT, residual < 7.5 uT), blended smoothly so compass headings do not jump.
 */
#include <time.h>
#define MAGCAL_SAMPLES          64
#define MAGCAL_CACHE_PATH       "/private/var/tmp/r5q_magcal.dat"

static double magcal_offset[3] = { -95.0, 40.0, -445.0 };
static double magcal_saved[3] = { -95.0, 40.0, -445.0 };
static time_t magcal_saved_at;

static void
magcal_load(void)
{
	FILE *fp = fopen(MAGCAL_CACHE_PATH, "rb");
	double buf[3];
	char line[160];

	if (fp == NULL) {
		return;
	}
	if (fread(buf, sizeof(double), 3, fp) == 3 &&
	    isfinite(buf[0]) && isfinite(buf[1]) && isfinite(buf[2]) &&
	    fabs(buf[0]) < 1000.0 && fabs(buf[1]) < 1000.0 && fabs(buf[2]) < 1000.0) {
		memcpy(magcal_offset, buf, sizeof(magcal_offset));
		memcpy(magcal_saved, buf, sizeof(magcal_saved));
		snprintf(line, sizeof(line), "r5q-bbd: compass: loaded offset %.1f %.1f %.1f from %s\n",
		    buf[0], buf[1], buf[2], MAGCAL_CACHE_PATH);
		say(line);
	}
	fclose(fp);
}

static void
magcal_save_if_needed(void)
{
	time_t now = time(NULL);
	double d2 = 0.0;
	FILE *fp;

	for (int k = 0; k < 3; k++) {
		double d = magcal_offset[k] - magcal_saved[k];
		d2 += d * d;
	}
	if (d2 < 4.0 || (magcal_saved_at != 0 && now - magcal_saved_at < 30)) {
		return;
	}
	fp = fopen(MAGCAL_CACHE_PATH ".tmp", "wb");
	if (fp != NULL) {
		size_t w = fwrite(magcal_offset, sizeof(double), 3, fp);
		fclose(fp);
		if (w == 3 && rename(MAGCAL_CACHE_PATH ".tmp", MAGCAL_CACHE_PATH) == 0) {
			memcpy(magcal_saved, magcal_offset, sizeof(magcal_saved));
			magcal_saved_at = now;
		}
	}
}

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

static bool
fit_sphere(const double pts[][3], unsigned n, double x[4], double *r_out, double *res_out)
{
	double a[4][5] = { { 0 } }, r, rad2, res = 0.0;

	for (unsigned i = 0; i < n; i++) {
		double row[5] = { 2 * pts[i][0], 2 * pts[i][1], 2 * pts[i][2], 1,
			pts[i][0] * pts[i][0] + pts[i][1] * pts[i][1] + pts[i][2] * pts[i][2] };

		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 5; k++) {
				a[j][k] += row[j] * row[k];
			}
		}
	}
	if (!solve4(a, x)) {
		return false;
	}
	rad2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
	if (rad2 <= 0.0 || !isfinite(rad2)) {
		return false;
	}
	r = sqrt(rad2);
	for (unsigned i = 0; i < n; i++) {
		double dx = pts[i][0] - x[0], dy = pts[i][1] - x[1], dz = pts[i][2] - x[2];
		double d = sqrt(dx * dx + dy * dy + dz * dz) - r;

		res += d * d;
	}
	*r_out = r;
	*res_out = sqrt(res / n);
	return isfinite(r) && isfinite(*res_out);
}

static void
magcal_feed(const double raw[3])
{
	static double kept[MAGCAL_SAMPLES][3];
	static unsigned n, next, fresh, fits;
	double minv[3], maxv[3], x[4], r = 0.0, res = 0.0;
	char line[200];

	for (unsigned i = 0; i < n; i++) {
		double d = 0;

		for (int k = 0; k < 3; k++) {
			d += (raw[k] - kept[i][k]) * (raw[k] - kept[i][k]);
		}
		if (d < 4.0 * 4.0) {
			return;                         /* too near one already kept */
		}
	}
	memcpy(kept[next], raw, sizeof(kept[0]));
	next = (next + 1) % MAGCAL_SAMPLES;
	n = n < MAGCAL_SAMPLES ? n + 1 : n;
	if (++fresh < 6 || n < 12) {
		return;
	}
	fresh = 0;
	for (int k = 0; k < 3; k++) {
		minv[k] = maxv[k] = kept[0][k];
	}
	for (unsigned i = 1; i < n; i++) {
		for (int k = 0; k < 3; k++) {
			if (kept[i][k] < minv[k]) minv[k] = kept[i][k];
			if (kept[i][k] > maxv[k]) maxv[k] = kept[i][k];
		}
	}
	if (maxv[0] - minv[0] < 18.0 || maxv[1] - minv[1] < 18.0 || maxv[2] - minv[2] < 18.0) {
		return;
	}
	if (!fit_sphere(kept, n, x, &r, &res)) {
		return;
	}
	/* Second IRLS pass dropping > 10 uT residuals if >= 16 inliers remain. */
	{
		double inliers[MAGCAL_SAMPLES][3];
		unsigned ni = 0;

		for (unsigned i = 0; i < n; i++) {
			double dx = kept[i][0] - x[0], dy = kept[i][1] - x[1], dz = kept[i][2] - x[2];
			double d = fabs(sqrt(dx * dx + dy * dy + dz * dz) - r);

			if (d <= 10.0) {
				memcpy(inliers[ni++], kept[i], sizeof(inliers[0]));
			}
		}
		if (ni >= 16 && ni < n) {
			double x2[4], r2 = 0.0, res2 = 0.0;

			if (fit_sphere(inliers, ni, x2, &r2, &res2)) {
				memcpy(x, x2, sizeof(x));
				r = r2;
				res = res2;
			}
		}
	}
	bool accept = (r >= 20.0 && r <= 80.0 && res < 7.5 &&
	    fabs(x[0]) < 1000.0 && fabs(x[1]) < 1000.0 && fabs(x[2]) < 1000.0);
	if (accept) {
		for (int k = 0; k < 3; k++) {
			magcal_offset[k] = 0.65 * magcal_offset[k] + 0.35 * x[k];
		}
		magcal_save_if_needed();
	}
	if (fits++ < 10 || fits % 50 == 0) {
		snprintf(line, sizeof(line), "r5q-bbd: compass: fit of %u samples: centre %.1f %.1f %.1f, radius %.1f uT, residual %.1f: %s\n",
		    n, x[0], x[1], x[2], r, res, accept ? "taken" : "not taken");
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
	magcal_load();
	/* (Test 230) the gyroscope (0xff00/9), rad/s on the same axes as Android's: CoreMotion's gyro and device motion */
	gyro = sensor_service(client, "Redwood gyroscope", 0xff00, 0x09);
	snprintf(line, sizeof(line), "r5q-bbd: sensors: ambient light service %p, compass service %p\n", (void *)light, (void *)compass);
	say(line);
	uint32_t last_cal_count = 0, spikes_dropped = 0, slpi_cal_logs = 0;
	bool slpi_cal_active = false, have_prev_mag = false, disp_off = false;
	double prev_mag[3] = { 0 };
	unsigned mag_jump_streak = 0;
	/*
	 * (Test 348 / 440) Every 5 ms while screen is on, backing off to 50 ms (20 Hz) while display is asleep
	 * (kern.redwood.cpufreq_state contains "disp=off") to eliminate 180/s idle userland wakeups.
	 */
	for (unsigned round = 0;; round++) {
		usleep(disp_off ? 50000 : 5000);
		if (round % (disp_off ? 20 : 200) == 0) {
			char cbuf[192];
			size_t csz = sizeof(cbuf) - 1;
			if (sysctlbyname("kern.redwood.cpufreq_state", cbuf, &csz, NULL, 0) == 0) {
				cbuf[csz] = '\0';
				disp_off = (strstr(cbuf, "disp=off") != NULL);
			}
		}
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
		/* (Test 349) every new sample of each, each with the time it came (the kernel's ring), not only the newest */
		{
			int32_t v[16][3];
			uint64_t t[16];
			unsigned n;

			if (compass != NULL && (n = motion_new(&m, 2, &last_mag, v, t)) != 0) {
				for (unsigned i = 0; i < n; i++) {
					double raw[3] = { v[i][0] / 1000.0, v[i][1] / 1000.0, v[i][2] / 1000.0 };
					IOHIDEventRef event;

					if (!isfinite(raw[0]) || !isfinite(raw[1]) || !isfinite(raw[2]) ||
					    fabs(raw[0]) > 5000.0 || fabs(raw[1]) > 5000.0 || fabs(raw[2]) > 5000.0) {
						continue;
					}
					if (have_prev_mag) {
						double d2 = (raw[0] - prev_mag[0]) * (raw[0] - prev_mag[0]) +
						    (raw[1] - prev_mag[1]) * (raw[1] - prev_mag[1]) +
						    (raw[2] - prev_mag[2]) * (raw[2] - prev_mag[2]);
						if (d2 > 80.0 * 80.0) {
							if (++mag_jump_streak < 3) {
								if (spikes_dropped++ < 10) {
									snprintf(line, sizeof(line),
									    "r5q-bbd: compass: dropped spike %.1f %.1f %.1f (prev %.1f %.1f %.1f)\n",
									    raw[0], raw[1], raw[2], prev_mag[0], prev_mag[1], prev_mag[2]);
									say(line);
								}
								continue;
							}
						}
					}
					mag_jump_streak = 0;
					memcpy(prev_mag, raw, sizeof(prev_mag));
					have_prev_mag = true;

					if (size >= sizeof(m) && m.version >= 3 && m.mag_cal_status >= 1) {
						if (m.mag_cal_count != last_cal_count) {
							double sb[3] = {
								m.mag_cal_bias[0] / 1000.0,
								m.mag_cal_bias[1] / 1000.0,
								m.mag_cal_bias[2] / 1000.0,
							};
							double mag_sb = sqrt(sb[0] * sb[0] + sb[1] * sb[1] + sb[2] * sb[2]);

							last_cal_count = m.mag_cal_count;
							if (isfinite(mag_sb) && mag_sb > 5.0 && mag_sb < 1500.0) {
								double dm2 = (raw[0] - sb[0]) * (raw[0] - sb[0]) +
								    (raw[1] - sb[1]) * (raw[1] - sb[1]) +
								    (raw[2] - sb[2]) * (raw[2] - sb[2]);
								double dp2 = (raw[0] + sb[0]) * (raw[0] + sb[0]) +
								    (raw[1] + sb[1]) * (raw[1] + sb[1]) +
								    (raw[2] + sb[2]) * (raw[2] + sb[2]);
								double sgn = (dp2 < dm2 * 0.25 && dm2 > 10000.0) ? -1.0 : 1.0;
								double alpha = slpi_cal_active ? 0.25 : 0.60;

								for (int k = 0; k < 3; k++) {
									double eff = sgn * sb[k];
									magcal_offset[k] = (1.0 - alpha) * magcal_offset[k] + alpha * eff;
								}
								/*
								 * (Test 451) a status-1 (low accuracy) bias seeds the offset as before, but only
								 * status >= 2 stops our own sphere fit: a poor first bias no longer pins the
								 * hard-iron offset for the rest of the boot
								 */
								if (m.mag_cal_status >= 2) {
									slpi_cal_active = true;
								}
								magcal_save_if_needed();
								if (slpi_cal_logs++ < 5) {
									snprintf(line, sizeof(line),
									    "r5q-bbd: compass: SLPI mag_cal st=%u sgn=%.0f bias %.1f %.1f %.1f -> off %.1f %.1f %.1f\n",
									    m.mag_cal_status, sgn, sb[0], sb[1], sb[2],
									    magcal_offset[0], magcal_offset[1], magcal_offset[2]);
									say(line);
								}
							}
						}
						if (!slpi_cal_active) {
							magcal_feed(raw);       /* (Test 451) still ours until SLPI is confident */
						}
					} else {
						slpi_cal_active = false;        /* (Test 451) SLPI fell back to uncalibrated: our fit again */
						magcal_feed(raw);
					}
					/*
					 * (Test 445) Pass linear hard-iron-corrected field without non-linear radial
					 * clamping so CoreMotion/CLCompassCalculator sees a true 3D sphere and locks
					 * high-accuracy heading cleanly on rotation.
					 */
					event = IOHIDEventCreateCompassEvent(NULL, t[i],
					    raw[0] - magcal_offset[0], raw[1] - magcal_offset[1], raw[2] - magcal_offset[2], 0);
					if (event != NULL) {
						IOHIDEventSetIntegerValue(event, 0x4, 1);
						IOHIDVirtualServiceClientDispatchEvent(compass, event);
						CFRelease(event);
					}
				}
			}
			if (gyro != NULL && (n = motion_new(&m, 1, &last_gyro, v, t)) != 0) {
				for (unsigned i = 0; i < n; i++) {
					IOHIDEventRef event = IOHIDEventCreateGyroEvent(NULL, t[i], v[i][0] / 1000.0, v[i][1] / 1000.0,
					    v[i][2] / 1000.0, 0);

					if (event != NULL) {
						IOHIDEventSetIntegerValue(event, 0x4, 1);
						IOHIDVirtualServiceClientDispatchEvent(gyro, event);
						CFRelease(event);
					}
				}
			}
			n = motion_new(&m, 0, &last, v, t);
			for (unsigned i = 0; i < n; i++) {
				double x = -v[i][0] / 9806.65, y = -v[i][1] / 9806.65, z = -v[i][2] / 9806.65;
				IOHIDEventRef event = IOHIDEventCreateAccelerometerEvent(NULL, t[i], x, y, z, 0);

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
	}
	return NULL;
}

/*
 * (Test 435) QuartzCore virtual display writeback (ReplayKit / AirPlay cloning).
 * Without AppleM2ScalerCSCDriver, CA::WindowServer::IOSurface::copy_surface (vtable 0x1d5b38a38 slot +160 =
 * 0x1d5b38ad8 -> 0x188321ff0) fails IOSurfaceAcceleratorCreate and returns false ("CoreAnimation: failed to copy
 * main layer"). When the original method returns false, lock src and dst IOSurfaces and perform software scaling,
 * rotation, and BGRA/RGBA -> NV12 ('420f'/'420v') or 32-bit copy into dst (stored at *((IOSurfaceRef *)dst_obj + 8)).
 */
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>

#define WS_IOSURFACE_COPY_SLOT  0x1d5b38ad8ull
#define WS_IOSURFACE_COPY_IMPL  0x188321ff0ull

static bool (*orig_ws_iosurface_copy)(void *dst_obj, IOSurfaceRef src, uint32_t flags, uint32_t transform,
    const void *src_rect, const void *dst_rect, float scale);

static inline uint8_t
clamp_u8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static inline void
unpack_rgb(const uint8_t *px, uint32_t fmt, int *r, int *g, int *b)
{
	if (fmt == 0x52474241u /* RGBA */) {
		*r = px[0];
		*g = px[1];
		*b = px[2];
	} else if (fmt == 0x41524742u /* ARGB */) {
		*r = px[1];
		*g = px[2];
		*b = px[3];
	} else if (fmt == 0x41424752u /* ABGR */) {
		*b = px[1];
		*g = px[2];
		*r = px[3];
	} else { /* 0x42475241 BGRA and default */
		*b = px[0];
		*g = px[1];
		*r = px[2];
	}
}

static inline uint8_t
rgb_to_y(int r, int g, int b, bool full_range)
{
	return full_range ? clamp_u8((77 * r + 150 * g + 29 * b + 128) >> 8)
	                  : clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

static inline void
rgb_to_uv(int r, int g, int b, bool full_range, uint8_t *cb, uint8_t *cr)
{
	if (full_range) {
		*cb = clamp_u8(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
		*cr = clamp_u8(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
	} else {
		*cb = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
		*cr = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
	}
}

static inline void
map_rotated_coords(size_t dx, size_t dy, size_t dw, size_t dh, size_t sw, size_t sh, uint32_t rot, size_t *sx, size_t *sy)
{
	size_t x, y;

	switch (rot & 3u) {
	case 1: /* 90 CW */
		x = (dy * sw) / dh;
		y = ((dw - 1 - dx) * sh) / dw;
		break;
	case 2: /* 180 */
		x = ((dw - 1 - dx) * sw) / dw;
		y = ((dh - 1 - dy) * sh) / dh;
		break;
	case 3: /* 270 CW */
		x = ((dh - 1 - dy) * sw) / dh;
		y = (dx * sh) / dw;
		break;
	default:
		x = (dx * sw) / dw;
		y = (dy * sh) / dh;
		break;
	}
	*sx = x < sw ? x : sw - 1;
	*sy = y < sh ? y : sh - 1;
}

static bool
blit_to_nv12(const uint8_t *src_base, size_t src_w, size_t src_h, size_t src_bpr, uint32_t src_fmt,
    IOSurfaceRef dst, bool full_range, uint32_t flags, uint32_t transform)
{
	uint8_t *y_base = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(dst, 0);
	uint8_t *uv_base = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(dst, 1);
	size_t y_bpr = IOSurfaceGetBytesPerRowOfPlane(dst, 0);
	size_t uv_bpr = IOSurfaceGetBytesPerRowOfPlane(dst, 1);
	size_t y_w = IOSurfaceGetWidthOfPlane(dst, 0);
	size_t y_h = IOSurfaceGetHeightOfPlane(dst, 0);
	size_t uv_w = IOSurfaceGetWidthOfPlane(dst, 1);
	size_t uv_h = IOSurfaceGetHeightOfPlane(dst, 1);
	uint32_t sx_off[4096];

	if (y_base == NULL || uv_base == NULL || y_w == 0 || y_h == 0 || y_w > 4096 ||
	    y_bpr < y_w || uv_w == 0 || uv_h == 0 || uv_bpr < uv_w * 2) {
		return false;
	}
	if ((flags & 7u) == 0 && transform == 0) {
		for (size_t x = 0; x < y_w; x++) {
			size_t sx = (x * src_w) / y_w;
			if (sx >= src_w) {
				sx = src_w - 1;
			}
			sx_off[x] = (uint32_t)(sx * 4u);
		}
		for (size_t dy = 0; dy < y_h; dy += 2) {
			size_t sy0 = (dy * src_h) / y_h;
			size_t sy1 = dy + 1 < y_h ? ((dy + 1) * src_h) / y_h : sy0;
			const uint8_t *s0 = src_base + (sy0 < src_h ? sy0 : src_h - 1) * src_bpr;
			const uint8_t *s1 = src_base + (sy1 < src_h ? sy1 : src_h - 1) * src_bpr;
			uint8_t *dy0 = y_base + dy * y_bpr;
			uint8_t *dy1 = dy + 1 < y_h ? y_base + (dy + 1) * y_bpr : NULL;
			size_t uvy = dy >> 1;
			uint8_t *duv = uvy < uv_h ? uv_base + uvy * uv_bpr : NULL;
			size_t dx = 0;

			for (; dx + 1 < y_w; dx += 2) {
				int r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;
				uint32_t ox0 = sx_off[dx], ox1 = sx_off[dx + 1];

				unpack_rgb(s0 + ox0, src_fmt, &r00, &g00, &b00);
				unpack_rgb(s0 + ox1, src_fmt, &r01, &g01, &b01);
				dy0[dx] = rgb_to_y(r00, g00, b00, full_range);
				dy0[dx + 1] = rgb_to_y(r01, g01, b01, full_range);
				if (dy1 != NULL) {
					unpack_rgb(s1 + ox0, src_fmt, &r10, &g10, &b10);
					unpack_rgb(s1 + ox1, src_fmt, &r11, &g11, &b11);
					dy1[dx] = rgb_to_y(r10, g10, b10, full_range);
					dy1[dx + 1] = rgb_to_y(r11, g11, b11, full_range);
				} else {
					r10 = r00; g10 = g00; b10 = b00;
					r11 = r01; g11 = g01; b11 = b01;
				}
				if (duv != NULL && dx + 1 < uv_w * 2) {
					int mr = (r00 + r01 + r10 + r11 + 2) >> 2;
					int mg = (g00 + g01 + g10 + g11 + 2) >> 2;
					int mb = (b00 + b01 + b10 + b11 + 2) >> 2;
					rgb_to_uv(mr, mg, mb, full_range, &duv[dx], &duv[dx + 1]);
				}
			}
			if (dx < y_w) {
				int r, g, b;
				uint32_t ox = sx_off[dx];

				unpack_rgb(s0 + ox, src_fmt, &r, &g, &b);
				dy0[dx] = rgb_to_y(r, g, b, full_range);
				if (duv != NULL && dx + 1 < uv_w * 2) {
					rgb_to_uv(r, g, b, full_range, &duv[dx], &duv[dx + 1]);
				}
				if (dy1 != NULL) {
					unpack_rgb(s1 + ox, src_fmt, &r, &g, &b);
					dy1[dx] = rgb_to_y(r, g, b, full_range);
				}
			}
		}
		return true;
	} else {
		uint32_t rot = transform != 0 ? (transform & 3u) : (flags & 7u);

		for (size_t dy = 0; dy < y_h; dy++) {
			uint8_t *dy_row = y_base + dy * y_bpr;
			uint8_t *duv = (dy & 1u) == 0 && (dy >> 1) < uv_h ? uv_base + (dy >> 1) * uv_bpr : NULL;

			for (size_t dx = 0; dx < y_w; dx++) {
				size_t sx, sy;
				int r, g, b;

				map_rotated_coords(dx, dy, y_w, y_h, src_w, src_h, rot, &sx, &sy);
				unpack_rgb(src_base + sy * src_bpr + sx * 4u, src_fmt, &r, &g, &b);
				dy_row[dx] = rgb_to_y(r, g, b, full_range);
				if (duv != NULL && (dx & 1u) == 0 && dx + 1 < uv_w * 2) {
					rgb_to_uv(r, g, b, full_range, &duv[dx], &duv[dx + 1]);
				}
			}
		}
		return true;
	}
}

static bool
blit_to_32bit(const uint8_t *src_base, size_t src_w, size_t src_h, size_t src_bpr, uint32_t src_fmt,
    IOSurfaceRef dst, uint32_t dst_fmt)
{
	uint8_t *dst_base = (uint8_t *)IOSurfaceGetBaseAddress(dst);
	size_t dst_w = IOSurfaceGetWidth(dst);
	size_t dst_h = IOSurfaceGetHeight(dst);
	size_t dst_bpr = IOSurfaceGetBytesPerRow(dst);

	if (dst_base == NULL || dst_w == 0 || dst_h == 0 || dst_w > 4096 || dst_bpr < dst_w * 4u) {
		return false;
	}
	for (size_t dy = 0; dy < dst_h; dy++) {
		size_t sy = (dy * src_h) / dst_h;
		const uint32_t *srow = (const uint32_t *)(src_base + (sy < src_h ? sy : src_h - 1) * src_bpr);
		uint32_t *drow = (uint32_t *)(dst_base + dy * dst_bpr);

		for (size_t dx = 0; dx < dst_w; dx++) {
			size_t sx = (dx * src_w) / dst_w;
			uint32_t px = srow[sx < src_w ? sx : src_w - 1];

			if (src_fmt != dst_fmt) {
				px = (px & 0xff00ff00u) | ((px & 0x00ff0000u) >> 16) | ((px & 0x000000ffu) << 16);
			}
			drow[dx] = px;
		}
	}
	return true;
}

static bool
my_ws_iosurface_copy(void *dst_obj, IOSurfaceRef src, uint32_t flags, uint32_t transform,
    const void *src_rect, const void *dst_rect, float scale)
{
	static unsigned calls;
	IOSurfaceRef dst;
	bool ok = false;

	if (orig_ws_iosurface_copy != NULL && orig_ws_iosurface_copy(dst_obj, src, flags, transform, src_rect, dst_rect, scale)) {
		return true;
	}
	if (dst_obj == NULL || src == NULL) {
		return false;
	}
	dst = *(IOSurfaceRef *)((char *)dst_obj + 64);
	if (dst == NULL) {
		return false;
	}
	if (IOSurfaceLock(src, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
		return false;
	}
	if (IOSurfaceLock(dst, 0, NULL) != kIOReturnSuccess) {
		IOSurfaceUnlock(src, kIOSurfaceLockReadOnly, NULL);
		return false;
	}
	{
		size_t src_w = IOSurfaceGetWidth(src);
		size_t src_h = IOSurfaceGetHeight(src);
		size_t src_bpr = IOSurfaceGetBytesPerRow(src);
		uint32_t src_fmt = IOSurfaceGetPixelFormat(src);
		const uint8_t *src_base = (const uint8_t *)IOSurfaceGetBaseAddress(src);
		size_t dst_w = IOSurfaceGetWidth(dst);
		size_t dst_h = IOSurfaceGetHeight(dst);
		uint32_t dst_fmt = IOSurfaceGetPixelFormat(dst);
		size_t dst_planes = IOSurfaceGetPlaneCount(dst);
		unsigned n = calls++;

		if (n < 3 || (n % 300) == 0) {
			char line[240];

			snprintf(line, sizeof(line),
			    "r5q-bbd: copy_surface #%u: src=%zux%zu fmt=0x%08x bpr=%zu -> dst=%zux%zu fmt=0x%08x planes=%zu flags=0x%x xform=%u rects=%d/%d\n",
			    n, src_w, src_h, src_fmt, src_bpr, dst_w, dst_h, dst_fmt, dst_planes, flags, transform,
			    src_rect != NULL ? 1 : 0, dst_rect != NULL ? 1 : 0);
			say(line);
		}
		if (src_base != NULL && src_w > 0 && src_h > 0 && src_bpr >= src_w * 4u) {
			if ((dst_fmt == 0x34323066u /* 420f */ || dst_fmt == 0x34323076u /* 420v */) && dst_planes >= 2) {
				ok = blit_to_nv12(src_base, src_w, src_h, src_bpr, src_fmt, dst,
				    dst_fmt == 0x34323066u, flags, transform);
			} else if (dst_fmt == 0x42475241u /* BGRA */ || dst_fmt == 0x52474241u /* RGBA */) {
				ok = blit_to_32bit(src_base, src_w, src_h, src_bpr, src_fmt, dst, dst_fmt);
			}
		}
	}
	IOSurfaceUnlock(dst, 0, NULL);
	IOSurfaceUnlock(src, kIOSurfaceLockReadOnly, NULL);
	return ok;
}

static void
iosurface_copy_hook(void)
{
	size_t length = 0;
	uintptr_t start = (uintptr_t)_dyld_get_shared_cache_range(&length), slide;
	uintptr_t *entry;
	vm_address_t page;
	char line[200];

	if (start == 0) {
		return;
	}
	slide = start - CACHE_UNSLID_BASE;
	entry = (uintptr_t *)(WS_IOSURFACE_COPY_SLOT + slide);
	if (*entry != WS_IOSURFACE_COPY_IMPL + slide) {
		snprintf(line, sizeof(line), "r5q-bbd: QuartzCore IOSurface::copy_surface: found %#lx, expected %#llx; left alone\n",
		    *entry - slide, WS_IOSURFACE_COPY_IMPL);
		say(line);
		return;
	}
	page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("r5q-bbd: QuartzCore IOSurface::copy_surface: cannot make vtable writable\n");
		return;
	}
	orig_ws_iosurface_copy = (bool (*)(void *, IOSurfaceRef, uint32_t, uint32_t, const void *, const void *, float))*entry;
	*entry = (uintptr_t)my_ws_iosurface_copy;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("r5q-bbd: QuartzCore IOSurface::copy_surface hooked for virtual display writeback\n");
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t thread;

	no_als_wait();
	orientation_hook();
	iosurface_copy_hook();
	if (pthread_create(&thread, NULL, sensors_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}
