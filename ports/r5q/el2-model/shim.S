/*
 * EL2 stand-in for the S10 Lite's Samsung uH hypervisor (EMULATION ONLY, QEMU virt with
 * virtualization=on). It reproduces the EL1-visible configuration found by static analysis of the
 * stock hyp.mbn (research/r5q-hypervisor-watchdog.md, uH section):
 *   HCR_EL2 = RW | VM (stage 2) | PTW, optionally | TVM; never DC. CPTR_EL2 = 0x33ff, CNTHCTL_EL2 = 3,
 *   ICC_SRE_EL2 = 9, MDCR_EL2 with bits 5..11 cleared, HSTR_EL2 = 0.
 * Stage 2: identity map, 0-1 GiB device (nGnRE), 1-4 GiB normal write-back inner-shareable.
 * TVM traps are passed through (the write is performed) and the interesting ones are printed on the
 * PL011 UART; HVC is forwarded to QEMU's PSCI by SMC. It does NOT model uH's own kernel-protection
 * policy (KPP/HypX), only the architectural trap and translation configuration.
 * Build-time: -DHCR_VALUE=... ; linked at 0xFC000000 (top 64 MiB of a 3 GiB guest, hidden from UEFI by DT).
 */
#ifndef HCR_VALUE
#define HCR_VALUE 0x84000005        /* RW | TVM | PTW | VM */
#endif
#define UART 0x09000000

	.section .text
	.global _start
_start:
	mov	x19, x0                     /* QEMU's boot registers (x0 = device tree) go to UEFI unchanged */
	mov	x20, x1
	mov	x21, x2
	mov	x22, x3
	ldr	x0, =el2_vectors
	msr	vbar_el2, x0
	ldr	x0, =stack_top
	mov	sp, x0
	adr	x0, msg_banner
	bl	puts
	ldr	x0, =HCR_VALUE
	bl	puthex
	adr	x0, msg_nl
	bl	puts

	/*
	 * Stage 2. First GiB through a level-2 table of 2 MiB blocks: 0-128 MiB (QEMU's flash, where
	 * UEFI runs and keeps its first page tables; RAM-resident firmware on the phone) normal
	 * write-back, the rest device nGnRE. Then 1-4 GiB as 1 GiB normal blocks.
	 */
	ldr	x6, =s2_l2
	mov	x3, #0
