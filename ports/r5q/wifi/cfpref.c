/*
 * cfpref DOMAIN USER KEY [bool|int|str VALUE]: read (or set, then read back) one preference through cfprefsd, for USER
 * ("mobile", "root" or "any"), any host. A test helper over r5qd (r5q, Test 197: locationd's
 * LocationServicesEnabledIn7.0).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CFStringRef
cfs(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

int
main(int argc, char **argv)
{
	if (argc != 4 && argc != 6) {
		printf("usage: cfpref DOMAIN USER KEY [bool|int|str VALUE]\n");
		return 1;
	}
	CFStringRef domain = cfs(argv[1]), key = cfs(argv[3]);
	CFStringRef user = strcmp(argv[2], "any") == 0 ? kCFPreferencesAnyUser : cfs(argv[2]);

	if (argc == 6) {
		CFPropertyListRef value = NULL;
		if (strcmp(argv[4], "bool") == 0) {
			value = atoi(argv[5]) ? kCFBooleanTrue : kCFBooleanFalse;
		} else if (strcmp(argv[4], "int") == 0) {
			int n = atoi(argv[5]);
			value = CFNumberCreate(NULL, kCFNumberIntType, &n);
		} else {
			value = cfs(argv[5]);
		}
		CFPreferencesSetValue(key, value, domain, user, kCFPreferencesAnyHost);
		printf("cfpref: synchronize %s\n", CFPreferencesSynchronize(domain, user, kCFPreferencesAnyHost) ? "ok" : "FAILED");
	}
	CFPropertyListRef now = CFPreferencesCopyValue(key, domain, user, kCFPreferencesAnyHost);
	if (now) {
		CFStringRef d = CFCopyDescription(now);
		char buf[256];
		CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
		printf("cfpref: %s %s %s = %s\n", argv[1], argv[2], argv[3], buf);
	} else {
		printf("cfpref: %s %s %s is not set\n", argv[1], argv[2], argv[3]);
	}
	return 0;
}
