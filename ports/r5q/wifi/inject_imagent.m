/*
 * libr5q-imagent.dylib (2026-11-20, Test 401): into imagent. Messages' icon carried a red "!" at every boot with no
 * messages at all. imagent's IMDBadgeUtilities posts the string "!" when -_shouldShowFailureString, which is
 * `_showingFailure || isUnexpectedlyLoggedOut` (IMDaemonCore+0x175844, iOS 16.5); `isUnexpectedlyLoggedOut` is a
 * stored flag set from the iMessage account's login-status notifications (wifi/WIFI-PLAN.md, "Messages' red '!'
 * badge"). On a real iPhone that "!" means "iMessage was signed out: sign in again". On this port iMessage can never
 * sign in (no Apple ID sign-in, no device attestation), so the flag is set for good and the badge is noise.
 *
 * -[IMDBadgeUtilities isUnexpectedlyLoggedOut] answers NO. A real failure (`_showingFailure`, a message that did not
 * go out) still shows "!", and the unread count is untouched. iMessage's own setting is left as it is.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <unistd.h>

static int tries;

static BOOL
never(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return NO;
}

static void
hook(void *unused)
{
	Class c = objc_getClass("IMDBadgeUtilities");
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName("isUnexpectedlyLoggedOut")) : NULL;

	(void)unused;
	if (m == NULL) {
		/* (IMDaemonCore can come in after this library's constructor: look again, for up to a minute) */
		if (++tries < 240) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_global_queue(0, 0), NULL, hook);
		}
		return;
	}
	method_setImplementation(m, (IMP)never);
	int fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		const char line[] = "r5q-imagent: IMDBadgeUtilities isUnexpectedlyLoggedOut answers NO (no iMessage sign-in here)\n";
		(void)write(fd, line, sizeof(line) - 1);
		close(fd);
	}
}

__attribute__((constructor)) static void
start(void)
{
	hook(NULL);
}