0:	lsl	x4, x3, #21
	cmp	x3, #64
	mov	x5, #0x7fd                  /* normal WB, S2AP RW, inner shareable, AF, block */
	mov	x7, #0x4c5                  /* device nGnRE, S2AP RW, AF, block */
	csel	x5, x5, x7, lo
	orr	x4, x4, x5
	str	x4, [x6, x3, lsl #3]
	add	x3, x3, #1
	cmp	x3, #512
	b.lo	0b
	mov	x3, #0
10:	dc	civac, x6
	add	x6, x6, #64
	add	x3, x3, #1
	cmp	x3, #64
	b.lo	10b
	ldr	x1, =s2_l1
	ldr	x2, =s2_l2
	orr	x2, x2, #3                  /* table descriptor */
	str	x2, [x1]
	mov	x3, #1
1:	lsl	x4, x3, #30
	mov	x5, #0x7fd                  /* normal WB, S2AP RW, inner shareable, AF, block */
	orr	x4, x4, x5
	str	x4, [x1, x3, lsl #3]
	add	x3, x3, #1
	cmp	x3, #4
	b.lo	1b
	dc	civac, x1
	dsb	sy
	/* VTCR: RES1(31) | PS 32-bit | TG0 4K | SH0 inner | ORGN0/IRGN0 WB | SL0=1 (start level 1) | T0SZ=32 */
	ldr	x0, =0x80003560
	msr	vtcr_el2, x0
	msr	vttbr_el2, x1

	mov	x0, #3
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	msr	hstr_el2, xzr
	mov	x0, #0x33ff
	msr	cptr_el2, x0
	mrs	x0, mdcr_el2
	mov	x1, #0xfe0
	bic	x0, x0, x1
	msr	mdcr_el2, x0
	mov	x0, #9
	msr	icc_sre_el2, x0
	isb
	ldr	x0, =0x30d00800             /* SCTLR_EL1: RES1 bits, MMU/caches off */
	msr	sctlr_el1, x0
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0
	tlbi	alle1
	dsb	sy
	isb
	ldr	x0, =HCR_VALUE
	msr	hcr_el2, x0
	isb
	adr	x0, msg_go
	bl	puts
	mov	x0, #0x3c5                  /* EL1h, DAIF masked */
	msr	spsr_el2, x0
	msr	elr_el2, xzr                /* UEFI reset vector in flash */
	mov	x0, x19                     /* QEMU's reset x0; every other register zero, as after a reset */
	.irp	r, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
	mov	x\r, xzr
	.endr
	eret

/* --- UART helpers (EL2 runs with its MMU off: device accesses) --------------------- */
putc:	ldr	x9, =UART
2:	ldr	w10, [x9, #0x18]
	tbnz	w10, #5, 2b
	strb	w0, [x9]
	ret
puts:	mov	x11, x30
	mov	x12, x0
3:	ldrb	w0, [x12], #1
	cbz	w0, 4f
	bl	putc
	b	3b
4:	mov	x30, x11
	ret
puthex:	mov	x11, x30
	mov	x12, x0
	mov	x13, #60
5:	lsr	x0, x12, x13
	and	x0, x0, #0xf
	cmp	x0, #10
	add	x14, x0, #'0'
	add	x15, x0, #('a' - 10)
	csel	x0, x14, x15, lo
	bl	putc
	subs	x13, x13, #4
	b.pl	5b
	mov	x30, x11
	ret

/* --- exception handling ---------------------------------------------------------- */
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
	mrs	x20, esr_el2
	lsr	x21, x20, #26
	cmp	x21, #0x18
	b.eq	sysreg_trap
	cmp	x21, #0x16
	b.eq	hvc_call
	b	unexpected

hvc_call:                               /* forward to QEMU's PSCI: x0-x3 in, x0-x3 out */
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	smc	#0
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	restore
	eret

sysreg_trap:
	tbnz	x20, #0, unexpected         /* TVM traps only writes */
	ubfx	x22, x20, #5, #5            /* Rt */
	mov	x23, xzr
	cmp	x22, #31
	b.eq	6f
	ldr	x23, [sp, x22, lsl #3]
6:	ldr	x24, =0x3ffc1e
	and	x24, x20, x24
	ldr	x25, =trap_count
	ldr	x26, [x25]
	add	x26, x26, #1
	str	x26, [x25]
	.macro	emul key, reg, name, show
	ldr	x26, =\key
	cmp	x24, x26
	b.ne	7f
	msr	\reg, x23
	.if \show
	adr	x0, \name
	bl	puts
	mov	x0, x23
	bl	puthex
	adr	x0, msg_at
	bl	puts
	mrs	x0, elr_el2
	bl	puthex
	adr	x0, msg_nl
	bl	puts
	.endif
	b	trap_done
7:
	.endm
	emul	0x300400, sctlr_el1, n_sctlr, 1
	emul	0x300800, ttbr0_el1, n_ttbr0, 1
	emul	0x320800, ttbr1_el1, n_ttbr1, 1
	emul	0x340800, tcr_el1, n_tcr, 1
	emul	0x302804, mair_el1, n_mair, 1
	emul	0x302806, amair_el1, n_amair, 0
	emul	0x323400, contextidr_el1, n_ctx, 0
	emul	0x301404, esr_el1, n_esr, 0
	emul	0x301402, afsr0_el1, n_afsr, 0
	emul	0x321402, afsr1_el1, n_afsr, 0
	emul	0x301800, far_el1, n_far, 0
	b	unexpected
trap_done:
	isb
	mrs	x0, elr_el2
	add	x0, x0, #4
	msr	elr_el2, x0
	restore
	eret

unexpected:
	adr	x0, msg_unexp
	bl	puts
	mov	x0, x20
	bl	puthex
	adr	x0, msg_elr
	bl	puts
	mrs	x0, elr_el2
	bl	puthex
	adr	x0, msg_far
	bl	puts
	mrs	x0, far_el2
	bl	puthex
	adr	x0, msg_hpfar
	bl	puts
	mrs	x0, hpfar_el2
	bl	puthex
	adr	x0, msg_nl
	bl	puts
8:	wfe
	b	8b

self_fault:
	adr	x0, msg_self
	bl	puts
	mrs	x0, esr_el2
	bl	puthex
	adr	x0, msg_elr
	bl	puts
	mrs	x0, elr_el2
	bl	puthex
	adr	x0, msg_nl
	bl	puts
9:	wfe
	b	9b

lower_serror:
	save
	mrs	x20, esr_el2
	adr	x0, msg_serror
	bl	puts
	b	unexpected

	.balign	0x800
el2_vectors:
	.rept	8                           /* current EL, SP0 and SPx: our own faults */
	b	self_fault
	.balign	0x80
	.endr
	b	lower_sync                  /* lower EL, AArch64: sync */
	.balign	0x80
	b	self_fault                  /* IRQ (not routed here: IMO=0) */
	.balign	0x80
	b	self_fault                  /* FIQ */
	.balign	0x80
	b	lower_serror
	.balign	0x80
	.rept	4                           /* lower EL, AArch32 */
	b	self_fault
	.balign	0x80
	.endr

msg_banner:	.asciz "\n[EL2] uH stand-in: HCR_EL2="
msg_go:		.asciz "[EL2] stage 2 on; entering UEFI at EL1\n"
msg_nl:		.asciz "\n"
msg_at:		.asciz " at "
msg_unexp:	.asciz "[EL2] UNEXPECTED trap ESR="
msg_serror:	.asciz "[EL2] SError from EL1\n"
msg_self:	.asciz "[EL2] EL2 SELF FAULT ESR="
msg_elr:	.asciz " ELR="
msg_far:	.asciz " FAR="
msg_hpfar:	.asciz " HPFAR="
n_sctlr:	.asciz "[EL2] TVM SCTLR_EL1="
n_ttbr0:	.asciz "[EL2] TVM TTBR0_EL1="
n_ttbr1:	.asciz "[EL2] TVM TTBR1_EL1="
n_tcr:		.asciz "[EL2] TVM TCR_EL1="
n_mair:		.asciz "[EL2] TVM MAIR_EL1="
n_amair:	.asciz "a"
n_ctx:		.asciz "c"
n_esr:		.asciz "e"
n_afsr:		.asciz "f"
n_far:		.asciz "r"
	.ltorg

	.section .data
	.balign	8
trap_count:	.quad 0
	.section .bss
	.balign	4096
s2_l1:		.skip 4096
s2_l2:		.skip 4096
		.skip 65536
stack_top:
