/*
 * r5q twin — a QEMU "virtual S10 Lite" for the boot path from ABL's hand-off onwards. EMULATION ONLY.
 *
 * Reproduces, from reverse engineering and the phone's own logs:
 *  - EL2: Samsung uH's EL1-visible configuration (research/r5q-uh-tvm-decompile.md): HCR_EL2 =
 *    RW|VM|PTW|TVM, no DC; TVM writes re-executed (passed through) as uH's KPP handler does before RKP.
 *  - the phone's physical layout: DRAM from 0x80000000 (0x40000000.. in QEMU), framebuffer 0x9C400000
 *    1080x2400, Samsung RAM log ring 0xC1600000 ("LOGM", idx modulo 2097136, data at +16), FD at
 *    0x9FC00000, payload at 0xF0000000, reserved window 0xE0000000..0xF0000000.
 *  - ABL's hand-off state (Test 17's phone log): EL1, MMU/caches off, SCTLR_EL1 0x00c00808, kernel
 *    image at 0x80100000.
 *  - peripherals by trap-and-emulate: PS_HOLD 0x0C264000 (write = reset), APSS watchdog 0x17C10000
 *    (watchdog_v2.c layout; the runner enforces the bite), a minimal MDSS (the display pipe the shim
 *    probes for the framebuffer). Any other unknown device access is logged and reads as zero.
 *  - the r5q firmware (Mu UEFI + Test 12 FirstBootManager) by a stand-in providing exactly the UEFI
 *    services our loader uses (the set the Unicorn harnesses established), and the same ring lines.
 * NOT reproduced: CPU caches (QEMU memory is coherent), the real Qualcomm UEFI, TrustZone, the real uH.
 */
#include <stdint.h>
#include <stddef.h>

#define UART        0x09000000UL
#define RING        0xC1600000UL
#define RING_MAGIC  0x4D474F4Cu
#define RING_SIZE   2097136u
#define FB_BASE     0x9C400000UL
#define FD_BASE     0x9FC00000UL
#define PAYLOAD     0xF0000000UL
#define KERNEL_AT   0x80100000UL
#define LOADER_AT   0x9B4DC000UL       /* where the phone's firmware put the loader (Test 13 log) */
#define PSHOLD      0x0C264000UL
#define WDT         0x17C10000UL
#define MDSS        0x0AE00000UL
#ifndef MMIO_LOG_LIMIT
#define MMIO_LOG_LIMIT 96
#endif

#define SYS_W(reg, v) __asm__ volatile ("msr " #reg ", %0" :: "r" ((uint64_t)(v)) : "memory")
#define SYS_R(reg) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #reg : "=r" (_v)); _v; })

