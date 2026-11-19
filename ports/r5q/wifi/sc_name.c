/*
 * sc_name [NAME]: the device's name as SystemConfiguration keeps it (2026-11-19, the Bluetooth name).
 *
 * bluetoothd names the phone from SCDynamicStoreCopyComputerName (configd's dynamic store), which configd fills from
 * /var/preferences/SystemConfiguration/preferences.plist (System > System > ComputerName). On an iPhone lockdownd
 * writes it; the port runs no lockdownd (its job panics the system on a crash and opens TCP 62078), so it was never
 * set and Settings > Bluetooth said: This iPhone is discoverable as "(null)".
 *
 *   sc_name          print the dynamic store's ComputerName and the preferences' ComputerName / HostName
 *   sc_name NAME     set both preferences (SCPreferencesSetComputerName, SCPreferencesSetLocalHostName from NAME
 *                    with spaces as '-'), commit and apply (configd then updates the dynamic store), print again
 * Run as root (r5qd). Syncs after writing: a restart of the port is a hard reset. Built by pipeline/build_phone_tools.sh.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef const struct __SCDynamicStore *SCDynamicStoreRef;
typedef const struct __SCPreferences *SCPreferencesRef;
extern CFStringRef SCDynamicStoreCopyComputerName(SCDynamicStoreRef store, CFStringEncoding *nameEncoding);
extern SCPreferencesRef SCPreferencesCreate(CFAllocatorRef allocator, CFStringRef name, CFStringRef prefsID);
extern Boolean SCPreferencesSetComputerName(SCPreferencesRef prefs, CFStringRef name, CFStringEncoding nameEncoding);
extern Boolean SCPreferencesSetLocalHostName(SCPreferencesRef prefs, CFStringRef name);
extern CFStringRef SCPreferencesGetHostName(SCPreferencesRef prefs);
extern Boolean SCPreferencesCommitChanges(SCPreferencesRef prefs);
extern Boolean SCPreferencesApplyChanges(SCPreferencesRef prefs);
extern CFPropertyListRef SCPreferencesPathGetValue(SCPreferencesRef prefs, CFStringRef path);
extern int SCError(void);
extern const char *SCErrorString(int status);

static void
say(const char *what, CFTypeRef value)
{
	char buf[256] = "(none)";

	if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)value, buf, sizeof(buf), kCFStringEncodingUTF8);
	} else if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		CFTypeRef name = CFDictionaryGetValue((CFDictionaryRef)value, CFSTR("ComputerName"));

		snprintf(buf, sizeof(buf), "{ComputerName: ...}");
		if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
			CFStringGetCString((CFStringRef)name, buf, sizeof(buf), kCFStringEncodingUTF8);
		}
	}
	printf("%-34s %s\n", what, buf);
}

static void
show(void)
{
	SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("sc_name"), NULL);
	CFStringRef dyn = SCDynamicStoreCopyComputerName(NULL, NULL);

	say("dynamic store ComputerName:", dyn);
	if (dyn != NULL) {
		CFRelease(dyn);
	}
	if (prefs != NULL) {
		say("preferences System/System:", SCPreferencesPathGetValue(prefs, CFSTR("/System/System")));
		say("preferences HostName:", SCPreferencesGetHostName(prefs));
		CFRelease(prefs);
	}
}

int
main(int argc, char **argv)
{
	show();
	if (argc > 1) {
		SCPreferencesRef prefs = SCPreferencesCreate(NULL, CFSTR("sc_name"), NULL);
		CFStringRef name = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
		char local[64];
		CFStringRef lname;
		bool ok;

		snprintf(local, sizeof(local), "%s", argv[1]);
		for (char *p = local; *p; p++) {
			if (*p == ' ') {
				*p = '-';
			}
		}
		lname = CFStringCreateWithCString(NULL, local, kCFStringEncodingUTF8);
		ok = prefs != NULL && name != NULL && SCPreferencesSetComputerName(prefs, name, kCFStringEncodingUTF8) &&
		    SCPreferencesSetLocalHostName(prefs, lname) && SCPreferencesCommitChanges(prefs) &&
		    SCPreferencesApplyChanges(prefs);
		printf("set \"%s\" (local host name \"%s\"): %s\n", argv[1], local, ok ? "done" : SCErrorString(SCError()));
		sync();         /* (the port's restarts are hard resets: unflushed writes to /var are lost) */
		sleep(1);
		show();
	}
	return 0;
}
