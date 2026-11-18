/* openurl URL: open a URL the way a tap on a link does (LSApplicationWorkspace, as uiopen does): a test helper over r5qd */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			printf("usage: openurl URL\n");
			return 1;
		}
		NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:argv[1]]];
		Class ws = objc_getClass("LSApplicationWorkspace");
		id w = [ws performSelector:@selector(defaultWorkspace)];
		BOOL ok = NO;

		if (strncmp(argv[1], "app:", 4) == 0) {         /* openurl app:BUNDLE_ID: that application brought up */
			NSString *bundle = [NSString stringWithUTF8String:argv[1] + 4];
			ok = (BOOL)(intptr_t)[w performSelector:@selector(openApplicationWithBundleID:) withObject:bundle];
			printf("openurl: %s: %s\n", argv[1], ok ? "opened" : "NOT opened");
			return ok ? 0 : 1;
		}

		if ([w respondsToSelector:@selector(openURL:withOptions:error:)]) {
			NSError *__autoreleasing error = nil;
			NSMethodSignature *sig = [w methodSignatureForSelector:@selector(openURL:withOptions:error:)];
			NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
			NSDictionary *opts = @{};
			NSError *__autoreleasing *ep = &error;
			[inv setSelector:@selector(openURL:withOptions:error:)];
			[inv setTarget:w];
			[inv setArgument:&url atIndex:2];
			[inv setArgument:&opts atIndex:3];
			[inv setArgument:&ep atIndex:4];
			[inv invoke];
			[inv getReturnValue:&ok];
			printf("openurl: %s: %s %s\n", argv[1], ok ? "opened" : "NOT opened", error.description.UTF8String ?: "");
		} else {
			ok = (BOOL)(intptr_t)[w performSelector:@selector(openURL:) withObject:url];
			printf("openurl: %s: %s\n", argv[1], ok ? "opened" : "NOT opened");
		}
		return ok ? 0 : 1;
	}
}