/* volatile so the compiler cannot turn these loops back into calls to themselves */
void *memset(void *d, int c, size_t n) { volatile uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { volatile uint8_t *p = d; const volatile uint8_t *q = s; while (n--) *p++ = *q++; return d; }

/* ---- UART (device-mapped in both stages) ------------------------------------------------- */
static void putc_(char c) { volatile uint32_t *u = (volatile uint32_t *)UART; while (u[6] & (1u << 5)) {} u[0] = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(uint64_t v, int digits) { for (int i = digits - 1; i >= 0; i--) putc_("0123456789abcdef"[(v >> (4 * i)) & 15]); }
static void dec(uint64_t v) { char b[24]; int n = 0; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }

/* ---- the Samsung RAM log ring (what last_kmsg shows after a reboot) ------------------------- */
static void ring_init(void)
{
	volatile uint32_t *h = (volatile uint32_t *)RING;
	if (h[1] == RING_MAGIC) {
		/* the runner preloaded the phone's own ring (contents and wrapped index) */
		puts_("[TWIN] RAM log ring preloaded from the phone: idx=0x"); hex(h[2], 8); puts_("\n");
		return;
	}
	h[0] = 1; h[1] = RING_MAGIC; h[2] = 0; h[3] = 0;
	memset((void *)(RING + 16), 0, 4096);
}
static void ring_log(const char *s)
{
	volatile uint32_t *h = (volatile uint32_t *)RING;
	volatile uint8_t *b = (volatile uint8_t *)(RING + 16);
	if (h[1] != RING_MAGIC) return;
	uint32_t i = h[2];
	while (*s) b[(i++) % RING_SIZE] = (uint8_t)*s++;
	h[2] = i;
}

/* =========================================================================================== */
/* EL2: the uH stand-in                                                                        */
/* =========================================================================================== */
static uint64_t s2_l1[512] __attribute__((aligned(4096)));
static uint64_t s2_dev[512] __attribute__((aligned(4096)));    /* 0x00000000-0x3FFFFFFF */
static uint64_t s2_lo[512] __attribute__((aligned(4096)));     /* 0x40000000-0x7FFFFFFF */
static uint64_t s2_mid[512] __attribute__((aligned(4096)));    /* 0x80000000-0xBFFFFFFF */
#ifdef IOS_MODE
/*
 * iOS mode (run_twin.py ios): the production loader boots the source-built XNU and iOS 16.5 userland on
 * the twin. RAM as on the phone (live DT: 0x80000000.. and 0xC0000000-0x280000000), the r5q GIC-500 at its
 * own addresses (stage-2 aliases onto QEMU's GICv3), uH's real TVM behaviour (clear TVM, re-execute).
 */
#define IOS_RAM_END 0x280000000UL
#endif
static uint64_t s2_gic[512] __attribute__((aligned(4096)));    /* 0x17A00000-0x17BFFFFF: r5q GIC-500, every mode */
static int phase;                  /* 0: our BootShim runs (ABL hand-off), 1: firmware stand-in */
static uint32_t wdt_regs[8];
static uint64_t mmio_logged, mmio_total, tvm_count;
extern void fw_entry(void);

#define S2_NORMAL  (1UL | (0xFUL << 2) | (3UL << 6) | (3UL << 8) | (1UL << 10))   /* block, WB, RW, ISH, AF */
#define S2_DEVICE  (1UL | (0x1UL << 2) | (3UL << 6) | (1UL << 10))               /* block, nGnRE, RW, AF */
#define S2_XN      (1UL << 54)

uint64_t twin_loader_lo, twin_loader_hi;     /* the loader image, set by the firmware stand-in */

#ifdef CACHE_MODEL
/*
 * Approximate write-back data cache (EMULATION ONLY). While EL1 runs cacheable (SCTLR M and C set),
 * writes to the phone's DRAM range are tracked per 4 KiB page via stage-2 write protection; each
 * tracked page keeps a RAM view (what has actually reached the point of coherency). DC CVAC/CIVAC/IVAC
 * (trapped by HCR_EL2.TPC) update it per 64-byte line; DC set/way ops (HCR_EL2.TSW) are counted as a
 * whole-cache flush. A FIFO of at most CM_CAPACITY_PAGES dirty pages stands in for capacity eviction.
 * When caching goes off, uncached accesses see the RAM view (uncleaned writes are invisible); when it
 * comes back on, dirty data reappears and wins; at a reset, dirty data is lost.
 * Not modelled: stale CLEAN lines (only dirty ones), per-line write tracking (pages are coarse).
 */
#ifndef CM_CAPACITY_PAGES
#define CM_CAPACITY_PAGES 640           /* about 2.5 MiB: L1 + L2 + the DSU L3 of a Kryo 485 Gold core */
#endif
#define CM_POOL   0x40000000UL          /* EL2-private RAM: the phone layout never uses it; stage 2 hides it */
#define CM_L3     (CM_POOL)                         /* 1024 level-3 tables, 4 MiB */
#define CM_META   ((volatile uint32_t *)(CM_POOL + 0x00800000UL))   /* per tracked page: slot + 1 */
#define CM_FIFO   ((volatile uint32_t *)(CM_POOL + 0x00C00000UL))   /* dirty pages, oldest first */
#define CM_NCRW   ((volatile uint32_t *)(CM_POOL + 0x00D00000UL))   /* pages opened while uncached */
#define CM_FREE   ((volatile uint32_t *)(CM_POOL + 0x00E00000UL))   /* free slot stack */
#define CM_DATA   (CM_POOL + 0x01000000UL)          /* per slot: 4 KiB RAM view, 4 KiB cached view */
#define CM_SLOTS  16384
#define CM_LO     0x80000000UL
#define CM_HI     0xFC000000UL                      /* the twin's own memory above is not modelled */
static uint64_t s2_hi[512] __attribute__((aligned(4096)));
static int cm_cacheable;
static uint32_t cm_head, cm_tail, cm_ncrw_n, cm_free_n, cm_sw_ops;
static uint64_t cm_faults, cm_evictions;

static volatile uint64_t *cm_l3e(uint64_t pa)
{
	uint64_t gb = (pa - CM_LO) >> 30, l2 = (pa >> 21) & 511;
	return (volatile uint64_t *)(CM_L3 + (gb * 512 + l2) * 4096) + ((pa >> 12) & 511);
}
static void cm_ap(uint64_t page, int rw)
{
	volatile uint64_t *e = cm_l3e(page);
	*e = (*e & ~(3UL << 6)) | (rw ? (3UL << 6) : (1UL << 6));
	__asm__ volatile ("dsb ish; tlbi ipas2e1is, %0; dsb ish; tlbi vmalle1is; dsb ish; isb" :: "r" (page >> 12) : "memory");
}
static uint64_t cm_ram(uint32_t slot) { return CM_DATA + (uint64_t)slot * 8192; }
static uint64_t cm_cv(uint32_t slot) { return CM_DATA + (uint64_t)slot * 8192 + 4096; }
static void cp64(uint64_t d, uint64_t s, uint64_t n) { for (uint64_t i = 0; i < n; i += 8) *(volatile uint64_t *)(d + i) = *(volatile uint64_t *)(s + i); }
static int eq64(uint64_t a, uint64_t b) { for (int i = 0; i < 64; i += 8) if (*(volatile uint64_t *)(a + i) != *(volatile uint64_t *)(b + i)) return 0; return 1; }
static uint32_t cm_count(void) { return (cm_tail - cm_head) & (CM_SLOTS - 1); }
static int cm_tracked(uint64_t pa)
{
	/* the real r5q firmware maps these regions without write-back caching (Test 12 MemoryMapLib.c):
	 * "SEC Debug" 0xC1400000 (the RAM log ring) uncached; "Display Reserved" 0x9C400000 write-through */
	if (pa >= 0xC1400000UL && pa < 0xC1C00000UL) return 0;
	if (pa >= 0x9C400000UL && pa < 0x9E400000UL) return 0;
	return pa >= CM_LO && pa < CM_HI;
}

static void cm_build(void)
{
	for (uint64_t gb = 0; gb < 2; gb++) {
		uint64_t *l2 = gb ? s2_hi : s2_mid;
		for (uint64_t i = 0; i < 512; i++) {
			uint64_t t = CM_L3 + (gb * 512 + i) * 4096;
			l2[i] = t | 3;
			for (uint64_t j = 0; j < 512; j++) {
				uint64_t pa = CM_LO + (gb << 30) + (i << 21) + (j << 12);
				uint64_t d = pa | 3 | (0xFUL << 2) | (3UL << 8) | (1UL << 10) | (cm_tracked(pa) ? (1UL << 6) : (3UL << 6));
#ifndef REAL_FD
				if (pa >= FD_BASE && pa < FD_BASE + 0x300000UL) d |= S2_XN;
#endif
				*(volatile uint64_t *)(t + j * 8) = d;
			}
		}
	}
	for (uint64_t i = 0; i < 512; i++) s2_lo[i] = 0;          /* the pool is invisible to EL1 */
	for (uint64_t i = 0; i < (CM_HI - CM_LO) >> 12; i++) CM_META[i] = 0;
	for (uint32_t k = 0; k < CM_SLOTS; k++) CM_FREE[k] = CM_SLOTS - 1 - k;
	cm_free_n = CM_SLOTS;
	puts_("[TWIN-CACHE] write-back cache model on: capacity "); dec(CM_CAPACITY_PAGES); puts_(" pages (4 KiB)\n");
}
static void cm_evict(void)
{
	uint32_t p = CM_FIFO[cm_head];
	cm_head = (cm_head + 1) & (CM_SLOTS - 1);
	uint32_t s = CM_META[p];
	if (!s) return;
	/* written back: RAM now holds the cached data, which is what memory already contains */
	CM_FREE[cm_free_n++] = s - 1;
	CM_META[p] = 0;
	cm_ap(CM_LO + ((uint64_t)p << 12), 0);
	cm_evictions++;
}
static void cm_write_fault(uint64_t ipa)
{
	uint64_t page = ipa & ~0xfffUL;
	uint32_t p = (uint32_t)((page - CM_LO) >> 12);
	cm_faults++;
	if (!cm_cacheable) {                        /* uncached write: goes straight to RAM */
		cm_ap(page, 1);
		if (cm_ncrw_n < 0x40000) CM_NCRW[cm_ncrw_n++] = p;
		return;
	}
	if (!CM_META[p]) {
		while (cm_count() >= CM_CAPACITY_PAGES || cm_free_n == 0) cm_evict();
		uint32_t s = CM_FREE[--cm_free_n];
		cp64(cm_ram(s), page, 4096);            /* the RAM view: the page as it is in memory now */
		CM_META[p] = s + 1;
		CM_FIFO[cm_tail] = p;
		cm_tail = (cm_tail + 1) & (CM_SLOTS - 1);
	}
	cm_ap(page, 1);
}
static void cm_dc(uint32_t key, uint64_t va)
{
	uint64_t pa = va & ~63UL;                    /* the code under test runs identity-mapped */
	if (key == 0x141C0C || key == 0x141C14 || key == 0x141C1C) { cm_sw_ops++; return; }   /* set/way */
	if (!cm_tracked(pa)) return;
	uint32_t s = CM_META[(pa - CM_LO) >> 12];
	if (!s) return;
	uint64_t off = pa & 0xfff;
	if (key == 0x12DC14 || key == 0x12DC1C) cp64(cm_ram(s - 1) + off, pa, 64);          /* DC CVAC / CIVAC */
	else if (key == 0x121C0C && cm_cacheable) cp64(pa, cm_ram(s - 1) + off, 64);        /* DC IVAC: discard */
}
static void cm_report_line(uint64_t *open_lo, uint64_t *open_hi, int *shown, uint64_t line)
{
	if (*open_hi == line) { *open_hi = line + 64; return; }
	if (*open_hi && *shown < 16) {
		puts_("[TWIN-CACHE]   0x"); hex(*open_lo, 9); puts_("-0x"); hex(*open_hi, 9);
		puts_(" ("); dec(*open_hi - *open_lo); puts_(" bytes)");
		if (*open_lo >= twin_loader_lo && *open_lo < twin_loader_hi) { puts_(" loader image +0x"); hex(*open_lo - twin_loader_lo, 5); }
		else if (*open_lo >= RING && *open_lo < RING + 0x200000) puts_(" Samsung RAM log");
		puts_("\n");
		(*shown)++;
	}
	*open_lo = line; *open_hi = line + 64;
}
static void cm_off(void)
{
	uint64_t lines = 0, lo = 0, hi = 0;
	int shown = 0;
	if (cm_sw_ops >= 1024) {
		puts_("[TWIN-CACHE] set/way clean+invalidate of the whole cache seen ("); dec(cm_sw_ops);
		puts_(" ops): every dirty line is now in RAM\n");
		for (uint32_t i = cm_head; i != cm_tail; i = (i + 1) & (CM_SLOTS - 1)) {
			uint32_t p = CM_FIFO[i], s = CM_META[p];
			if (s) cp64(cm_ram(s - 1), CM_LO + ((uint64_t)p << 12), 4096);
		}
	}
	cm_sw_ops = 0;
	puts_("[TWIN-CACHE] caches OFF: writes that never reached RAM are now invisible:\n");
	for (uint32_t i = cm_head; i != cm_tail; i = (i + 1) & (CM_SLOTS - 1)) {
		uint32_t p = CM_FIFO[i], s = CM_META[p];
		if (!s) continue;
		uint64_t page = CM_LO + ((uint64_t)p << 12);
		cp64(cm_cv(s - 1), page, 4096);
		for (uint64_t o = 0; o < 4096; o += 64)
			if (!eq64(page + o, cm_ram(s - 1) + o)) { lines++; cm_report_line(&lo, &hi, &shown, page + o); }
		cp64(page, cm_ram(s - 1), 4096);
	}
	cm_report_line(&lo, &hi, &shown, ~0UL);
	puts_("[TWIN-CACHE]   total: "); dec(lines); puts_(" dirty lines ("); dec(lines * 64); puts_(" bytes) not in RAM; ");
	dec(cm_count()); puts_(" pages tracked, "); dec(cm_evictions); puts_(" evictions so far\n");
}
static void cm_on(void)
{
	uint64_t back = 0;
	for (uint32_t i = cm_head; i != cm_tail; i = (i + 1) & (CM_SLOTS - 1)) {
		uint32_t p = CM_FIFO[i], s = CM_META[p];
		if (!s) continue;
		uint64_t page = CM_LO + ((uint64_t)p << 12);
		for (uint64_t o = 0; o < 4096; o += 64) {
			uint64_t ram = page + o, rv = cm_ram(s - 1) + o, cv = cm_cv(s - 1) + o;
			int dirty = !eq64(cv, rv);
			uint8_t tmp[64];
			for (int b = 0; b < 64; b++) tmp[b] = *(volatile uint8_t *)(ram + b);
			if (dirty) { cp64(ram, cv, 64); back++; }
			for (int b = 0; b < 64; b++) *(volatile uint8_t *)(rv + b) = tmp[b];
		}
	}
	for (uint32_t k = 0; k < cm_ncrw_n; k++) if (!CM_META[CM_NCRW[k]]) cm_ap(CM_LO + ((uint64_t)CM_NCRW[k] << 12), 0);
	cm_ncrw_n = 0;
	puts_("[TWIN-CACHE] caches ON: "); dec(back); puts_(" dirty lines visible again (they win over RAM)\n");
}
static void cm_sctlr(uint64_t oldv, uint64_t newv)
{
	int was = (oldv & 5) == 5, now = (newv & 5) == 5;
	if (was && !now) cm_off();
	if (!was && now) cm_on();
	cm_cacheable = now;
}
static void cm_reset(void)
{
	if (!cm_cacheable) return;
	uint64_t lost = 0;
	for (uint32_t i = cm_head; i != cm_tail; i = (i + 1) & (CM_SLOTS - 1)) {
		uint32_t p = CM_FIFO[i], s = CM_META[p];
		if (!s) continue;
		uint64_t page = CM_LO + ((uint64_t)p << 12);
		for (uint64_t o = 0; o < 4096; o += 64) if (!eq64(page + o, cm_ram(s - 1) + o)) lost++;
		cp64(page, cm_ram(s - 1), 4096);
	}
	puts_("[TWIN-CACHE] reset with caches on: "); dec(lost); puts_(" dirty lines never reached RAM and are lost\n");
}
#endif

/* the per-core part of the uH stand-in: every core gets the same stage 2 and HCR_EL2 */
static void el2_cpu_regs(void)
{
#ifdef IOS_MODE
	SYS_W(vtcr_el2, 0x80013559UL);          /* RES1 | PS 36-bit | 4K | ISH | WB | SL0=1 | T0SZ=25 */
#else
	SYS_W(vtcr_el2, 0x80003560UL);          /* RES1 | PS 32-bit | 4K | ISH | WB | SL0=1 | T0SZ=32 */
#endif
	SYS_W(vttbr_el2, (uint64_t)(uintptr_t)s2_l1);
	SYS_W(cnthctl_el2, 3);
	SYS_W(cntvoff_el2, 0);
	SYS_W(hstr_el2, 0);
	SYS_W(cptr_el2, 0x33ff);
	SYS_W(mdcr_el2, SYS_R(mdcr_el2) & ~0xfe0UL);
	SYS_W(icc_sre_el2, 9);
	/* EL1 reads of MIDR_EL1 return VPIDR_EL2: report the phone's Kryo 485 Gold (Test 17 log: MIDR=51df805e).
	 * CNTFRQ stays QEMU's real 62.5 MHz on purpose: faking 19.2 MHz would skew every CNTVCT-timed wait. */
	SYS_W(vpidr_el2, 0x51df805eUL);
	SYS_W(vmpidr_el2, SYS_R(mpidr_el1));
	__asm__ volatile ("isb; tlbi alle1; dsb sy; isb" ::: "memory");
#ifdef CACHE_MODEL
	/* + TPC (DC CVAC/CIVAC/IVAC) and TSW (DC set/way) so the cache model sees every clean */
	SYS_W(hcr_el2, (1UL << 31) | (1UL << 26) | (1UL << 23) | (1UL << 22) | (1UL << 2) | 1UL);
#else
	SYS_W(hcr_el2, (1UL << 31) | (1UL << 26) | (1UL << 2) | 1UL);
#endif
}

#ifdef SECURE_EL3
/* =========================================================================================== */
/* EL3: the TrustZone stand-in (what Qualcomm's TZ does for the non-secure world)               */
/* =========================================================================================== */
#define GICD_PA     0x08000000UL          /* EL3 has no stage 2: QEMU's GIC at its own address */
#define GICR_PA     0x080A0000UL
#define MAILBOX     0x40100000UL          /* the boot ROM's secondary-core pen polls one u64 per core */
#define MAX_CPUS    8
volatile uint64_t psci_entry[MAX_CPUS], psci_ctx[MAX_CPUS], psci_on[MAX_CPUS];
extern void secondary_el3(void);
static uint64_t smc_unknown_logged, twin_cpus = 1;
static void park(const char *why);

void el3_fault(uint64_t esr, uint64_t elr)
{
	puts_("[TWIN] EL3 fault ESR_EL3=0x"); hex(esr, 8); puts_(" ELR_EL3=0x"); hex(elr, 10); puts_("\n[TWIN] PARKED\n");
}

static void el3_gicr_init(uint64_t cpu)
{
	volatile uint32_t *rd = (volatile uint32_t *)(GICR_PA + cpu * 0x20000UL), *sgi = rd + 0x10000 / 4;
	rd[0x14 / 4] &= ~2u;                         /* GICR_WAKER.ProcessorSleep: only Secure can (DS=0) */
	while (rd[0x14 / 4] & 4u) {}
	sgi[0x80 / 4] = ~(1u << 29);                 /* IGROUPR0: SGIs/PPIs Group 1 NS, secure timer PPI 29 Group 0 */
	sgi[0xD00 / 4] = 0;                          /* IGRPMODR0 */
}

void el3_main(void)
{
	volatile uint32_t *gicd = (volatile uint32_t *)GICD_PA;
	puts_("\n[TWIN] EL3 TrustZone stand-in: GIC security ON (DS=0), interrupts Group 1 NS, PSCI at EL3\n");
	gicd[0] = (1u << 4) | (1u << 5);             /* ARE_S | ARE_NS */
	while (gicd[0] & (1u << 31)) {}
	uint32_t lines = ((gicd[1] & 31) + 1) * 32;
	for (uint32_t i = 1; i < lines / 32; i++) { gicd[0x80 / 4 + i] = ~0u; gicd[0xD00 / 4 + i] = 0; }
	gicd[0] = (1u << 4) | (1u << 5) | 7u;        /* + EnableGrp0 | EnableGrp1NS | EnableGrp1S */
	while (gicd[0] & (1u << 31)) {}
	el3_gicr_init(0);
	/* how many cores this machine has: one redistributor frame each, the last one marked */
	twin_cpus = 1;
	while (twin_cpus < MAX_CPUS && !(*(volatile uint64_t *)(GICR_PA + (twin_cpus - 1) * 0x20000UL + 8) & (1UL << 4)))
		twin_cpus++;
	for (int i = 0; i < MAX_CPUS; i++) { psci_on[i] = 0; *(volatile uint64_t *)(MAILBOX + 8 * i) = 0; }
	psci_on[0] = 1;
	puts_("[TWIN] EL3: GICD_CTLR(S)=0x"); hex(gicd[0], 8); puts_(", "); dec(lines - 32); puts_(" SPIs -> Group 1 NS, ");
	dec(twin_cpus); puts_(" core(s); secondaries wait in the boot ROM pen for PSCI CPU_ON\n");
}
void el3_cpu_regs(void)
{
	SYS_W(S3_6_C12_C12_5, 0xf);                  /* ICC_SRE_EL3: SRE | DFB | DIB | Enable (lower ELs may use SRE) */
	__asm__ volatile ("isb" ::: "memory");
	SYS_W(cptr_el3, 0);
	SYS_W(mdcr_el3, 0);
	SYS_W(sctlr_el2, 0x30c50830UL);              /* EL2 starts with its MMU off, little-endian */
	SYS_W(scr_el3, (1UL << 10) | (1UL << 8) | 1UL);   /* RW (lower ELs AArch64) | HCE | NS; SMC enabled */
	__asm__ volatile ("isb" ::: "memory");
}
void el3_secondary(uint64_t cpu)
{
	el3_gicr_init(cpu);
	el3_cpu_regs();
}

#define PSCI_SUCCESS          0
#define PSCI_NOT_SUPPORTED   -1
#define PSCI_INVALID_PARAMS  -2
#define PSCI_ALREADY_ON      -4
static int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
	uint64_t cpu = mpidr & 0xff;             /* QEMU numbers cores in Aff0 (the phone: Aff1, see README) */
	if ((mpidr & 0xffff00) || cpu >= MAX_CPUS || cpu >= twin_cpus) return PSCI_INVALID_PARAMS;
	if (psci_on[cpu]) return PSCI_ALREADY_ON;
	psci_entry[cpu] = entry;
	psci_ctx[cpu] = ctx;
	psci_on[cpu] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint64_t *)(MAILBOX + 8 * cpu) = (uint64_t)(uintptr_t)secondary_el3;
	__asm__ volatile ("dsb sy; sev" ::: "memory");
	puts_("[TWIN] EL3 PSCI CPU_ON core "); dec(cpu); puts_(" entry 0x"); hex(entry, 10); puts_("\n");
	return PSCI_SUCCESS;
}

/* an SMC from EL1 (straight to EL3: HCR_EL2.TSC is clear, as on the phone) or from the EL2 stand-in */
void el3_sync(uint64_t *f)
{
	uint64_t esr = SYS_R(esr_el3), fid = f[0];
	uint32_t ec = (uint32_t)(esr >> 26) & 0x3f;
	if (ec != 0x17) {
		puts_("[TWIN] EL3: unexpected trap ESR_EL3=0x"); hex(esr, 8); puts_(" ELR_EL3=0x"); hex(SYS_R(elr_el3), 10);
		park("\n");
	}
	int64_t r = PSCI_NOT_SUPPORTED;
	switch (fid) {
	case 0x84000000: r = 0x10001; break;                                     /* PSCI_VERSION 1.1 */
	case 0x8400000A:                                                          /* PSCI_FEATURES */
		switch (f[1]) {
		case 0x84000000: case 0x84000002: case 0xC4000003: case 0x84000003: case 0xC4000004:
		case 0x84000004: case 0x84000006: case 0x84000008: case 0x84000009: case 0x8400000A: r = 0; break;
		}
		break;
	case 0xC4000003: case 0x84000003: r = psci_cpu_on(f[1], f[2], f[3]); break;   /* CPU_ON */
	case 0xC4000004: case 0x84000004: {                                       /* AFFINITY_INFO */
		uint64_t cpu = f[1] & 0xff;
		r = ((f[1] & 0xffff00) || cpu >= twin_cpus) ? PSCI_INVALID_PARAMS : (psci_on[cpu] ? 0 : 1);
		break;
	}
	case 0x84000006: r = 2; break;                                            /* MIGRATE_INFO_TYPE: none */
	case 0xC4000001: case 0x84000001: r = 0; break;                           /* CPU_SUSPEND: standby, back at once */
	case 0x84000002: {                                                        /* CPU_OFF */
		uint64_t cpu = SYS_R(mpidr_el1) & 0xff;
		puts_("[TWIN] EL3 PSCI CPU_OFF core "); dec(cpu); puts_("\n");
		psci_on[cpu] = 0;
		*(volatile uint64_t *)(MAILBOX + 8 * cpu) = 0;
		__asm__ volatile ("dsb sy" ::: "memory");
		for (;;) {                                                            /* back to the pen */
			__asm__ volatile ("wfe");
			uint64_t go = *(volatile uint64_t *)(MAILBOX + 8 * cpu);
			if (go) ((void (*)(void))(uintptr_t)go)();
		}
	}
	case 0x84000008:
		park("[TWIN] EL3 PSCI SYSTEM_OFF\n[TWIN] RESET\n");
	case 0x84000009:
		park("[TWIN] EL3 PSCI SYSTEM_RESET (on the phone our code must not use this: PS_HOLD instead)\n[TWIN] RESET\n");
	default:
		if (smc_unknown_logged++ < 16) { puts_("[TWIN] EL3: SMC 0x"); hex(fid, 8); puts_(" not supported (TZ would answer)\n"); }
	}
	f[0] = (uint64_t)r;
}
#else
/* without SECURE_EL3 QEMU starts the twin at EL2 and none of the EL3 entry points is reached */
void el3_main(void) {}
void el3_cpu_regs(void) {}
void el3_secondary(uint64_t cpu) { (void)cpu; }
void el3_sync(uint64_t *f) { (void)f; }
void el3_fault(uint64_t esr, uint64_t elr) { (void)esr; (void)elr; }
#endif

