/*
 * r5q_reboot: the running iOS restarted from the phone itself (reboot(2), as root, over r5qd) -- the kernel goes back
 * to the loader as it does for `phone.py cycle`. For when the PC's USB link to the loader is stuck ("no link") and
 * nobody is there to replug the cable: the loader brings its USB up again when the phone restarts (2026-11-19).
 */
#include <stdio.h>
#include <unistd.h>

/* (the iOS SDK has no sys/reboot.h; the call is in libsystem_kernel all the same) */
#define RB_AUTOBOOT 0
int reboot(int howto);

int
main(void)
{
	printf("r5q_reboot: syncing, then reboot(RB_AUTOBOOT)\n");
	fflush(stdout);
	sync();
	sleep(1);
	reboot(RB_AUTOBOOT);
	perror("r5q_reboot: reboot");
	return 1;
}
