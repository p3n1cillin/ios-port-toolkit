/*
 * libr5q-clearbadge.dylib (2026-11-20, a one-off, injected with `r5qd_client.py reload` into MobileSMS): the app's own
 * icon badge set to 0 once it has started. Why: Messages' icon carried a red "!" with no messages and no chats in
 * sms.db (nothing failed); SpringBoard restored it at every boot from its saved application state ("Setting badge to !
 * [ old badge: ! ]"), a leftover of an earlier run, and nothing on this port recomputes it. An app may set only its own
 * badge, so this runs inside Messages. Said on the console.
 */
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <unistd.h>

static void
clear(void *unused)
{
	(void)unused;
	[UIApplication sharedApplication].applicationIconBadgeNumber = 0;
	int fd = open("/dev/console", O_WRONLY);
	if (fd >= 0) {
		const char line[] = "r5q-clearbadge: Messages' icon badge set to 0\n";
		(void)write(fd, line, sizeof(line) - 1);
		close(fd);
	}
}

__attribute__((constructor)) static void
start(void)
{
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, clear);
}