/* a core started by PSCI CPU_ON reaches EL2 here (from secondary_el3): same uH configuration, then its
 * entry point at EL1 with the MMU off; returns the context id for x0 */
uint64_t el2_secondary_main(uint64_t cpu)
{
#ifdef SECURE_EL3
	el2_cpu_regs();
	SYS_W(sctlr_el1, 0x30d00800UL);          /* MMU and caches off */
	SYS_W(spsr_el2, 0x3c5);
	SYS_W(elr_el2, psci_entry[cpu]);
	__asm__ volatile ("isb" ::: "memory");
	return psci_ctx[cpu];
#else
	(void)cpu;
	return 0;
#endif
}

void el2_main(void)
{
	puts_("\n[TWIN] r5q virtual twin (EMULATION ONLY): EL2 uH stand-in, HCR_EL2=RW|VM|PTW|TVM, no DC\n");
	for (int i = 0; i < 512; i++) {
		uint64_t pa = (uint64_t)i << 21;
		/* QEMU's own GIC/UART/RTC/fw_cfg (0x08000000-0x0A000000) pass through; every other device
		 * address traps to the device model below. */
		s2_dev[i] = (pa >= 0x08000000UL && pa < 0x0A000000UL) ? (pa | S2_DEVICE) : 0;
		s2_lo[i] = (0x40000000UL + pa) | S2_NORMAL;
		s2_mid[i] = (0x80000000UL + pa) | S2_NORMAL;
	}
	/* r5q's GIC-500 where the phone has it (GICD 0x17A00000 64 KiB, GICR 0x17A60000 8 x 128 KiB, from the
	 * firmware's MemoryMapLib and the live DT), backed by QEMU's GICv3 (0x08000000 / 0x080A0000). In every
	 * mode since Test 34: a kernel that runs past arm_init reaches for it (before, only iOS mode had it). */
	for (int i = 0; i < 512; i++) {
		uint64_t off = (uint64_t)i << 12, pa = 0;
		if (off < 0x10000) pa = 0x08000000UL + off;
		else if (off >= 0x60000 && off < 0x160000) pa = 0x080A0000UL + (off - 0x60000);
		s2_gic[i] = pa ? (pa | S2_DEVICE | 2) : 0;        /* level-3 page descriptor */
	}
	s2_dev[0x17A00000UL >> 21] = (uint64_t)(uintptr_t)s2_gic | 3;
#ifdef IOS_MODE
	/* twin-only: QEMU's virtio-mmio transports (a virtual touch panel and buttons for driving iOS); the
	 * r5q map has nothing in this 2 MiB block, and the kernel only looks there with virtio_input=1 */
	s2_dev[0x0A000000UL >> 21] = 0x0A000000UL | S2_DEVICE;
	puts_("[TWIN] iOS mode: r5q GIC-500 at 0x17A00000/0x17A60000 (QEMU GICv3 behind it), RAM to 0x280000000\n");
#endif
	/* the FD's 3 MiB (two 2 MiB blocks) is not executable: the shim's jump into SEC traps here */
#if !defined(REAL_FD) && !defined(IOS_MODE)
	s2_mid[(FD_BASE - 0x80000000UL) >> 21] |= S2_XN;
	s2_mid[(FD_BASE - 0x80000000UL + 0x200000UL) >> 21] |= S2_XN;
#elif defined(REAL_FD)
	puts_("[TWIN] REAL_FD mode: the real r5q Mu UEFI runs; unknown hardware reads as zero\n");
#endif
	s2_l1[0] = (uint64_t)(uintptr_t)s2_dev | 3;
	s2_l1[1] = (uint64_t)(uintptr_t)s2_lo | 3;
	s2_l1[2] = (uint64_t)(uintptr_t)s2_mid | 3;
	s2_l1[3] = 0xC0000000UL | S2_NORMAL;
#ifdef CACHE_MODEL
	cm_build();
	s2_l1[3] = (uint64_t)(uintptr_t)s2_hi | 3;
#endif
#ifdef IOS_MODE
	for (uint64_t g = 4; g < (IOS_RAM_END >> 30); g++) s2_l1[g] = (g << 30) | S2_NORMAL;
	__asm__ volatile ("dsb sy" ::: "memory");
	SYS_W(vtcr_el2, 0x80013559UL);          /* RES1 | PS 36-bit | 4K | ISH | WB | SL0=1 | T0SZ=25 */
#else
	__asm__ volatile ("dsb sy" ::: "memory");
	SYS_W(vtcr_el2, 0x80003560UL);          /* RES1 | PS 32-bit | 4K | ISH | WB | SL0=1 | T0SZ=32 */
#endif
	el2_cpu_regs();
	ring_init();
	SYS_W(sctlr_el1, 0x00c00808UL);         /* exactly what Test 17's shim logged on the phone */
	SYS_W(spsr_el2, 0x3c5);                 /* EL1h, DAIF masked */
#ifdef IOS_MODE
	/* the BootShim's part (copy the payload to 0xF0000000, enter SEC) is done by the runner: it was
	 * validated on the phone by Test 17. Straight to the firmware stand-in. */
	phase = 1;
	SYS_W(elr_el2, (uint64_t)(uintptr_t)fw_entry);
	__asm__ volatile ("isb" ::: "memory");
	puts_("[TWIN] ABL hand-off emulated (EL1, MMU/caches off); payload already at 0xF0000000; firmware stand-in next\n");
#else
	SYS_W(elr_el2, KERNEL_AT);
	__asm__ volatile ("isb" ::: "memory");
	puts_("[TWIN] ABL hand-off emulated: entering the BootShim at 0x80100000, EL1, MMU/caches off\n");
#endif
}

