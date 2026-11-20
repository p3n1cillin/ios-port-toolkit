/*
 * mgq KEY... (2026-11-20, cellular Test 390): MobileGestalt's answers for KEYs, as iOS sees them (MGCopyAnswer from
 * /usr/lib/libMobileGestalt.dylib, loaded at run time: the SDK has no stub for it). Prints "KEY = value" (booleans,
 * numbers, strings; other types by their description). For capability questions (cellular-data, telephony, ...):
 * never ask it for identifiers (IMEI, serial numbers) -- they are private and would land in logs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>

typedef CFTypeRef (*mg_copy_t)(CFStringRef key, void *unused);

int
main(int argc, char **argv)
{
	void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	mg_copy_t copy = h ? (mg_copy_t)dlsym(h, "MGCopyAnswer") : NULL;

	if (copy == NULL) {
		printf("mgq: no MGCopyAnswer (%s)\n", dlerror());
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
		CFTypeRef v = copy(key, NULL);
		char buf[256] = "(null)";

		if (v != NULL) {
			if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
				snprintf(buf, sizeof(buf), "%s", CFBooleanGetValue(v) ? "true" : "false");
			} else if (CFGetTypeID(v) == CFStringGetTypeID()) {
				CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
			} else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
				long long n = 0;
				CFNumberGetValue(v, kCFNumberLongLongType, &n);
				snprintf(buf, sizeof(buf), "%lld", n);
			} else {
				CFStringRef d = CFCopyDescription(v);
				CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
				CFRelease(d);
			}
			CFRelease(v);
		}
		printf("%s = %s\n", argv[i], buf);
		CFRelease(key);
	}
	return 0;
}
