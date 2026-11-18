/*
 * appinst PATH.app|PATH.ipa [--uninstall BUNDLE_ID]: install an application through iOS's own installer (installd, by
 * LSApplicationWorkspace), and say what it answered. A test helper over r5qd: can an application built here, or one
 * the owner has, be installed on this phone?
 */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			printf("usage: appinst PATH.app|PATH.ipa | appinst --uninstall BUNDLE_ID\n");
			return 1;
		}
		id w = ((id (*)(id, SEL))objc_msgSend)(objc_getClass("LSApplicationWorkspace"), sel_registerName("defaultWorkspace"));
		NSError *error = nil;
		BOOL ok;

		if (strcmp(argv[1], "--uninstall") == 0 && argc > 2) {
			ok = ((BOOL (*)(id, SEL, id, id))objc_msgSend)(w, sel_registerName("uninstallApplication:withOptions:"),
			    [NSString stringWithUTF8String:argv[2]], nil);
			printf("appinst: uninstall %s: %s %s\n", argv[2], ok ? "done" : "FAILED", error.description.UTF8String ?: "");
			return ok ? 0 : 1;
		}
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
		NSMutableDictionary *options = [@{ @"PackageType" : @"Developer" } mutableCopy];
		if (argc > 2) {
			options[@"CFBundleIdentifier"] = [NSString stringWithUTF8String:argv[2]];     /* appinst PATH BUNDLE_ID */
		}
		if (argc > 3) {
			options[@"PackageType"] = [NSString stringWithUTF8String:argv[3]];            /* ... and Developer|Customer|System */
		}
		ok = ((BOOL (*)(id, SEL, id, id, NSError **))objc_msgSend)(w, sel_registerName("installApplication:withOptions:error:"), url,
		    options, &error);
		printf("appinst: install %s: %s %s\n", argv[1], ok ? "done" : "FAILED", error.description.UTF8String ?: "");
		return ok ? 0 : 1;
	}
}