static void park(const char *why)
{
	puts_(why);
	puts_("[TWIN] PARKED\n");
	for (;;) __asm__ volatile ("wfi");
}

/* ---- device model ------------------------------------------------------------------------ */
static uint64_t mmio_read(uint64_t ipa, int size)
{
	if (ipa >= WDT && ipa < WDT + 0x1000) {
		uint64_t o = ipa - WDT;
		return o < 0x20 ? wdt_regs[o / 4] : 0;
	}
	if (ipa >= MDSS && ipa < MDSS + 0x100000) {
		uint64_t o = ipa - MDSS;
		/* pipe DMA0: the scanout the shim looks for (address, stride, size) */
		if (o == 0x25014) return FB_BASE;
		if (o == 0x25024) return 1080 * 4;
		if (o == 0x25000) return (2400u << 16) | 1080u;
		return 0;                       /* CTL_START idle, nothing claims INTF1 */
	}
	(void)size;
	return 0;
}
static void mmio_write(uint64_t ipa, uint64_t v, int size)
{
	if (ipa == PSHOLD) {
		puts_("[TWIN] PS_HOLD <- 0x"); hex(v, 8); puts_(": the phone resets (warm, RAM ring kept)\n");
#ifdef CACHE_MODEL
		cm_reset();
#endif
		park("[TWIN] RESET\n");
	}
	if (ipa >= WDT && ipa < WDT + 0x1000) {
		uint64_t o = ipa - WDT;
		if (o < 0x20) wdt_regs[o / 4] = (uint32_t)v;
		if (o == 0x08 && (v & 1)) {
			uint64_t ms = (uint64_t)wdt_regs[0x14 / 4] * 1000 / 32765;
			puts_("[TWIN] WDT armed bite_ms="); dec(ms); puts_(" (the runner enforces the bite)\n");
		} else if (o == 0x08) {
			puts_("[TWIN] WDT disabled\n");
		}
		return;
	}
	(void)size;
}
static void log_access(int write, uint64_t ipa, uint64_t v, int size, uint64_t elr)
{
	mmio_total++;
#ifdef IOS_MODE
	{
		/* which r5q hardware the kernel reaches for: the first access to each distinct page */
		static uint64_t seen[256];
		static int nseen;
		uint64_t pg = ipa >> 12;
		for (int i = 0; i < nseen; i++) if (seen[i] == pg) return;
		if (nseen < 256) seen[nseen++] = pg; else return;
	}
#else
	if (mmio_logged >= MMIO_LOG_LIMIT || ipa == PSHOLD || (ipa >= WDT && ipa < WDT + 0x1000)) return;
#endif
	mmio_logged++;
	puts_(write ? "[TWIN] MMIO W " : "[TWIN] MMIO R ");
	hex(ipa, 9); puts_(write ? " <- " : " -> "); hex(v, 8);
	puts_(" ("); dec((uint64_t)size); puts_("B) at "); hex(elr, 9);
	puts_(ipa >= MDSS && ipa < MDSS + 0x100000 ? " [MDSS]\n" : " [unknown device]\n");
}

/* ---- TVM: pass the EL1 write through, as uH does before RKP starts ------------------------- */
static int tvm_write(uint32_t key, uint64_t v, uint64_t elr)
{
	const char *name = 0;
	switch (key) {
	case 0x300400:
#ifdef CACHE_MODEL
		cm_sctlr(SYS_R(sctlr_el1), v);
#endif
		SYS_W(sctlr_el1, v); name = "SCTLR_EL1"; break;
	case 0x300800: SYS_W(ttbr0_el1, v); name = "TTBR0_EL1"; break;
	case 0x320800: SYS_W(ttbr1_el1, v); name = "TTBR1_EL1"; break;
	case 0x340800: SYS_W(tcr_el1, v); name = "TCR_EL1"; break;
	case 0x302804: SYS_W(mair_el1, v); name = "MAIR_EL1"; break;
	case 0x302806: SYS_W(amair_el1, v); break;
	case 0x323400: SYS_W(contextidr_el1, v); break;
	case 0x301404: SYS_W(esr_el1, v); break;
	case 0x301402: SYS_W(afsr0_el1, v); break;
	case 0x321402: SYS_W(afsr1_el1, v); break;
	case 0x301800: SYS_W(far_el1, v); break;
	default: return 0;
	}
	tvm_count++;
	if (name) { puts_("[TWIN] TVM "); puts_(name); puts_("="); hex(v, 16); puts_(" at "); hex(elr, 9); puts_("\n"); }
	return 1;
}

