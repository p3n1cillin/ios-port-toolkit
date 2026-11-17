/*
 * r5q twin — EL2 entry, vectors, and the EL1 firmware-stand-in trampolines. EMULATION ONLY.
 * QEMU starts the CPU here at EL2 (-device loader,cpu-num=0). el2_main() (twin.c) builds stage 2,
 * the uH-like HCR_EL2, the phone's RAM log ring, and prepares EL1 to enter our real BootShim
 * exactly as ABL leaves it; we then ERET with every GPR zero.
 */
	.section .text.boot
	.global _start
_start:
	mrs	x0, CurrentEL
	cmp	x0, #(3 << 2)
	b.ne	el2_start
	/* QEMU virt,secure=on: we start at EL3, where Qualcomm's TrustZone sits on the phone */
	ldr	x0, =el3_vectors
	msr	vbar_el3, x0
	ldr	x0, =el3_stacks
	add	x0, x0, #0x4000
	mov	sp, x0
	bl	el3_main
	bl	el3_cpu_regs
	ldr	x0, =el2_start
	msr	elr_el3, x0
	mov	x0, #0x3c9                      /* EL2h, DAIF masked */
	msr	spsr_el3, x0
	eret

/* a core released by PSCI CPU_ON (from the boot ROM pen or from CPU_OFF): EL3 -> EL2 -> kernel entry at EL1 */
	.global	secondary_el3
secondary_el3:
	mrs	x19, mpidr_el1
	and	x19, x19, #0xff
	ldr	x0, =el3_vectors
	msr	vbar_el3, x0
	ldr	x0, =el3_stacks
	add	x1, x19, #1
	add	x0, x0, x1, lsl #14
	mov	sp, x0
	mov	x0, x19
	bl	el3_secondary
	ldr	x0, =el2_secondary
	msr	elr_el3, x0
	mov	x0, #0x3c9
	msr	spsr_el3, x0
	mov	x0, x19
	eret
el2_secondary:
	mov	x19, x0
	ldr	x1, =el2_vectors
	msr	vbar_el2, x1
	ldr	x1, =el2_sec_stacks
	add	x2, x19, #1
	add	x1, x1, x2, lsl #15
	mov	sp, x1
	bl	el2_secondary_main               /* returns the PSCI context id */
	.irp	r, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	mov	x\r, xzr
	.endr
	eret

el2_start:
	ldr	x0, =el2_vectors
	msr	vbar_el2, x0
	ldr	x0, =el2_stack_top
	mov	sp, x0
	bl	el2_main
	.irp	r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	mov	x\r, xzr
	.endr
	eret

	.macro	save
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	.endm
	.macro	restore
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #256
	.endm

lower_sync:
	save
	mov	x0, sp
	bl	el2_sync                    /* may edit the saved registers, ELR_EL2 and SPSR_EL2 */
	restore
	eret
lower_serror:
	save
	mov	x0, sp
	bl	el2_serror
1:	wfe
	b	1b
self_fault:
	save
	mov	x0, sp
	bl	el2_self_fault
2:	wfe
	b	2b

	.balign	0x800
el2_vectors:
	.rept	8
	b	self_fault
	.balign	0x80
	.endr
	b	lower_sync
	.balign	0x80
	b	self_fault
	.balign	0x80
	b	self_fault
	.balign	0x80
	b	lower_serror
	.balign	0x80
	.rept	4
	b	self_fault
	.balign	0x80
	.endr

/* ---- EL3 vectors: SMCs (PSCI) from EL1/EL2; anything else is reported and parks ----- */
el3_lower_sync:
	save
	mov	x0, sp
	bl	el3_sync
	restore
	eret
el3_other:
	mrs	x0, esr_el3
	mrs	x1, elr_el3
	bl	el3_fault
5:	wfe
	b	5b

	.balign	0x800
el3_vectors:
	.rept	8
	b	el3_other
	.balign	0x80
	.endr
	b	el3_lower_sync
	.balign	0x80
	.rept	7
	b	el3_other
	.balign	0x80
	.endr

	.bss
	.balign	16
	.global	el3_stacks
el3_stacks:	.space	0x4000 * 9
el2_sec_stacks:	.space	0x8000 * 9
	.text

/* ---- EL1: the firmware stand-in (what the r5q Mu UEFI does for our loader) ---------- */
	.global	fw_entry
fw_entry:
	ldr	x0, =el1_stack_top
	mov	sp, x0
	ldr	x0, =fw_vectors
	msr	vbar_el1, x0
	isb
	bl	fw_main
3:	wfe
	b	3b

	.macro	fwvec n
	.balign	0x80
	mov	x0, #\n
	b	fw_vec_common
	.endm
	.balign	0x800
	.global	fw_vectors
fw_vectors:
	fwvec 0
	fwvec 1
	fwvec 2
	fwvec 3
	fwvec 4
	fwvec 5
	fwvec 6
	fwvec 7
	fwvec 8
	fwvec 9
	fwvec 10
	fwvec 11
	fwvec 12
	fwvec 13
	fwvec 14
	fwvec 15
fw_vec_common:
	ldr	x1, =el1_stack_top
	sub	x1, x1, #0x1000              /* a known-good stack even if the faulting code broke SP */
	mov	sp, x1
	bl	fw_exception
4:	wfe
	b	4b
