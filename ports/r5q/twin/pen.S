/*
 * r5q twin boot ROM (secure flash at 0x0, QEMU -bios). EMULATION ONLY.
 * With virt,secure=on and a boot ROM, QEMU has no PSCI of its own and starts every core here at EL3.
 * Core 0 is redirected to the twin's _start by QEMU's loader device (cpu-num=0); every other core waits
 * in this pen until the EL3 stand-in's PSCI CPU_ON writes an address into its mailbox slot.
 */
	.text
	.global	_start
_start:
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	ldr	x1, =0x40100000                 /* MAILBOX in twin.c: one u64 per core */
	add	x1, x1, x0, lsl #3
1:	wfe
	ldr	x2, [x1]
	cbz	x2, 1b
	br	x2
	.ltorg