static void dump_state(const char *what, uint64_t *f)
{
	puts_("[TWIN] "); puts_(what);
	puts_(" ESR_EL2="); hex(SYS_R(esr_el2), 8);
	puts_(" ELR="); hex(SYS_R(elr_el2), 9);
	puts_(" FAR="); hex(SYS_R(far_el2), 9);
	puts_(" HPFAR="); hex(SYS_R(hpfar_el2), 9);
	puts_(" SCTLR_EL1="); hex(SYS_R(sctlr_el1), 8);
	puts_(" x30="); hex(f[30], 9); puts_("\n");
}

void el2_sync(uint64_t *f)
{
	uint64_t esr = SYS_R(esr_el2), elr = SYS_R(elr_el2);
	uint32_t ec = (uint32_t)(esr >> 26) & 0x3f, iss = (uint32_t)esr & 0x1ffffff;

#ifdef CACHE_MODEL
	if (ec == 0x18 && ((iss >> 20) & 3) == 1) {             /* trapped DC maintenance (TPC/TSW) */
		uint32_t rt = (iss >> 5) & 31;
		cm_dc(iss & 0x3ffc1e, rt == 31 ? 0 : f[rt]);
		SYS_W(elr_el2, elr + 4);
		return;
	}
	if (ec == 0x24) {                                       /* write to a tracked page */
		uint64_t ipa = ((SYS_R(hpfar_el2) >> 4) << 12) | (SYS_R(far_el2) & 0xfff);
		if (cm_tracked(ipa) && (iss & 0x3c) == 0x0c) { cm_write_fault(ipa); return; }
	}
#endif
#ifdef IOS_MODE
	if (ec == 0x18 && !(iss & 1)) {
		/* what uH's TVM handler (FUN_8576f6a8) does before RKP: clear HCR_EL2.TVM, rewind, re-execute */
		SYS_W(hcr_el2, SYS_R(hcr_el2) & ~(1UL << 26));
		__asm__ volatile ("isb" ::: "memory");
		if (!tvm_count++) {
			puts_("[TWIN] first TVM trap (sysreg key 0x"); hex(iss & 0x3ffc1e, 6); puts_(") at "); hex(elr, 9);
			puts_(": TVM cleared and the write re-executed, as uH does before RKP_START\n");
		}
		return;
	}
#endif
	if (ec == 0x18 && !(iss & 1)) {                         /* trapped MSR (TVM) */
		uint32_t rt = (iss >> 5) & 31;
		uint64_t v = rt == 31 ? 0 : f[rt];
		if (tvm_write(iss & 0x3ffc1e, v, elr)) { __asm__ volatile ("isb"); SYS_W(elr_el2, elr + 4); return; }
		dump_state("unhandled sysreg trap", f); park("");
	}
	if (ec == 0x20) {                                       /* instruction abort from EL1 */
		uint64_t far = SYS_R(far_el2);
		if (phase == 0 && far >= FD_BASE && far < FD_BASE + 0x300000) {
			phase = 1;
			puts_("[TWIN] BootShim jumped into the FD's SEC at 0x"); hex(far, 9);
			puts_(": the real r5q Mu UEFI cannot run off the phone; the firmware stand-in takes over\n");
			SYS_W(elr_el2, (uint64_t)(uintptr_t)fw_entry);
			SYS_W(spsr_el2, 0x3c5);
			return;
		}
		dump_state("EL1 instruction abort (stage 2)", f); park("");
	}
	if (ec == 0x24) {                                       /* data abort from EL1: device model */
		uint64_t ipa = ((SYS_R(hpfar_el2) >> 4) << 12) | (SYS_R(far_el2) & 0xfff);
		if (!(iss & (1u << 24))) {                          /* no syndrome: cannot emulate */
			dump_state("MMIO access without syndrome (ldp/stp?) at", f);
			SYS_W(elr_el2, elr + 4);
			return;
		}
		int size = 1 << ((iss >> 22) & 3), wnr = (iss >> 6) & 1;
		uint32_t srt = (iss >> 16) & 31;
		if (wnr) {
			uint64_t v = srt == 31 ? 0 : f[srt];
			if (size < 8) v &= (1UL << (8 * size)) - 1;
			log_access(1, ipa, v, size, elr);
			mmio_write(ipa, v, size);
		} else {
			uint64_t v = mmio_read(ipa, size);
			log_access(0, ipa, v, size, elr);
			if (srt != 31) f[srt] = v;
		}
		SYS_W(elr_el2, elr + 4);
		return;
	}
	if (ec == 0x16) {                                       /* HVC from EL1: forward to QEMU's PSCI */
		register uint64_t x0 __asm__("x0") = f[0], x1 __asm__("x1") = f[1], x2 __asm__("x2") = f[2], x3 __asm__("x3") = f[3];
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) :: "memory");
		f[0] = x0; f[1] = x1; f[2] = x2; f[3] = x3;
		return;
	}
	dump_state("unexpected trap to EL2", f);
	park("");
}
void el2_serror(uint64_t *f) { dump_state("SError from EL1", f); park(""); }
void el2_self_fault(uint64_t *f) { dump_state("EL2 SELF FAULT", f); park(""); }

/* =========================================================================================== */
/* EL1: the firmware stand-in (r5q Mu UEFI + Test 12 FirstBootManager, as seen by our loader)  */
/* =========================================================================================== */
static uint64_t el1_l1[512] __attribute__((aligned(4096)));
static uint64_t st[16], conout[8], bs[64], gop[4], gop_mode[5];
static uint32_t gop_info[9];
static uint64_t window_proto[6];
static uint64_t loader_pages;

