/*
 * dtq PATH... (2026-11-20, cellular Test 390): an IORegistry entry by path (e.g. IODeviceTree:/baseband) -- found or
 * not, its name, and its property names with their sizes (not their values: some hold identifiers).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>

static void
show(const void *key, const void *value, void *ctx)
{
	char k[128] = "?";
	long size = -1;

	(void)ctx;
	CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
	if (CFGetTypeID(value) == CFDataGetTypeID()) {
		size = CFDataGetLength(value);
	} else if (CFGetTypeID(value) == CFStringGetTypeID()) {
		size = CFStringGetLength(value);
	}
	printf("    %s (%ld)\n", k, size);
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		io_registry_entry_t e = IORegistryEntryFromPath(kIOMainPortDefault, argv[i]);
		CFMutableDictionaryRef props = NULL;
		io_name_t name = "";

		if (e == MACH_PORT_NULL) {
			printf("%s: not found\n", argv[i]);
			continue;
		}
		IORegistryEntryGetName(e, name);
		printf("%s: found, name \"%s\"\n", argv[i], name);
		if (IORegistryEntryCreateCFProperties(e, &props, NULL, 0) == KERN_SUCCESS && props != NULL) {
			CFDictionaryApplyFunction(props, show, NULL);
			CFRelease(props);
		}
		io_iterator_t it;
		if (IORegistryEntryGetChildIterator(e, "IODeviceTree", &it) == KERN_SUCCESS) {
			io_registry_entry_t c;
			printf("  children:");
			while ((c = IOIteratorNext(it)) != MACH_PORT_NULL) {
				io_name_t cn = "";
				IORegistryEntryGetName(c, cn);
				printf(" %s", cn);
				IOObjectRelease(c);
			}
			printf("\n");
			IOObjectRelease(it);
		}
		IOObjectRelease(e);
	}
	return 0;
}