static uint64_t efi_unsupported(void) { puts_("[TWIN] loader called an unemulated UEFI service\n"); return 0x8000000000000003UL; }
static uint64_t efi_ok(void) { return 0; }
static uint64_t efi_output(void *self, const uint16_t *s)
{
	(void)self;
	char b[2] = { 0, 0 };
	for (; *s; s++) { if (*s == '\r') continue; b[0] = (char)*s; puts_(b); }
	return 0;
}
#ifdef IOS_MODE
static uint64_t rt[16];
static uint64_t efi_alloc_pages(uint64_t type, uint64_t mtype, uint64_t pages, uint64_t *addr)
{
	/* production: the loader claims its whole kernel window by address (AllocateAddress) */
	if (type == 2 && *addr >= 0xC0000000UL && *addr + (pages << 12) <= IOS_RAM_END) {
		puts_("[TWIN] AllocatePages(AllocateAddress) 0x"); hex(*addr, 9); puts_(" + 0x"); hex(pages << 12, 9); puts_(": granted\n");
		return 0;
	}
	(void)mtype;
	puts_("[TWIN] AllocatePages type "); dec(type); puts_(" refused\n");
	return 0x8000000000000009UL;
}
static uint64_t efi_stall(uint64_t us)
{
	uint64_t f = SYS_R(cntfrq_el0), t0 = SYS_R(cntpct_el0);
	while (SYS_R(cntpct_el0) - t0 < us * (f / 1000000)) {}
	return 0;
}
static uint64_t efi_not_found(void) { return 0x800000000000000EUL; }
static uint64_t efi_reset_system(uint64_t type)
{
	puts_("[TWIN] ResetSystem("); dec(type); puts_(") from the loader: PS_HOLD\n");
	*(volatile uint32_t *)PSHOLD = 0;
	for (;;) __asm__ volatile ("wfe");
	return 0;
}
#else
static uint64_t efi_alloc_pages(void) { puts_("[TWIN] AllocatePages refused (the reserved-window loader must not allocate)\n"); return 0x8000000000000009UL; }
#endif
static uint64_t efi_get_memory_map(uint64_t *size, uint8_t *map, uint64_t *key, uint64_t *dsize, uint32_t *dver)
{
	/* the two regions the validated Unicorn harnesses describe: the loader image, firmware data */
#ifdef MAP_NOLOADER
	/* variant: a firmware map that does not describe the loader image as loader code/data */
	const uint64_t loader_type = 7;
#else
	const uint64_t loader_type = 1;
#endif
	/* the regions and types the real r5q firmware reports (Test 12 MemoryMapLib.c): the loader's
	 * cleaning loop walks types 1-6/9/10 and skips 0 (reserved) */
#ifdef IOS_MODE
	/* the production r5q UEFI: fixed regions from r5qPkg MemoryMapLib.c, plus the DRAM banks of the live
	 * DT as conventional memory (the payload at 0xF0000000 is not reserved there: the loader steps
	 * around it itself, so its window starts at 0xF4000000, as on QEMU and as planned for the phone) */
	uint64_t d[] = {
		0, 0x80000000UL, 0, 0x5700000UL / 4096, 0xf, 0,          /* "Kernel": reserved */
		loader_type, LOADER_AT, 0, loader_pages, 0xf, 0,
		4, FD_BASE, 0, 0x300000UL / 4096, 0xf, 0,                /* "UEFI FD": BsData */
		0, 0x9C400000UL, 0, 0x2000000UL / 4096, 0x8, 0,          /* "Display Reserved" */
		7, 0xC0000000UL, 0, 0x1400000UL / 4096, 0xf, 0,          /* DRAM */
		0, 0xC1400000UL, 0, 0x800000UL / 4096, 0x1, 0,           /* "SEC Debug" (Samsung RAM log) */
		7, 0xC1C00000UL, 0, (IOS_RAM_END - 0xC1C00000UL) / 4096, 0xf, 0,   /* DRAM to the end of bank 3 */
	};
#else
	uint64_t d[] = {
		loader_type, LOADER_AT, 0, loader_pages, 0xf, 0,
		4, FD_BASE, 0, 0x300000UL / 4096, 0xf, 0,                /* "UEFI FD": BsData, write-back */
		4, 0xFC000000UL, 0, 0x4000000UL / 4096, 0xf, 0,          /* the stand-in's own data and stack */
		0, 0x9C400000UL, 0, 0x2000000UL / 4096, 0x8, 0,          /* "Display Reserved": reserved */
		0, 0xC1400000UL, 0, 0x800000UL / 4096, 0x1, 0,           /* "SEC Debug" (RAM log): reserved, uncached */
		0, 0xE0000000UL, 0, 0x10000000UL / 4096, 0xf, 0,         /* "XNU Test RAM": reserved */
		0, PAYLOAD, 0, 0x4000000UL / 4096, 0xf, 0,               /* "XNU Payload": reserved */
	};
#endif
	const int n = sizeof(d) / sizeof(d[0]) / 6;
	uint64_t need = (uint64_t)n * 48;
	if (*size < need) { *size = need; return 0x8000000000000005UL; }
	for (int i = 0; i < n; i++) {
		uint8_t *p = map + 48 * i;
		memset(p, 0, 48);
		*(uint32_t *)p = (uint32_t)d[6 * i];
		*(uint64_t *)(p + 8) = d[6 * i + 1];
		*(uint64_t *)(p + 16) = d[6 * i + 1];
		*(uint64_t *)(p + 24) = d[6 * i + 3];
		*(uint64_t *)(p + 32) = d[6 * i + 4];
	}
	*size = need; *key = 1; *dsize = 48; *dver = 1;
	return 0;
}
static const uint8_t guid_gop[16] = { 0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a };
static const uint8_t guid_win[16] = { 0x45,0x99,0x7b,0x48,0x22,0x3e,0xdd,0x4d,0x91,0xb7,0x89,0x24,0x02,0xd6,0xcf,0x99 };
static int guid_eq(const uint8_t *a, const uint8_t *b) { for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0; return 1; }
static uint64_t efi_locate(const uint8_t *guid, void *reg, void **out)
{
	(void)reg;
	if (guid_eq(guid, guid_gop)) { *out = gop; return 0; }
	if (guid_eq(guid, guid_win)) { *out = window_proto; return 0; }
	puts_("[TWIN] LocateProtocol for an unknown GUID\n");
	return 0x800000000000000EUL;
}

static void fw_log(const char *s) { ring_log(s); }

void fw_exception(uint64_t vec)
{
	puts_("[TWIN] EL1 EXCEPTION (the phone's UEFI would dead-loop here) vector="); dec(vec);
	puts_(" ESR_EL1="); hex(SYS_R(esr_el1), 8);
	puts_(" ELR_EL1="); hex(SYS_R(elr_el1), 9);
	puts_(" FAR_EL1="); hex(SYS_R(far_el1), 9);
	puts_(" SCTLR_EL1="); hex(SYS_R(sctlr_el1), 8);
	puts_("\n[TWIN] HANG\n");
	for (;;) __asm__ volatile ("wfe");
}

void fw_main(void)
{
	puts_("[TWIN] firmware stand-in at EL1: building the UEFI environment the loader expects\n");
	/* UEFI-style identity map: 0-1 GiB device, 1-4 GiB normal write-back */
	el1_l1[0] = 0x0UL | (1UL << 10) | (0UL << 2) | 1UL;                         /* attr0 device, AF, block */
#ifdef IOS_MODE
	for (uint64_t i = 1; i < (IOS_RAM_END >> 30); i++) el1_l1[i] = (i << 30) | (1UL << 10) | (3UL << 8) | (1UL << 2) | 1UL;
	__asm__ volatile ("dsb sy" ::: "memory");
	SYS_W(mair_el1, 0xFF00UL);
	SYS_W(tcr_el1, (25UL) | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 32));   /* 39-bit VA, 36-bit PA */
#else
	for (int i = 1; i < 4; i++) el1_l1[i] = ((uint64_t)i << 30) | (1UL << 10) | (3UL << 8) | (1UL << 2) | 1UL;
	__asm__ volatile ("dsb sy" ::: "memory");
	SYS_W(mair_el1, 0xFF00UL);                       /* attr0 Device-nGnRnE, attr1 Normal WB */
	SYS_W(tcr_el1, (32UL) | (1UL << 8) | (1UL << 10) | (3UL << 12));
#endif
	SYS_W(ttbr0_el1, (uint64_t)(uintptr_t)el1_l1);
	__asm__ volatile ("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	SYS_W(sctlr_el1, 0x30d0198dUL);                  /* the UEFI value seen in QEMU (M|C|SA|I + RES1) */
	__asm__ volatile ("isb" ::: "memory");
	SYS_W(icc_sre_el1, 7);
	SYS_W(cpacr_el1, 3UL << 20);                     /* FPEN: UEFI enables FP/SIMD; the loader saves FPCR/FPSR */
	__asm__ volatile ("isb" ::: "memory");

	{
		/* what Mu's ArmGicV3Dxe leaves for a boot application: distributor on with affinity routing,
		 * this core's redistributor awake, the CPU interface on (at the phone's GIC addresses) */
		volatile uint32_t *gicd = (volatile uint32_t *)0x17A00000UL, *gicr = (volatile uint32_t *)0x17A60000UL;
		gicd[0] = 0x10;                              /* ARE */
		gicd[0] = 0x13;                              /* ARE | EnableGrp1 | EnableGrp0 (security disabled) */
		gicr[0x14 / 4] &= ~2u;                       /* GICR_WAKER.ProcessorSleep = 0 */
		while (gicr[0x14 / 4] & 4u) {}               /* ChildrenAsleep */
		SYS_W(S3_0_C4_C6_0, 0xff);                   /* ICC_PMR_EL1 */
		SYS_W(S3_0_C12_C12_7, 1);                    /* ICC_IGRPEN1_EL1 */
		__asm__ volatile ("isb" ::: "memory");
		puts_("[TWIN] GIC: GICD_TYPER=0x"); hex(gicd[1], 8); puts_(" GICR_TYPER=0x"); hex(*(volatile uint64_t *)0x17A60008UL, 16);
		puts_(" (r5q addresses, QEMU GICv3)\n");
	}
#ifdef IOS_MODE
	fw_log("[TWIN-IOS] production r5q UEFI stand-in: booting the payload's loader.efi\n");
#else
	/* the Test 12 firmware's own ring lines, in the order the phone logged them (Test 17 last_kmsg) */
	fw_log("[TEST12] DXE PHASE READY=0x0\n");
	fw_log("[TEST12] PlatformBootManagerBeforeConsole\n");
	fw_log("[TEST12] BC1:FbFillRect\n");
	fw_log("[TEST12] BC2:Checkpoint0\n");
	fw_log("[TEST12] BC3:Print1\n");
	fw_log("[TEST12] BeforeConsole_END\n");
	fw_log("[BDS] 1:BeforeConsole_RET\n");
	fw_log("[BDS] 2:Hotkey_OK\n");
	fw_log("[BDS] 3:Consoles_OK\n");
	fw_log("[TEST12] PlatformBootManagerAfterConsole\n");
	fw_log("[6] PARAM CLEARED 0x2->1 (0x0)\n");
	fw_log("[TEST12] RESERVED WINDOW STATUS=0x0\n");
#endif

	/* the payload the shim copied: find loader.efi */
	volatile uint32_t *ph = (volatile uint32_t *)PAYLOAD;
	if (ph[0] != 0x50554e58) park("[TWIN] no XNUP payload at 0xF0000000 (shim rejected it?)\n");
	const uint8_t *p = (const uint8_t *)(PAYLOAD + 8), *end = p + ph[1], *efi = 0;
	uint32_t efi_size = 0;
	while (p + 24 <= end && p[0]) {
		uint32_t sz = *(const uint32_t *)(p + 16);
		if (p[0] == 'l' && p[1] == 'o' && p[6] == '.' && p[7] == 'e') { efi = p + 24; efi_size = sz; }
		p += 24 + ((sz + 15) & ~15u);
	}
	if (!efi) park("[TWIN] payload has no loader.efi\n");
	puts_("[TWIN] loader.efi from the payload: "); dec(efi_size); puts_(" bytes\n");

	/* PE32+ load + DIR64 relocation, exactly like the Unicorn harnesses */
	uint32_t pe = *(const uint32_t *)(efi + 60);
	const uint8_t *opt = efi + pe + 24;
	uint32_t entry = *(const uint32_t *)(opt + 16);
	uint64_t preferred = *(const uint64_t *)(opt + 24);
	uint32_t image_size = *(const uint32_t *)(opt + 56), header_size = *(const uint32_t *)(opt + 60);
	uint16_t nsec = *(const uint16_t *)(efi + pe + 6), optsz = *(const uint16_t *)(efi + pe + 20);
	uint8_t *img = (uint8_t *)LOADER_AT;
	memset(img, 0, image_size);
	memcpy(img, efi, header_size);
	for (int i = 0; i < nsec; i++) {
		const uint8_t *sh = opt + optsz + 40 * i;
		uint32_t va = *(const uint32_t *)(sh + 12), raw = *(const uint32_t *)(sh + 16), off = *(const uint32_t *)(sh + 20);
		if (raw) memcpy(img + va, efi + off, raw);
	}
	uint32_t rva = *(const uint32_t *)(opt + 112 + 5 * 8), rlen = *(const uint32_t *)(opt + 112 + 5 * 8 + 4);
	for (uint32_t c = rva; c < rva + rlen;) {
		uint32_t page = *(uint32_t *)(img + c), block = *(uint32_t *)(img + c + 4);
		if (block < 8) break;
		for (uint32_t o = c + 8; o < c + block; o += 2) {
			uint16_t it = *(uint16_t *)(img + o);
			if ((it >> 12) == 10) *(uint64_t *)(img + page + (it & 0xfff)) += LOADER_AT - preferred;
		}
		c += block;
	}
	loader_pages = (image_size + 4095) / 4096;
	twin_loader_lo = LOADER_AT;
	twin_loader_hi = LOADER_AT + image_size;
	for (uint64_t a = LOADER_AT; a < LOADER_AT + image_size; a += 64) __asm__ volatile ("dc cvau, %0" :: "r" (a));
	__asm__ volatile ("dsb ish; ic iallu; dsb ish; isb" ::: "memory");

	/* system table, console, boot services, GOP and the R5Q reserved-window protocol */
	for (int i = 0; i < 64; i++) bs[i] = (uint64_t)(uintptr_t)efi_unsupported;
	bs[40 / 8] = (uint64_t)(uintptr_t)efi_alloc_pages;
	bs[56 / 8] = (uint64_t)(uintptr_t)efi_get_memory_map;
	bs[256 / 8] = (uint64_t)(uintptr_t)efi_ok;              /* SetWatchdogTimer */
	bs[320 / 8] = (uint64_t)(uintptr_t)efi_locate;
#ifdef IOS_MODE
	bs[248 / 8] = (uint64_t)(uintptr_t)efi_stall;
	bs[312 / 8] = (uint64_t)(uintptr_t)efi_not_found;      /* LocateHandleBuffer: no block devices */
	rt[104 / 8] = (uint64_t)(uintptr_t)efi_reset_system;
	st[88 / 8] = (uint64_t)(uintptr_t)rt;
#endif
	conout[0] = (uint64_t)(uintptr_t)efi_ok;
	conout[1] = (uint64_t)(uintptr_t)efi_output;
	for (int i = 2; i < 8; i++) conout[i] = (uint64_t)(uintptr_t)efi_ok;
	st[64 / 8] = (uint64_t)(uintptr_t)conout;
	st[80 / 8] = (uint64_t)(uintptr_t)conout;
	st[96 / 8] = (uint64_t)(uintptr_t)bs;
	gop[3] = (uint64_t)(uintptr_t)gop_mode;
	gop_mode[1] = (uint64_t)(uintptr_t)gop_info;
	gop_mode[3] = FB_BASE;
	gop_mode[4] = 1080UL * 2400 * 4;
	gop_info[1] = 1080; gop_info[2] = 2400; gop_info[3] = 1; gop_info[8] = 1080;
	window_proto[0] = 0x31304e4957513552UL;
	window_proto[1] = 1UL | (48UL << 32);
	window_proto[2] = 0xE0000000UL; window_proto[3] = 0x10000000UL; window_proto[4] = 8; window_proto[5] = 1;

	puts_("[TWIN] starting the loader at 0x"); hex(LOADER_AT + entry, 9); puts_(" (FirstBootManager RunTestLoader)\n");
	uint64_t (*efi_main)(uint64_t, void *) = (uint64_t (*)(uint64_t, void *))(uintptr_t)(LOADER_AT + entry);
	uint64_t status = efi_main(0x1234, st);
	puts_("[TWIN] loader returned 0x"); hex(status, 16); puts_("\n");
	{
		char line[40] = "[5] loader.efi OK RET=0x";
		int n = 24;
		for (int i = 7; i >= 0; i--) line[n++] = "0123456789ABCDEF"[(status >> (4 * i)) & 15];
		line[n++] = '\n';
		line[n] = 0;
		/* the phone prints the full status width without leading zeros; match its common case */
		if (status == 0) { line[24] = '0'; line[25] = '\n'; line[26] = 0; }
		fw_log(line);
	}

	fw_log("\n[TEST12] RESERVED WINDOW RELEASE=0x0\n");
	fw_log("[TEST12] Resetting via PSHOLD...\n");
	*(volatile uint32_t *)PSHOLD = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (;;) __asm__ volatile ("wfe");
}
