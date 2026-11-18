/* Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026, from the donor G986N-iOS (commit 946de0fed8e5, a port of HackiOS): Apple Public Source License 2.0, see LICENSE and MODIFICATIONS.md in this directory. */
/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

#ifdef R5Q_TEST_LABEL
#define R5Q_PREPARE_LABEL R5Q_TEST_LABEL
#elif defined(R5Q_TEST18)
#define R5Q_PREPARE_LABEL "TEST18"
#elif defined(R5Q_TEST16)
#define R5Q_PREPARE_LABEL "TEST16"
#elif defined(R5Q_TEST15)
#define R5Q_PREPARE_LABEL "TEST15"
#elif defined(R5Q_TEST14)
#define R5Q_PREPARE_LABEL "TEST14"
#elif defined(R5Q_TEST13)
#define R5Q_PREPARE_LABEL "TEST13"
#elif defined(R5Q_TEST12)
#define R5Q_PREPARE_LABEL "TEST12"
#elif defined(R5Q_TEST8)
#define R5Q_PREPARE_LABEL "TEST8"
#elif defined(R5Q_PREPARE_ONLY)
#define R5Q_PREPARE_LABEL "TEST7"
#else
#define R5Q_PREPARE_LABEL "TEST6"
#endif

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

/* First-board bring-up: retain logs in RAM/USB, with no log partition writes. */
#define ENABLE_PERSISTENT_LOGS 0
#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;

#ifdef R5Q_LAB_TRACE
#ifdef R5Q_COHERENT_TRACE
/*
 * Test 18: keep the Samsung ring coherent across the MMU/cache-off return path and the
 * PSHOLD reset. The return stub writes RB0 straight to RAM with caches off; a dirty or stale
 * cached copy of the header or text would otherwise overwrite it, or never reach RAM before
 * the reset. Clean+invalidate (to the point of coherency) the header before reading it and
 * every written line after writing, as the hardware-verified Test 17 shim does.
 */
static inline void lab_civac(volatile const void *p)
{
	__asm__ volatile ("dc civac, %0" :: "r" (p) : "memory");
}
#define LAB_SYNC_HEADER(sec) do { lab_civac(sec); __asm__ volatile ("dsb sy" ::: "memory"); } while (0)
#define LAB_SYNC_BYTE(p) lab_civac(p)
#else
#define LAB_SYNC_HEADER(sec) ((void)0)
#define LAB_SYNC_BYTE(p) ((void)0)
#endif
// Stage markers after cache cleaning must not call firmware or dirty its heap.
static void lab_trace_ram(const char *text)
{
	volatile uint32_t *sec = (volatile uint32_t *)(uintptr_t)0xC1600000ULL;
	LAB_SYNC_HEADER(sec);
	if (sec[1] != 0x4D474F4Cu) return;
	volatile uint8_t *ring = (volatile uint8_t *)(uintptr_t)0xC1600010ULL;
	uint32_t next = sec[2];
#ifdef R5Q_COHERENT_TRACE
	while (*text) {
		volatile uint8_t *at = &ring[(next++) % 2097136u];
		*at = (uint8_t)*text++;
		LAB_SYNC_BYTE(at);
	}
#else
	while (*text) ring[(next++) % 2097136u] = (uint8_t)*text++;
#endif
	sec[2] = next;
	LAB_SYNC_HEADER(sec);
	__asm__ volatile ("dsb sy" ::: "memory");
}
#else
#define lab_trace_ram(text) ((void)0)
#endif

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };
#ifdef R5Q_LAB_TRACE
	volatile uint32_t *sec = (volatile uint32_t *)(uintptr_t)0xC1600000ULL;
	LAB_SYNC_HEADER(sec);
	if (sec[1] == 0x4D474F4Cu) {
		volatile uint8_t *ring = (volatile uint8_t *)(uintptr_t)0xC1600010ULL;
		uint32_t next = sec[2];
		ring[next % 2097136u] = (uint8_t)c;
		LAB_SYNC_BYTE(&ring[next % 2097136u]);
		sec[2] = next + 1;
		LAB_SYNC_HEADER(sec);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
#endif

#ifndef R5Q_XNU_TEST
	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
#endif
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

#ifdef R5Q_GIC_RESTORE
/*
 * Test 50: the kernel enables its PPIs (the virtual timer: INTID 19 on SM8150) in this core's redistributor and
 * leaves them on when it comes back. The firmware's virtual timer was left enabled and expired by the boot
 * chain (harmless while PPI 19 was disabled), and the return path restores it: back in UEFI that is a level
 * interrupt storm, which the full DEBUG firmware reports forever ("Spurious GIC interrupt 0x13", Test 49).
 * The return stub unmasks IRQs itself, so the timer is masked (IMASK) before the context is saved, and the
 * kernel's extra PPI/SGI enables are taken back after the return. UEFI does not use the virtual timer.
 */
static volatile uint32_t *gic_sgi_frame;
static uint32_t gic_saved_enable;

static void
gic_before_handoff(uint64_t redist)
{
	uint64_t mpidr = READ_SYSREG(MPIDR_EL1), ctl = READ_SYSREG(CNTV_CTL_EL0);
	uint32_t aff = (uint32_t)((((mpidr >> 32) & 0xff) << 24) | (mpidr & 0xffffff));

	gic_sgi_frame = NULL;
	for (int i = 0; i < 16; i++, redist += 0x20000) {
		uint64_t typer = *(volatile uint64_t *)(uintptr_t)(redist + 8);
		if ((uint32_t)(typer >> 32) == aff) {
			gic_sgi_frame = (volatile uint32_t *)(uintptr_t)(redist + 0x10000);
			break;
		}
		if (typer & (1u << 4)) {   /* Last */
			break;
		}
	}
	if (gic_sgi_frame != NULL) {
		gic_saved_enable = gic_sgi_frame[0x100 / 4];
	}
	if (ctl & 1) {
		__asm__ volatile ("msr cntv_ctl_el0, %0\n isb" :: "r" (ctl | 2));
	}
	print("[LAB] gic: core %x redistributor %s, PPI/SGI enables %x; virtual timer ctl %x -> %x\n", mpidr & 0xffffff,
	    gic_sgi_frame != NULL ? "found" : "NOT FOUND", (uint64_t)gic_saved_enable, ctl, READ_SYSREG(CNTV_CTL_EL0));
}

static void
gic_after_return(void)
{
	uint64_t ctl = READ_SYSREG(CNTV_CTL_EL0);
	uint32_t now, extra;

	if (ctl & 1) {
		__asm__ volatile ("msr cntv_ctl_el0, %0\n isb" :: "r" (ctl | 2));
	}
	if (gic_sgi_frame == NULL) {
		return;
	}
	now   = gic_sgi_frame[0x100 / 4];
	extra = now & ~gic_saved_enable;
	gic_sgi_frame[0x180 / 4] = extra;   /* ICENABLER0 */
	__asm__ volatile ("dsb sy" ::: "memory");
	gic_sgi_frame[0x280 / 4] = extra;   /* ICPENDR0 */
	__asm__ volatile ("dsb sy" ::: "memory");
#ifdef R5Q_GIC_REENABLE
	/* Test 51: the kernel also turns off what UEFI had on (Test 50: PPI 18, the firmware's own timer tick) */
	gic_sgi_frame[0x100 / 4] = gic_saved_enable & ~now;   /* ISENABLER0 */
	__asm__ volatile ("dsb sy" ::: "memory");
#endif
	print("[LAB] gic: the kernel left PPI/SGI enables %x; disabled %x, back to %x; virtual timer ctl %x\n",
	    (uint64_t)now, (uint64_t)extra, (uint64_t)gic_sgi_frame[0x100 / 4], READ_SYSREG(CNTV_CTL_EL0));
}
#endif

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
#ifdef R5Q_XNU_TEST
static char out_buf[65536]; // early lab: bounded log staging, no USB or storage service
#else
static char out_buf[2 * 1024 * 1024];
#endif
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

static void
write_logs(const char *klog, uint64_t klog_len)
{
#ifdef R5Q_XNU_TEST
	// The lab trace already goes to Samsung's reserved RAM ring. No USB staging
	// or block buffers are needed for an early checkpoint's bounded kernel log.
#ifdef R5Q_LAB_USERLAND
	/* a userland run's log can be longer: keep its last 1 MiB, and keep it off the (slow) text console */
	if (klog != NULL && klog_len > (1u << 20)) {
		klog += klog_len - (1u << 20);
		klog_len = 1u << 20;
	}
	screen_quiet = 1;
#endif
#ifdef R5Q_LAB_TIMED
	if (klog != NULL && klog_len <= (1u << 20)) {   /* a timed run's log: fits the 2 MiB Samsung ring */
#else
	if (klog != NULL && klog_len <= 65536) {
#endif
		print("\n== KERNEL LOG ==\n");
		for (uint64_t i = 0; i < klog_len; i++) put_char(klog[i]);
		print("\n== END OF LOG ==\n");
	}
#else
	efi_block_io *bio = ENABLE_PERSISTENT_LOGS ? find_log_partition() : NULL;

	compose_logs(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
#endif
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
	/* the port's own partition "darwin" on logical unit 0, the one a write may reach (block_is_darwin): 0 for none */
	uint64_t darwin_first, darwin_last;      /* its first and last LBA, as the GPT has them */
	efi_guid darwin_unique;                  /* its unique GUID */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
	.darwin_first = 25374720, .darwin_last = 30617594,
	.darwin_unique = { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } },
};

static const struct platform platform_y2q = {
	/* Galaxy S20+ 5G (SM8250): the same peripheral window and GIC as redwood, from its device tree. */
	.name = "y2q", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/* Cores 0-3 are Cortex-A55s (Kryo 585 Silver), 4-6 A77s and 7 the prime A77 (capacity 0x766 vs 0x400). */
	.small_cores = 0x0f,
	.fb_base = 0x9C000000,           /* the UEFI's "Display Reserved" (cont_splash_region) */
	/* sda38, 128 GiB at the end of LUN 0, made out of userdata's end (y2q-dump/pre-repartition) */
	.darwin_first = 28904192, .darwin_last = 62458874,
	.darwin_unique = { 0xed95d4da, 0xc7dc, 0x4d4a, { 0xa8, 0x74, 0x6a, 0xc0, 0x97, 0x8e, 0xb3, 0x85 } },
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (common/new-device.sh writes a starting point, docs/PORTING.md says
 * where each value comes from); a display that matches none is QEMU's ramfb.
 */
/*
 * Samsung Galaxy S10 Lite, SM-G770F / r5q / SM8150.
 * GIC addresses, CPU affinity layout and the continuous-splash reservation
 * were read from this board's live FDT. UEFI bring-up remains outstanding.
 * The zero storage bounds leave this new board's persistent-write gate closed.
 */
static const struct platform platform_r5q = {
	.name = "r5q", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000,
#ifdef R5Q_TWIN
	/* EMULATION ONLY (twin/run_twin.py ios): the twin's PL011, so the kernel's console is readable */
	.uart = 0x09000000, .log_other_cpu = 1,
	/* QEMU numbers cores in Aff0 (the phone: Aff1), and its cores stop each other on every TLB flush */
	.cores = 8, .core_shift = 0, .small_cores = 0x0f, .timeout_scale = 20,
#else
	.uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8, .small_cores = 0x0f,
#endif
	.fb_base = 0x9C400000,
	.darwin_first = 0, .darwin_last = 0,
};

static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_y2q,
	&platform_r5q,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/* The phone off, as the firmware turns it off (its ResetSystem: the PMIC lets go of the power). */
static void __attribute__((noreturn))
power_off(void)
{
	hc_take_back("before powering off");
	print("the kernel asked for the phone to be turned off: off\n");
	flush_log();
	ST->rt->reset_system(2 /* EfiResetShutdown */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void) __attribute__((returns_twice));
void loader_return_stub(void);

/*
 * Tests 19-23 (R5Q_STUB_STAGES): the return stub tags each step into the Samsung RAM ring
 * ("RB0".."RB4", cache-maintained so the tags are right whether the MMU and caches are on or
 * off), and R5Q_RESET_STAGE ends the test after a chosen step with the same PS_HOLD write the
 * hardware-verified firmware uses ([TEST12] Resetting via PSHOLD), so each variant returns to
 * Android by itself. Stage 1: in C, before the MMU-off jump. 2: after RB0 (MMU off). 3: after
 * RB2 (translation registers, TLB and I-cache done; MMU off). 4: after RB3 (MMU back on).
 * Undefined: the full return (Test 18 plus the stage tags).
 */
#ifdef R5Q_STUB_STAGES
#define R5Q_STUB_MACROS \
        ".macro r5q_ring_tag lo, hi\n" \
        "  movz x11, #0xc160, lsl #16\n" \
        "  dc civac, x11\n  dsb sy\n" \
        "  ldr w12, [x11, #4]\n" \
        "  movz w13, #0x4f4c\n  movk w13, #0x4d47, lsl #16\n" \
        "  cmp w12, w13\n  b.ne 9f\n" \
        "  ldr w12, [x11, #8]\n" \
        "  movz w13, #0xfff0\n  movk w13, #0x1f, lsl #16\n" \
        "  udiv w14, w12, w13\n  msub w14, w14, w13, w12\n" \
        "  movz w16, #\\lo\n  movk w16, #\\hi, lsl #16\n" \
        "  mov w17, #4\n" \
        "8: add x15, x11, #16\n  add x15, x15, w14, uxtw\n" \
        "  strb w16, [x15]\n  dc civac, x15\n  lsr w16, w16, #8\n" \
        "  add w12, w12, #1\n  add w14, w14, #1\n" \
        "  cmp w14, w13\n  csel w14, wzr, w14, eq\n" \
        "  subs w17, w17, #1\n  b.ne 8b\n" \
        "  dsb sy\n  str w12, [x11, #8]\n  dc civac, x11\n  dsb sy\n" \
        "9:\n" \
        ".endm\n" \
        ".macro r5q_pshold_reset\n" \
        "  movz x11, #0x0c26, lsl #16\n  movk x11, #0x4000\n" \
        "  str wzr, [x11]\n  dsb sy\n" \
        "7: wfe\n  b 7b\n" \
        ".endm\n"
#define R5Q_TAG(n) "  r5q_ring_tag 0x4252, 0x0a3" #n "\n"
#else
#define R5Q_STUB_MACROS ""
#define R5Q_TAG(n) ""
#endif
#if defined(R5Q_RESET_STAGE) && R5Q_RESET_STAGE == 2
#define R5Q_RESET_AFTER_RB0 "  r5q_pshold_reset\n"
#else
#define R5Q_RESET_AFTER_RB0 ""
#endif
#if defined(R5Q_RESET_STAGE) && R5Q_RESET_STAGE == 3
#define R5Q_RESET_AFTER_RB2 "  r5q_pshold_reset\n"
#else
#define R5Q_RESET_AFTER_RB2 ""
#endif
#if defined(R5Q_RESET_STAGE) && R5Q_RESET_STAGE == 4
#define R5Q_RESET_AFTER_RB3 "  r5q_pshold_reset\n"
#else
#define R5Q_RESET_AFTER_RB3 ""
#endif

__asm__ (
        R5Q_STUB_MACROS
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
#if defined(R5Q_RETURN_SELFTEST) && defined(R5Q_STUB_STAGES)
        R5Q_TAG(0)
        R5Q_RESET_AFTER_RB0
#elif defined(R5Q_RETURN_SELFTEST)
        // This profile jumps here with MMU/cache already off, before any XNU
        // execution. Preserve x0-x7; emit RB0 to the reserved Samsung RAM ring.
        "  movz x11, #0xc160, lsl #16\n"
        "  ldr w12, [x11, #4]\n"
        "  movz w13, #0x4f4c\n  movk w13, #0x4d47, lsl #16\n"
        "  cmp w12, w13\n  b.ne 9f\n"
        "  ldr w12, [x11, #8]\n"
        "  movz w13, #0xfff0\n  movk w13, #0x1f, lsl #16\n"
        "  udiv w14, w12, w13\n  msub w14, w14, w13, w12\n"
        "  movz w16, #0x4252\n  movk w16, #0x0a30, lsl #16\n"
        "  mov w17, #4\n"
        "8: add x15, x11, #16\n  add x15, x15, w14, uxtw\n"
        "  strb w16, [x15]\n  lsr w16, w16, #8\n"
        "  add w12, w12, #1\n  add w14, w14, #1\n"
        "  cmp w14, w13\n  csel w14, wzr, w14, eq\n"
        "  subs w17, w17, #1\n  b.ne 8b\n"
        "  str w12, [x11, #8]\n  dsb sy\n"
        "9:\n"
#endif
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        R5Q_TAG(1)
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        R5Q_TAG(2)
        R5Q_RESET_AFTER_RB2
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        R5Q_TAG(3)
        R5Q_RESET_AFTER_RB3
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        R5Q_TAG(4)
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
#ifdef R5Q_ABL_SEQUENCE
	/*
	 * Test 30: mirror the phone's own bootloader. Stock ABL LinuxLoader (research/abl-decomp,
	 * PreparePlatformHardware @0x1e5f8) does, before every jump to Android's kernel at EL1 under uH:
	 * mask interrupts; clean+invalidate the ENTIRE data cache by set/way, every level up to LoC
	 * (ArmCleanInvalidateDataCache @0x3ccf8 -> dc cisw); invalidate the I-cache; then clear SCTLR.C,
	 * then SCTLR.I, then SCTLR.M (+ tlbi vmalle1), each its own write; then invalidate the TLBs.
	 * Emptying the data cache before the MMU goes off also means nothing stale can shadow memory
	 * written while it is off (RB0, the kernel's log) once the return stub turns it back on.
	 * Registers only between the flush and the cache switch-off: no store may re-dirty a line.
	 */
	__asm__ volatile (
                "msr daifset, #0xf\n"
                "mrs x0, clidr_el1\n"
                "ubfx x3, x0, #24, #3\n"            /* LoC */
                "lsl x3, x3, #1\n"
                "cbz x3, 15f\n"
                "mov x10, #0\n"                     /* level << 1 */
                "11: add x2, x10, x10, lsr #1\n"    /* level * 3 */
                "lsr x1, x0, x2\n"
                "and x1, x1, #7\n"                  /* cache type at this level */
                "cmp x1, #2\n"
                "b.lt 14f\n"                        /* no data cache here */
                "msr csselr_el1, x10\n"
                "isb\n"
                "mrs x1, ccsidr_el1\n"
                "and x2, x1, #7\n"
                "add x2, x2, #4\n"                  /* log2(line bytes) */
                "ubfx x4, x1, #3, #10\n"            /* max way */
                "clz w5, w4\n"                      /* way field shift */
                "mov x9, x4\n"
                "12: ubfx x7, x1, #13, #15\n"       /* max set */
                "13: lsl w6, w9, w5\n"
                "orr x11, x10, x6\n"
                "lsl x6, x7, x2\n"
                "orr x11, x11, x6\n"
                "dc cisw, x11\n"
                "subs x7, x7, #1\n"
                "b.ge 13b\n"
                "subs x9, x9, #1\n"
                "b.ge 12b\n"
                "14: add x10, x10, #2\n"
                "cmp x3, x10\n"
                "b.gt 11b\n"
                "15: msr csselr_el1, xzr\n"
                "dsb sy\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x4\n"                /* C off */
                "msr sctlr_el1, x2\n"
                "dsb sy\n"
                "isb\n"
                "bic x2, x2, #0x1000\n"             /* I off */
                "msr sctlr_el1, x2\n"
                "dsb sy\n"
                "isb\n"
                "bic x2, x2, #0x1\n"                /* M off */
                "msr sctlr_el1, x2\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "isb\n"
                "tlbi vmalle1\n"                    /* ArmInvalidateTlb */
                "dsb sy\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa)
                : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "cc", "memory");
	__builtin_unreachable();
#endif
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
	uint64_t power;                 /* KERNEL_POWER_TAG | off or restart: what iOS asked for at its end */
};
#define KERNEL_POWER_TAG        0x52574f5000000000ULL   /* "\0\0\0\0POWR": an older kernel has no such field */
#define KERNEL_POWER_OFF        1
#define KERNEL_POWER_RESTART    2
static uint64_t kernel_power;           /* what the kernel that came back last asked for */

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
/*
 * (Test 126) touches from the PC: struct r5q_touch_mailbox in the kernel's redwood_sec_ts.c, found in the loaded
 * kernel by its first word like kernel_live; "oem touch KIND X0 Y0 X1 Y1 MS" (hex) fills it while iOS runs.
 */
struct kernel_touch {
	uint64_t magic;                 /* "XNUTOUCH" */
	volatile uint32_t seq, done, kind, x0, y0, x1, y1, ms;
};
#define KERNEL_TOUCH_MAGIC      0x484355544f554e58ULL
static struct kernel_touch *kernel_touch_box;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void *maint_live_buf(void);
static uint64_t maint_live_room(void);

static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	/* the kernel's log in order, as up to three pieces */
	const char *piece[3];
	uint64_t piece_len[3], total = 0, skip, n, room;
	char *dst;

	if (!wrapped || ring_at >= live->log_ring) {
		piece[0] = log;
		piece_len[0] = len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring;
		piece_len[1] = piece_len[2] = 0;
		piece[1] = piece[2] = log;
	} else {
		piece[0] = log;
		piece_len[0] = live->log_head;
		piece[1] = log + live->log_head + ring_at;
		piece_len[1] = live->log_ring - ring_at;
		piece[2] = log + live->log_head;
		piece_len[2] = ring_at;
	}
	total = piece_len[0] + piece_len[1] + piece_len[2];

	compose_logs(NULL, 0);
	n = staged_len;
	dst = out_buf;
	room = sizeof(out_buf) - 8192;
	if (maint_live_buf() != NULL) {
		/* the 80 MiB maintenance buffer: room for all of it (the lab build's out_buf is 64 KiB) */
		dst = (char *)maint_live_buf();
		memcpy(dst, out_buf, n);
		room = maint_live_room();
		staged_data = dst;
	}
	/* too long for the room: the newest part is what a live log is for, so the oldest goes */
	skip = (n + sizeof(sep) - 1 + total > room) ? n + sizeof(sep) - 1 + total - room : 0;
	memcpy(dst + n, sep, sizeof(sep) - 1);
	n += sizeof(sep) - 1;
	for (int k = 0; k < 3; k++) {
		uint64_t from = 0, take = piece_len[k];

		if (skip >= take) {
			skip -= take;
			continue;
		}
		from = skip;
		take -= skip;
		skip = 0;
		memcpy(dst + n, piece[k] + from, take);
		n += take;
	}
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static int block_probe_requested;       /* (declared here too: ufs_survey() looks at it) */

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	if (!block_probe_requested) {
		return;         /* (Test 153: a survey for bring-up only -- hashes of dsp/xbl/tz/..., the GPT, super, userdata:
		                 * half a second of every start. ufs_probe=1 brings it back.) */
	}
	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/redwood_ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/redwood_ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define R5Q_USERDATA_LBA        2534400ULL      /* r5q: USERDATA's first block (PIT R5Q_EUR_OPEN; the early log found it there) */
#define R5Q_USERDATA_MIN_BLOCKS (16ULL << 20)   /* ... and at least 64 GiB of it */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }
/* where it is and its unique GUID are each phone's own: struct platform, darwin_first/last/unique */

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start(); ufs_survey() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE;
	const efi_guid uniq = PLAT->darwin_unique;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (str_eq(PLAT->name, "r5q")) {
		/* r5q: USERDATA offered as darwin (block_start): by where it begins, its unit, its blocks */
		if (info->start_lba != R5Q_USERDATA_LBA) {
			why = "USERDATA does not begin where it did";
		} else if (bp->unit != 0) {
			why = "it is not on the first logical unit";
		} else if (bp->block_size != 4096 || bp->blocks < R5Q_USERDATA_MIN_BLOCKS) {
			why = "its blocks are not what they were";
		} else if (bio->media->read_only) {
			why = "UEFI gives it out read only";
		}
	} else if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (PLAT->darwin_last == 0) {
		why = "this phone has no darwin of the port's";
	} else if (info->start_lba != PLAT->darwin_first || info->end_lba != PLAT->darwin_last) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != PLAT->darwin_last - PLAT->darwin_first + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		/*
		 * r5q (Test 80): iOS replaced Android, and USERDATA is the port's: the system image from block 0, the install
		 * record at 4 GiB, the firmware's logs after it -- and from BLOCK_WRITE_FIRST (7.8 GiB) on, iOS's data
		 * volume (userland var_store, slots at 8 GiB). Offered to the kernel under the name it knows, "darwin".
		 */
		if (str_eq(PLAT->name, "r5q") && str_eq(bp->name, "userdata") && info->start_lba == R5Q_USERDATA_LBA &&
		    bp->block_size == 4096 && bp->blocks >= R5Q_USERDATA_MIN_BLOCKS) {
			bp->name[0] = 'd', bp->name[1] = 'a', bp->name[2] = 'r', bp->name[3] = 'w', bp->name[4] = 'i', bp->name[5] = 'n',
			bp->name[6] = 0;
		}
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			if (block_write_part < 0 || part != (uint32_t)block_write_part || lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || PLAT != &platform_redwood) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/*
 * The y2q's Wi-Fi enable, GPIO20 (dt: qcom,cnss-qca6490 wlan-en-gpio; output high, pull-up, 16 mA): its
 * register resets the phone when the kernel reads it (bsd/dev/redwood_pcie.c, pcie_survey=2). UEFI's
 * own pin driver may reach it where we do not: 1 reads it through that driver, 2 also makes it an
 * output, drives it high and reads it back.
 */
#define PIN_WLAN_EN             20

static void
wlan_en_start(int how)
{
	static const efi_guid tlmm_guid = QCOM_TLMM_GUID;
	struct qcom_tlmm *tlmm = NULL;
	uint32_t input = TLMM_GPIO_CFG(PIN_WLAN_EN, 0, 0, TLMM_PULL_UP, TLMM_16MA);
	uint32_t output = TLMM_GPIO_CFG(PIN_WLAN_EN, 0, 1, TLMM_PULL_UP, TLMM_16MA);
	uint32_t value = 0xff;
	efi_status status;

	if (PLAT != &platform_y2q) {
		return;
	}
	status = BS->locate_protocol(&tlmm_guid, NULL, (void **)&tlmm);
	if (EFI_ERROR(status) || tlmm == NULL) {
		print("wlan_en: no pin driver in UEFI (%x)\n", status);
		return;
	}
	print("wlan_en: UEFI's pin driver, revision %d: about to read GPIO%d through it\n", tlmm->revision,
	    (uint64_t)PIN_WLAN_EN);
	flush_log();
	status = tlmm->gpio_in(input, &value);
	print("wlan_en: GPIO%d reads %d (%x)\n", (uint64_t)PIN_WLAN_EN, (uint64_t)value, status);
	if (how < 2) {
		return;
	}
	print("wlan_en: about to make GPIO%d an output and drive it high\n", (uint64_t)PIN_WLAN_EN);
	flush_log();
	status = tlmm->config_gpio(output, 1 /* TLMM_GPIO_ENABLE */);
	print("wlan_en: configured (%x)\n", status);
	status = tlmm->gpio_out(output, 1 /* GPIO_HIGH_VALUE */);
	value = 0xff;
	tlmm->gpio_in(output, &value);
	print("wlan_en: driven high (%x); it reads %d now\n", status, (uint64_t)value);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	const uint8_t *resident;        /* "resident": an image that comes with the payload, for the resident address */
	uint32_t resident_size;
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC || ph->size > PAYLOAD_MAX_SIZE - sizeof(*ph)) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
#ifdef R5Q_XNU_TEST
	if (ph->size > PAYLOAD_MAX_SIZE - 0x100000ULL - sizeof(*ph)) {
		print("lab payload overlaps reserved log/map scratch\n");
		return 0;
	}
#endif
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		unsigned name_len = 0;
		while (name_len < sizeof(e->name) && e->name[name_len] != 0) name_len++;
		uint64_t padded = ((uint64_t)e->size + 15) & ~15ULL;
		if (name_len == sizeof(e->name) || e->reserved != 0 ||
		    padded > (uint64_t)(pend - p) - sizeof(*e)) {
			print("payload entry has invalid name or bounds\n");
			return 0;
		}
		if ((str_eq(e->name, "cmdline") || str_eq(e->name, "checkpoint") ||
		     str_eq(e->name, "physbase") || str_eq(e->name, "rdres")) &&
		    (e->size == 0 || text[e->size - 1] != 0)) {
			print("payload text entry is not terminated\n");
			return 0;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "resident")) {
			pi->resident = p + sizeof(*e);            /* put at the resident address, as `xl ramdisk` would */
			pi->resident_size = e->size;
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + padded;
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
#ifndef R5Q_XNU_TEST
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
#endif
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
#ifdef R5Q_RESERVED_WINDOW
	static const efi_guid window_guid = {
		0x487b9945, 0x3e22, 0x4ddd, {0x91,0xb7,0x89,0x24,0x02,0xd6,0xcf,0x99}
	};
	struct reserved_window {
		uint64_t magic;
		uint32_t version, size;
		uint64_t base, length, attributes, flags;
	} *window = NULL;
	if (pi->forced_base != 0 || EFI_ERROR(BS->locate_protocol(&window_guid, NULL, (void **)&window)) ||
	    window == NULL || window->magic != 0x31304E4957513552ULL || window->version != 1 ||
	    window->size != sizeof(*window) || window->base != 0xE0000000ULL ||
	    window->length != 0x10000000ULL || window->attributes != 8 || window->flags != 1) {
		print("reserved RAM ownership handoff refused\n");
		return 0;
	}
	win_base = window->base;
	win_size = window->length;
	print("reserved firmware-owned window %x size %x\n", win_base, win_size);
	return 1;
#else
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	if (desc_size < sizeof(efi_memory_descriptor) || map_size > sizeof(map_buf) || map_size % desc_size != 0) {
		print("GetMemoryMap returned invalid descriptor bounds\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		if (d->pages > (~0ULL - d->phys_start) >> 12) continue;
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
#if defined(R5Q_XNU_TEST) && !defined(R5Q_LAB_USERLAND)
		// Constrain the actual claim, even if firmware exposes other free banks.
		if (lo < 0xE0000000ULL) lo = 0xE0000000ULL;
		if (hi > 0xF0000000ULL) hi = 0xF0000000ULL;
#endif
		if (hi <= lo || lo > ~0ULL - (BASE_ALIGN - 1)) continue;
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
#ifdef R5Q_XNU_TEST
		print("forced RAM windows are disabled for this R5Q lab profile\n");
		return 0;
#else
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
#endif
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	if (best_size == 0) {
		print("no usable RAM window\n");
		return 0;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
#endif
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

#ifdef R5Q_LAB_TIMED
static void r5q_charger_setup(void);
static void r5q_maintenance(int keep_usb);
static void r5q_clock_start(void);
static void r5q_clock_patch(char *cmdline);
static int r5q_args_load(void);
static const char *r5q_args_merge(const char *base);
static void r5q_modem_boot(int start);
static void r5q_adsp_boot(int start);
static void r5q_slpi_boot(int start);
static uint64_t r5q_rmtfs_pa;                  /* (Test 88: tentative; set by r5q_rmtfs_setup) */
#endif
static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
#ifdef R5Q_XNU_TEST
	// Last 512 KiB of the existing 64 MiB payload reservation, after pram.
	// Only the first 64 KiB is used; it is not part of the payload or XNU RAM.
	uint8_t *map_buf = (uint8_t *)(uintptr_t)(PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x80000ULL);
#else
	static uint8_t map_buf[65536];
#endif
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
#ifdef R5Q_XNU_TEST
#ifdef R5Q_LAB_TIMED
	/* Test 34: no checkpoint; the kernel runs for R5Q_LAB_TIMED seconds and comes back on its own timer */
#define R5Q_LT_STR2(n) #n
#define R5Q_LT_STR(n) R5Q_LT_STR2(n)
#ifndef R5Q_LAB_CMDLINE
#define R5Q_LAB_CMDLINE "loader_return_after=" R5Q_LT_STR(R5Q_LAB_TIMED)
#endif
#ifdef R5Q_LAB_USERLAND
	/*
	 * Test 49+: the timed lab run with a root: the payload's ramdisk and a resident system image that the
	 * full firmware put at the window's resident address (rdres) are allowed; the arguments must still
	 * start with the timed run's own, so the kernel always comes back with its log.
	 */
	if (!starts_with(pi.cmdline, R5Q_LAB_CMDLINE) || pi.resident != NULL || pi.usb_mode) {
#else
	if (!str_eq(pi.cmdline, R5Q_LAB_CMDLINE) || pi.ramdisk != NULL ||
	    pi.resident != NULL || pi.resident_rd != 0 || pi.usb_mode) {
#endif
#else
	if (!str_eq(pi.cmdline, "-v") || pi.ramdisk != NULL || pi.resident != NULL ||
	    pi.resident_rd != 0 || pi.usb_mode) {
#endif
		print("early R5Q lab accepts only -v without peripheral/userland payloads\n");
		return 0;
	}
#endif
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (pi.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}
	if (mh->ncmds == 0 || mh->ncmds > 256 || mh->sizeofcmds > pi.kernel_size - sizeof(*mh)) {
		print("invalid Mach-O command table bounds\n");
		return 0;
	}
	const uint8_t *command_end = kernel + sizeof(*mh) + mh->sizeofcmds;

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if ((const uint8_t *)lc + sizeof(*lc) > command_end || lc->cmdsize < sizeof(*lc) ||
		    lc->cmdsize > (uint64_t)(command_end - (const uint8_t *)lc)) {
			print("invalid Mach-O command extent\n");
			return 0;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (lc->cmdsize < sizeof(*sg) || sg->filesize > sg->vmsize || sg->fileoff > pi.kernel_size ||
			    sg->filesize > pi.kernel_size - sg->fileoff || sg->vmaddr + sg->vmsize < sg->vmaddr) {
				print("invalid Mach-O segment bounds\n");
				return 0;
			}
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			if (lc->cmdsize < 16 + 33 * sizeof(uint64_t)) return 0;
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if ((const uint8_t *)lc != command_end || vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET ||
	    vm_hi <= vm_lo || entry_va < vm_lo || entry_va >= vm_hi) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}
#ifdef R5Q_LAB_TIMED
	/*
	 * r5q (Test 75): the PC's maintenance mode only now, with the kernel's window (and the iOS image resident in it)
	 * claimed: USB's transfer buffers, taken below 4 GB, otherwise landed on the image's first megabytes, which the
	 * firmware had just let go of for this claim -- launchd "exec failed, errno 8" and a loop (Test 74).
	 */
	if (checkpoint != CHECKPOINT_SELFTEST) {
		int live = 0;

		for (const char *c = pi.cmdline; *c; c++) {
			live |= starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ');
		}
		r5q_clock_start();                      /* (Test 81: the time from the PM8150 RTC and the anchor) */
		r5q_maintenance(live);                  /* (where the PC may set it: oem settime) */
		r5q_clock_patch((char *)(uintptr_t)pi.cmdline);
		r5q_args_load();                        /* (Test 82: the PC's extra boot-args, oem args set) */
		pi.cmdline = r5q_args_merge(pi.cmdline);
		{
			int charger = 1;                /* (Test 102: the S2MU106 set to charge; charger=0 leaves it alone) */

			for (const char *c = pi.cmdline; *c; c++) {
				if (starts_with(c, "charger=0") && (c == pi.cmdline || c[-1] == ' ')) {
					charger = 0;
				}
			}
			if (charger) {
				r5q_charger_setup();
			}
		}
		for (const char *c = pi.cmdline; *c; c++) {     /* (Test 83: modem=1 reads the modem's image, modem=2 starts it) */
			if (starts_with(c, "modem=") && (c == pi.cmdline || c[-1] == ' ') && (c[6] == '1' || c[6] == '2' || c[6] == '3')) {
				r5q_modem_boot(c[6] == '1' ? 0 : c[6] == '2' ? 1 : 2);
			}
		}
		for (const char *c = pi.cmdline; *c; c++) {     /* (Test 139: adsp=1 reads the audio DSP's image, adsp=2 starts it) */
			if (starts_with(c, "adsp=") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == '1' || c[5] == '2')) {
				r5q_adsp_boot(c[5] == '2');
			}
		}
		for (const char *c = pi.cmdline; *c; c++) {     /* (Test 200: slpi=1 reads the sensors' DSP image, slpi=2 starts it) */
			if (starts_with(c, "slpi=") && (c == pi.cmdline || c[-1] == ' ') && c[5] >= '1' && c[5] <= '9') {
				r5q_slpi_boot(c[5] >= '5' ? 5 : c[5] >= '2');   /* (2-4: started; 5: loaded, the kernel releases it) */
			}
		}
	}
#endif

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	if (vm_hi - KERNEL_VIRT_BASE > win_size - PAGE_SIZE_MAX ||
	    vm_lo - KERNEL_VIRT_BASE >= win_size) {
		print("kernel virtual extent does not fit claimed RAM\n");
		return 0;
	}
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	/* a boot without the computer: the image that comes with the payload takes the resident one's place */
	if (pi.resident != NULL && pi.resident_size != 0) {
		if (RESIDENT_RD_OFFSET + pi.resident_size + RESIDENT_RD_RESERVE > win_size) {
			print("the payload's resident image (%x bytes) does not fit the window\n", (uint64_t)pi.resident_size);
			return 0;
		}
		memcpy((void *)(phys_base + RESIDENT_RD_OFFSET), pi.resident, pi.resident_size);
		resident_rd_size = pi.resident_size;
		print("resident image from the payload: %x bytes at %x\n", (uint64_t)pi.resident_size, phys_base + RESIDENT_RD_OFFSET);
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0;
#ifdef R5Q_LAB_TIMED
	int lab_ufs = 0;                /* Test 80: ufs=2 in the installed (lab) profile */
#endif
#ifndef R5Q_XNU_TEST
	int ufs_requested = 0;
#endif
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (bsd/dev/redwood_torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (bsd/dev/redwood_camera.c) */
	int camera_requested = 0;
	/* "wlan_en=1" (read) or "=2" (drive high): the y2q's Wi-Fi enable through UEFI's pin driver (wlan_en_start) */
#ifndef R5Q_XNU_TEST
	int wlan_en_requested = 0;
#endif
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
#if defined(R5Q_XNU_TEST) && defined(R5Q_LAB_SMP)
	/*
	 * Test 65: the lab userland run with more cores (Test 64: iOS on one core at 1.48 GHz was slow; Settings took
	 * 15 s to spawn). "cpus=N"/"cpumask=M" as in the production profile: the cores that PSCI says are off are
	 * listed after the boot core, and the kernel starts them itself (PSCI CPU_ON) and stops them at its return.
	 */
	for (const char *c = pi.cmdline; *c; c++) {
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
#ifdef R5Q_LAB_TIMED
		/*
		 * Test 79, live mode: "bootcpu=N" as in the production profile -- iOS on core N, and this loader stays on
		 * core 0 watching it (run_on_other_cpu): fastboot while iOS runs (oem log: its log so far; oem stop).
		 */
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '1' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		/* Test 80: the phone's storage served to the kernel from this core (ufs=2), writes into darwin (ufs_write=1) */
		lab_ufs |= starts_with(c, "ufs=2") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
#endif
		/* r5q (Test 179): "torch=N" here too -- the flash said to be there (MobileGestalt's camera-flash: Control Center's
		 * flashlight), the light the kernel's (bsd/dev/redwood_torch.c, the PM8150L's flash module) */
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
	}
#ifdef R5Q_LAB_TIMED
	if (lab_ufs && kernel_mpidr != this_mpidr) {
		/* (only with the kernel on another core: this one answers its block requests while it runs) */
		hc_init();
		ufs_survey();
		block_start();
	}
#endif
#endif
#ifndef R5Q_XNU_TEST
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		if (starts_with(c, "wlan_en=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '1' && c[8] <= '2') {
			wlan_en_requested = c[8] - '0';
		}
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (wlan_en_requested) {
		wlan_en_start(wlan_en_requested);
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
#endif
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	kernel_touch_box = NULL;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_TOUCH_MAGIC) {
			kernel_touch_box = (struct kernel_touch *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
#ifdef R5Q_QCOM_VTIMER
	/*
	 * Qualcomm wires the architected timers to its own PPIs: on SM8150 the EL1 virtual timer is INTID 19
	 * (Android's DT: arm,armv8-timer <1 3 ...>), not ARM's 27. The kernel (r5q overlay of gic_v3_irq.c)
	 * takes it from gic_vtimer=; without it, it boots until it first waits on its timer and sleeps for
	 * good (Tests 34/35 on the phone). R5Q_QCOM_VTIMER is the INTID; the twin (QEMU, 27) leaves it out.
	 */
#define R5Q_VT_STR2(n) #n
#define R5Q_VT_STR(n) R5Q_VT_STR2(n)
	if (PLAT == &platform_r5q) {
		for (const char *c = "gic_vtimer=" R5Q_VT_STR(R5Q_QCOM_VTIMER) " "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
#endif
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	{
		/*
		 * r5q (Test 189): 64 KB below 4 GB for DMA engines that take 32-bit addresses (the SLIMbus BAM's message
		 * queues, bsd/dev/redwood_adsp.c): iOS gets no RAM below 4 GB. Once per loader (it stays across runs).
		 */
		static uint64_t lowdma;
		static const char prefix[] = "lowdma_pa=0x";
		static const char hex[] = "0123456789abcdef";

		if (lowdma == 0) {
			uint64_t a = 0xffffffffULL;

			if (!EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, 0x10000 >> 12, &a))) {
				lowdma = a;
				memset((void *)(uintptr_t)lowdma, 0, 0x10000);
				clean_dcache(lowdma, lowdma + 0x10000);
			}
		}
		if (lowdma != 0) {
			for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
				ba->commandLine[n++] = *c;
			}
			for (int shift = 28; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
				ba->commandLine[n++] = hex[(lowdma >> shift) & 0xf];
			}
			if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
		}
	}
	if (r5q_rmtfs_pa != 0) {
		static const char prefix[] = "rmtfs_pa=0x";
		static const char hex[] = "0123456789abcdef";

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 28; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(r5q_rmtfs_pa >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr = (uint64_t)core << plat->core_shift;
				int64_t state;

				if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << ((cpu_mpidr[i] >> plat->core_shift) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
#ifdef R5Q_PREPARE_ONLY
	print(R5Q_PREPARE_LABEL " KERNEL PREPARATION VERIFIED; CPU/MMU handoff not attempted\n");
	return 2;
#endif
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	lab_trace_ram("[LAB] before context save\n");
	if (loader_save_ctx() == 0) {
		lab_trace_ram("[LAB] context saved\n");
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = 65536;
		if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver)) ||
		    desc_size < sizeof(efi_memory_descriptor) || map_size > 65536 ||
		    map_size % desc_size != 0) {
			print("handoff refused: invalid firmware memory map\n");
			return 0;
		} else {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				if (d->pages > (~0ULL - d->phys_start) >> 12) {
					print("handoff refused: overflowing firmware memory descriptor\n");
					return 0;
				}
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
#ifndef R5Q_SKIP_MAP_CLEAN
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
#endif
			}
		}
#ifdef R5Q_SKIP_MAP_CLEAN
		/*
		 * Test 31: the map is still fetched and validated, but no region is cleaned by address. None of
		 * the tests that passed on the phone ever ran this loop; if it is what hangs (e.g. a region the
		 * hypervisor protects), Test 19 shows it, and this variant avoids it. R5Q_ABL_SEQUENCE's
		 * set/way flush then cleans and invalidates every line of every level instead.
		 */
		lab_trace_ram("[LAB] firmware map validated; per-region clean skipped (set/way flush follows)\n");
#endif
		lab_trace_ram("[LAB] firmware cache clean complete\n");
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
#ifdef R5Q_EXPLICIT_CTX_CLEAN
		/*
		 * Test 29 candidate fix: the return stub reads loader_ctx (TTBR0/TCR/MAIR/SCTLR/VBAR/CPACR/SP)
		 * with the MMU and caches OFF, so that context must be at the point of coherency before the jump.
		 * On the single-core path it is otherwise flushed only if the firmware memory map happens to
		 * report the loader image as a cleanable type, which is not guaranteed on the phone. Clean and
		 * invalidate it (and this stack frame) explicitly so the restore cannot read a stale line.
		 */
		flush_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));
		flush_dcache(sp_now - 0x2000, sp_now + 0x2000);
		lab_trace_ram("[LAB] ctx+stack flushed to PoC (explicit)\n");
#endif
#ifdef R5Q_GIC_RESTORE
		gic_before_handoff(PLAT->gic_redist);
#endif
		lab_trace_ram("[LAB] before MMU-off handoff\n");
		if (checkpoint == CHECKPOINT_SELFTEST) {
#ifdef R5Q_ARM_WDT
			/*
			 * Tests 25-28: arm the APSS watchdog (Samsung watchdog_v2.c layout) just before the
			 * risky MMU-off step, so a hang in the hand-off reboots the phone by itself (debug level
			 * LOW -> a bite reboots to Android). Bite 8 s is far longer than a successful stage
			 * reset or full return, so it never fires on a good run. See research/r5q-hypervisor-watchdog.md.
			 */
			{
				volatile uint32_t *wdt = (volatile uint32_t *)(uintptr_t)0x17C10000ULL;
				wdt[0x08 / 4] = 0;
				__asm__ volatile ("dsb sy" ::: "memory");
				wdt[0x10 / 4] = 5u * 32765u;     /* WDT0_BARK_TIME */
				wdt[0x14 / 4] = 8u * 32765u;     /* WDT0_BITE_TIME */
				wdt[0x04 / 4] = 1;               /* WDT0_RST */
				wdt[0x08 / 4] = 1;               /* WDT0_EN */
				__asm__ volatile ("dsb sy" ::: "memory");
				lab_trace_ram("[LAB] WDT ARMED: bite 8 s guard over the hand-off\n");
			}
#endif
#if defined(R5Q_RESET_STAGE) && R5Q_RESET_STAGE == 5
			/*
			 * Test 24: watchdog probe. Arm the APSS watchdog (0x17C10000) exactly as Samsung's
			 * watchdog_v2.c does (32765 Hz sleep clock; bark 5 s, bite 8 s), never pet it, and wait.
			 * A bite resets the phone through the secure side. If no reset comes within 20 s by
			 * the virtual counter, fall back to the firmware's PS_HOLD write so the test returns anyway.
			 */
			{
				volatile uint32_t *wdt = (volatile uint32_t *)(uintptr_t)0x17C10000ULL;
				uint64_t freq, start, now;

				lab_trace_ram("[LAB] WDT PROBE: arming bark 5 s, bite 8 s; not petting\n");
				wdt[0x08 / 4] = 0;                       /* WDT0_EN off while programming */
				__asm__ volatile ("dsb sy" ::: "memory");
				wdt[0x10 / 4] = 5u * 32765u;             /* WDT0_BARK_TIME */
				wdt[0x14 / 4] = 8u * 32765u;             /* WDT0_BITE_TIME */
				wdt[0x04 / 4] = 1;                       /* WDT0_RST: restart the count */
				wdt[0x08 / 4] = 1;                       /* WDT0_EN */
				__asm__ volatile ("dsb sy" ::: "memory");
				lab_trace_ram("[LAB] WDT PROBE: armed, waiting for the bite\n");
				__asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
				__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (start) :: "memory");
				do {
					__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (now) :: "memory");
				} while (freq != 0 && now - start < 20 * freq);
				lab_trace_ram("[LAB] WDT PROBE: no bite after 20 s; PS_HOLD fallback\n");
				*(volatile uint32_t *)(uintptr_t)0x0C264000ULL = 0;
				__asm__ volatile ("dsb sy" ::: "memory");
				for (;;) {
					__asm__ volatile ("wfe");
				}
			}
#endif
#if defined(R5Q_RESET_STAGE) && R5Q_RESET_STAGE == 1
			/* Test 19: everything up to the MMU-off jump, then the firmware's own PS_HOLD reset. */
			lab_trace_ram("[LAB] STAGE 1: PS_HOLD reset before MMU-off\n");
			*(volatile uint32_t *)(uintptr_t)0x0C264000ULL = 0;
			__asm__ volatile ("dsb sy" ::: "memory");
			for (;;) {
				__asm__ volatile ("wfe");
			}
#endif
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	lab_trace_ram("[LAB] context restored\n");
#ifdef R5Q_GIC_RESTORE
	gic_after_return();
#endif
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	kernel_power = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && checkpoint != CHECKPOINT_SELFTEST &&
	    (kernel_live->power & ~0xffULL) == KERNEL_POWER_TAG) {
		kernel_power = kernel_live->power & 0xff;
		print("the kernel ended with a request to %s the phone\n", kernel_power == KERNEL_POWER_OFF ? "turn off" :
		    "restart");
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
/*
 * r5q maintenance mode (Test 73+): the installed S10 Lite from the PC, over fastboot (18d1:d00d, Google's driver).
 * Entered with a volume key held while the loader starts (r5q_maintenance()). Besides the loader's own commands:
 *   oem fwlog                      stage the firmware's 2 MiB RAM log ring (then "fastboot get_staged FILE")
 *   oem part NAME                  the partition's size in the reply
 *   oem readpart NAME OFF LEN      stage LEN bytes from OFF (hex, whole blocks, at most 80 MiB)
 *   download / "fastboot stage F"  into a buffer of its own (80 MiB): the iOS payload in memory stays as it is
 *   oem writepart NAME OFF         write the last download at OFF, then read it back and compare. NAME is boot or
 *                                  userdata only; boot takes whole Android boot images (ANDROID!, the partition's size)
 *   flash:boot                     = oem writepart boot 0
 *   oem continue                   leave the commands and start iOS;  oem poweroff;  reboot
 */
#define MAINT_BUF_SIZE  (80ULL << 20)
static int maint_mode;
static int maint_continue;              /* "oem continue" ends the command loop */
static int maint_probe;                 /* the loader listens for a PC before iOS: its first command enters the mode */
static uint8_t *maint_buf;
static uint64_t maint_len;
static uint8_t maint_check[1 << 20];

/* the download buffer of its own (80 MiB); allocated only once the kernel's window is claimed */
static int
maint_alloc(void)
{
	uint64_t a = 0;

	if (maint_buf == NULL) {
		if (EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, MAINT_BUF_SIZE >> 12, &a))) {
			print("maintenance: no memory for the download buffer\n");
			return 0;
		}
		maint_buf = (uint8_t *)(uintptr_t)a;
	}
	return 1;
}

/* maintenance mode on */
static void
maint_enter(void)
{
	if (maint_alloc()) {
		maint_mode = 1;
	}
}

/* where a live kernel log is composed (oem log during a run): the maintenance buffer when there is one */
static void *
maint_live_buf(void)
{
	return maint_buf;
}

static uint64_t
maint_live_room(void)
{
	return MAINT_BUF_SIZE - 8192;
}

static uint64_t
maint_hex(const char **p)
{
	uint64_t v = 0;

	while (**p == ' ') {
		(*p)++;
	}
	if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
		*p += 2;
	}
	for (;; (*p)++) {
		char c = **p;
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) {
			break;
		}
		v = v * 16 + (uint64_t)d;
	}
	return v;
}

static const char *
maint_word(const char **p, char *out, unsigned size)
{
	unsigned i = 0;

	while (**p == ' ') {
		(*p)++;
	}
	for (; **p != 0 && **p != ' ' && i + 1 < size; (*p)++) {
		out[i++] = **p;
	}
	out[i] = 0;
	return out;
}

/* The one GPT partition with this name (ASCII), or NULL if there is not exactly one. */
static efi_block_io *
maint_part(const char *want)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;
	efi_block_io *found = NULL;
	int matches = 0;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		if (str_eq(name, want)) {
			found = bio;
			matches++;
		}
	}
	return matches == 1 ? found : NULL;
}

/*
 * r5q (Test 81): iOS's clock without a PC. The firmware has no real-time clock of its own (RealTimeClockLibNull), but
 * the PM8150 has one that keeps counting while the phone is off: RTC_RW (peripheral 0x60 of slave 0), seconds in
 * RDATA0..3 (registers 0x48..0x4b; Linux rtc-pm8xxx, pm8941_regs). Its count is not the date (Android keeps an offset
 * of its own), so the loader keeps one too: an anchor (a wall time the PC gave, "oem settime", and the RTC's count at
 * that moment) in USERDATA's block 4 GiB + 512 KiB, between the install record and the firmware's log slots.
 * Reads only, through this system's observer channel, as bsd/dev/redwood_buttons.c does; nothing is written to the PMIC.
 */
static inline uint32_t
mmio_read32(uint64_t a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}

static inline void
mmio_write32(uint64_t a, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)a = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

#define R5Q_TIME_LBA            ((4ULL << 30) / 4096U + 128U)
#define R5Q_TIME_MAGIC          "R5Q-TIME-ANCHOR1"
#define R5Q_ARB_CORE            0x0c440000UL
#define R5Q_ARB_CNFG            0x0c40a000UL
#define R5Q_ARB_OBSERVER        0x0e600000UL
#define R5Q_RTC_ID              0x060u          /* slave 0, peripheral 0x60: registers 0x6000 */

struct r5q_time_anchor {
	char magic[16];
	uint64_t wall;                  /* seconds since 1970 (UTC), from the PC */
	uint32_t rtc;                   /* the PM8150 RTC's count at that moment */
	uint32_t rtc_ok;
	uint64_t set_count;             /* how many times the PC set it */
	uint64_t sum;                   /* of the 40 bytes before it */
};

static uint64_t r5q_wall_now;           /* the wall time worked out for this start (0: none) */
static uint32_t r5q_rtc_seen;
static int r5q_rtc_valid;

static uint64_t
r5q_anchor_sum(const struct r5q_time_anchor *a)
{
	const uint8_t *b = (const uint8_t *)a;
	uint64_t h = 1469598103934665603ULL;

	for (unsigned i = 0; i < 40; i++) {
		h = (h ^ b[i]) * 1099511628211ULL;
	}
	return h;
}

static int
r5q_spmi_channel(uint32_t id, uint32_t *channel)
{
	uint32_t channels = mmio_read32(R5Q_ARB_CORE + 0x004) & 0x7ff;
	int found = 0;

	if (channels == 0 || channels > 512) {
		channels = 512;
	}
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = mmio_read32(R5Q_ARB_CORE + 0x900 + 4 * n);

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		if (!found || (mmio_read32(R5Q_ARB_CNFG + 0x700 + 4 * n) & 7) == 0) {
			*channel = n;
			found = 1;
		}
	}
	return found;
}

/* `count` (1..4) registers of the peripheral on `channel` from `reg`; 0 if the arbiter said anything but done */
static int
r5q_spmi_read(uint32_t channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	typedef uint64_t (*tpl_raise_fn)(uint64_t);
	typedef void (*tpl_restore_fn)(uint64_t);
	uint64_t ch = R5Q_ARB_OBSERVER + 0x80UL * channel, old;
	uint32_t status = 0, data;

	/* (the firmware's own SPMI reads, from a timer callback, use the same channel: none of them in between) */
	old = ((tpl_raise_fn)BS->raise_tpl)(31);
	mmio_write32(ch + 0x00, (1u << 27) | (uint32_t)reg << 4 | (count - 1));
	for (int i = 0; i < 20000 && !(status & 1); i++) {
		status = mmio_read32(ch + 0x08);
	}
	data = mmio_read32(ch + 0x18);
	((tpl_restore_fn)BS->restore_tpl)(old);
	if ((status & 0xf) != 1) {
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data >> (8 * i));
	}
	return 1;
}

/* the PM8150 RTC's count of seconds; 0 if it could not be read */
static int
r5q_rtc_read(uint32_t *secs)
{
	uint32_t channel;
	uint8_t a[4], b[4], ctrl;

	if (!r5q_spmi_channel(R5Q_RTC_ID, &channel)) {
		print("clock: no arbiter channel is the PM8150's RTC\n");
		return 0;
	}
	if (!r5q_spmi_read(channel, 0x46, &ctrl, 1) || !r5q_spmi_read(channel, 0x48, a, 4) ||
	    !r5q_spmi_read(channel, 0x48, b, 4)) {
		print("clock: the PM8150's RTC (channel %d) did not answer\n", (uint64_t)channel);
		return 0;
	}
	if (b[0] < a[0] && !r5q_spmi_read(channel, 0x48, b, 4)) {        /* (the low byte rolled over in between) */
		return 0;
	}
	*secs = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	print("clock: PM8150 RTC (channel %d): control %x (%s), count %d s\n", (uint64_t)channel, (uint64_t)ctrl,
	    (ctrl & 0x80) ? "running" : "NOT ENABLED", (uint64_t)*secs);
	return (ctrl & 0x80) != 0;
}

/*
 * "oem charger" (Test 86): the PM8150B charger's own registers, read through the observer channel as above (reads
 * only; the kernel never reads this PMIC's charger, so no channel is shared). Linux smb5-reg.h: CHGR 0x1000
 * (BATTERY_CHARGER_STATUS_1 0x06, CHARGING_ENABLE_CMD 0x42, FAST_CHARGE_CURRENT_CFG 0x61, FLOAT_VOLTAGE_CFG 0x70),
 * USBIN 0x1300 (APSD_STATUS 0x07, APSD_RESULT 0x08, CMD_IL 0x40, ICL_OPTIONS 0x66, CURRENT_LIMIT_CFG 0x70),
 * MISC 0x1600 (ICL_STATUS 0x07, POWER_PATH_STATUS 0x0b, WD_CFG 0x51), TYPEC 0x1500 (TYPE_C_STATUS 0x06..0x0b).
 * PM8150B is slave 2. The text is staged for get_staged.
 */
static char chg_text[1024];
static uint32_t chg_at;

static void
chg_put(const char *s)
{
	while (*s && chg_at < sizeof(chg_text) - 1) {
		chg_text[chg_at++] = *s++;
	}
	chg_text[chg_at] = 0;
}

static void
chg_hex(uint32_t v, int digits)
{
	char t[9];

	for (int i = digits - 1; i >= 0; i--, v >>= 4) {
		t[i] = "0123456789abcdef"[v & 15];
	}
	t[digits] = 0;
	chg_put(t);
}

static void
chg_regs(const char *name, uint32_t id, uint8_t reg, uint32_t count)
{
	uint32_t channel;
	uint8_t b[4];

	chg_put(name);
	chg_put(" ");
	chg_hex(id >> 8, 1);
	chg_put(":");
	chg_hex(((id & 0xff) << 8) | reg, 4);
	chg_put(" =");
	if (!r5q_spmi_channel(id, &channel)) {
		chg_put(" (no channel)\n");
		return;
	}
	for (uint32_t i = 0; i < count; i += 4) {
		uint32_t n = count - i > 4 ? 4 : count - i;

		if (!r5q_spmi_read(channel, (uint8_t)(reg + i), b, n)) {
			chg_put(" (read failed)");
			break;
		}
		for (uint32_t k = 0; k < n; k++) {
			chg_put(" ");
			chg_hex(b[k], 2);
		}
	}
	chg_put("\n");
}

static void
r5q_charger_report(void)
{
	chg_at = 0;
	chg_text[0] = 0;
	chg_regs("chgr status_1..2", 0x210, 0x06, 2);
	chg_regs("chgr enable_cmd", 0x210, 0x42, 1);
	chg_regs("chgr fcc_cfg", 0x210, 0x61, 1);
	chg_regs("chgr float_cfg", 0x210, 0x70, 1);
	chg_regs("chgr int_rt_sts", 0x210, 0x10, 1);
	chg_regs("usbin apsd_status,result", 0x213, 0x07, 2);
	chg_regs("usbin int_rt_sts", 0x213, 0x10, 1);
	chg_regs("usbin cmd_il", 0x213, 0x40, 1);
	chg_regs("usbin icl_options", 0x213, 0x66, 1);
	chg_regs("usbin icl_cfg", 0x213, 0x70, 1);
	chg_regs("misc icl_status", 0x216, 0x07, 1);
	chg_regs("misc power_path", 0x216, 0x0b, 1);
	chg_regs("misc int_rt_sts", 0x216, 0x10, 1);
	chg_regs("misc wd_cfg", 0x216, 0x51, 1);
	chg_regs("typec status", 0x215, 0x06, 6);
	chg_regs("batif int_rt_sts", 0x212, 0x10, 1);
	/* (Test 91) the Wi-Fi block's supplies (live DT icnss): status_1 (0x08, bit 7 ready), mode/en_ctl (0x45, 0x46) */
	chg_regs("wifi vdd-cx-mx pm8150 L1 status", 0x040, 0x08, 1);
	chg_regs("wifi vdd-cx-mx pm8150 L1 mode,en", 0x040, 0x45, 2);
	chg_regs("wifi vdd-1.8-xo pm8150 L7 status", 0x046, 0x08, 1);
	chg_regs("wifi vdd-1.8-xo pm8150 L7 mode,en", 0x046, 0x45, 2);
	chg_regs("wifi vdd-1.3-rfa pm8150l L2 status", 0x441, 0x08, 1);
	chg_regs("wifi vdd-1.3-rfa pm8150l L2 mode,en", 0x441, 0x45, 2);
	chg_regs("wifi vdd-3.3-ch0 pm8150l L11 status", 0x44a, 0x08, 1);
	chg_regs("wifi vdd-3.3-ch0 pm8150l L11 mode,en", 0x44a, 0x45, 2);
}

static int
r5q_anchor_read(struct r5q_time_anchor *a)
{
	efi_block_io *bio = maint_part("userdata");
	static uint8_t block[4096] __attribute__((aligned(64)));

	if (bio == NULL || bio->media->block_size != 4096 ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, R5Q_TIME_LBA, 4096, block))) {
		return 0;
	}
	memcpy(a, block, sizeof(*a));
	return memcmp(a->magic, R5Q_TIME_MAGIC, 16) == 0 && a->sum == r5q_anchor_sum(a);
}

/* at the start: the wall time from the anchor and the RTC (or 0) */
static void
r5q_clock_start(void)
{
	struct r5q_time_anchor a;

	r5q_rtc_valid = r5q_rtc_read(&r5q_rtc_seen);
	if (!r5q_anchor_read(&a)) {
		print("clock: no time anchor in USERDATA yet (python phone.py settime sets one)\n");
		return;
	}
	if (!r5q_rtc_valid || !a.rtc_ok || r5q_rtc_seen < a.rtc) {
		print("clock: anchor %d (RTC %d) but the RTC now says %d: not usable\n", a.wall, (uint64_t)a.rtc,
		    (uint64_t)r5q_rtc_seen);
		return;
	}
	r5q_wall_now = a.wall + (r5q_rtc_seen - a.rtc);
	print("clock: %d = anchor %d + %d s by the RTC\n", r5q_wall_now, a.wall, (uint64_t)(r5q_rtc_seen - a.rtc));
}

/* "oem settime SECONDS" (decimal, UTC): a new anchor, and this start's time */
static int
r5q_clock_set(uint64_t wall)
{
	efi_block_io *bio = maint_part("userdata");
	static uint8_t block[4096] __attribute__((aligned(64)));
	struct r5q_time_anchor a;
	efi_status s;

	if (bio == NULL || bio->media->block_size != 4096) {
		return 0;
	}
	memset(&a, 0, sizeof(a));
	if (r5q_anchor_read(&a)) {
		a.set_count++;
	} else {
		memset(&a, 0, sizeof(a));
		a.set_count = 1;
	}
	memcpy(a.magic, R5Q_TIME_MAGIC, 16);
	r5q_rtc_valid = r5q_rtc_read(&r5q_rtc_seen);
	a.wall = wall;
	a.rtc = r5q_rtc_seen;
	a.rtc_ok = (uint32_t)r5q_rtc_valid;
	a.sum = r5q_anchor_sum(&a);
	memset(block, 0, sizeof(block));
	memcpy(block, &a, sizeof(a));
	s = bio->write_blocks(bio, bio->media->media_id, R5Q_TIME_LBA, 4096, block);
	if (!EFI_ERROR(s)) {
		s = bio->flush_blocks(bio);
	}
	r5q_wall_now = wall;
	print("clock: set to %d by the PC (RTC %d, %s); anchor %s\n", wall, (uint64_t)a.rtc,
	    r5q_rtc_valid ? "running" : "not usable", EFI_ERROR(s) ? "NOT WRITTEN" : "written");
	return !EFI_ERROR(s);
}

/* the payload's "epoch=<10 digits>" made this start's wall time (only ever forward of what it says) */
static void
r5q_clock_patch(char *cmdline)
{
	for (char *c = cmdline; *c; c++) {
		uint64_t had = 0, v = r5q_wall_now;
		int i;

		if (!starts_with(c, "epoch=") || (c != cmdline && c[-1] != ' ')) {
			continue;
		}
		for (i = 0; i < 10 && c[6 + i] >= '0' && c[6 + i] <= '9'; i++) {
			had = had * 10 + (uint64_t)(c[6 + i] - '0');
		}
		if (i != 10 || (c[16] != 0 && c[16] != ' ')) {
			print("clock: epoch= is not 10 digits: left alone\n");
			return;
		}
		if (v <= had || v > 9999999999ULL) {
			print("clock: epoch=%d kept (nothing later known)\n", had);
			return;
		}
		for (i = 9; i >= 0; i--) {
			c[6 + i] = (char)('0' + v % 10);
			v /= 10;
		}
		print("clock: epoch=%d (was %d)\n", r5q_wall_now, had);
		return;
	}
}

/*
 * r5q (Test 82): boot-args changed from the PC without a new BOOT image ("live patching"). Kept in USERDATA, the block
 * after the time anchor; put in at every start after the payload's own, and a key given here replaces the payload's
 * (a token "-key" just takes the payload's away). The PC sets them in the listening window, before they apply, so a
 * bad set is always undone the same way: python phone.py args --clear.
 */
#define R5Q_ARGS_LBA            (R5Q_TIME_LBA + 1U)
#define R5Q_ARGS_MAGIC          "R5Q-EXTRA-ARGS/1"
#define R5Q_ARGS_MAX            400U

struct r5q_extra_args {
	char magic[16];
	uint32_t len;
	uint32_t reserved;
	uint64_t sum;
	char text[R5Q_ARGS_MAX + 1];
};

static struct r5q_extra_args r5q_args;
static char r5q_cmdline_merged[1024];

static uint64_t
r5q_args_sum(const struct r5q_extra_args *a)
{
	uint64_t h = 1469598103934665603ULL;

	for (uint32_t i = 0; i < a->len && i < R5Q_ARGS_MAX; i++) {
		h = (h ^ (uint8_t)a->text[i]) * 1099511628211ULL;
	}
	return h ^ a->len;
}

static int
r5q_args_load(void)
{
	efi_block_io *bio = maint_part("userdata");
	static uint8_t block[4096] __attribute__((aligned(64)));

	memset(&r5q_args, 0, sizeof(r5q_args));
	if (bio == NULL || bio->media->block_size != 4096 ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, R5Q_ARGS_LBA, 4096, block))) {
		return 0;
	}
	memcpy(&r5q_args, block, sizeof(r5q_args));
	if (memcmp(r5q_args.magic, R5Q_ARGS_MAGIC, 16) != 0 || r5q_args.len > R5Q_ARGS_MAX ||
	    r5q_args.sum != r5q_args_sum(&r5q_args)) {
		memset(&r5q_args, 0, sizeof(r5q_args));
		return 0;
	}
	r5q_args.text[r5q_args.len] = 0;
	return 1;
}

static int
r5q_args_store(const char *text, uint64_t len)
{
	efi_block_io *bio = maint_part("userdata");
	static uint8_t block[4096] __attribute__((aligned(64)));
	efi_status s;

	if (bio == NULL || bio->media->block_size != 4096 || len > R5Q_ARGS_MAX) {
		return 0;
	}
	memset(&r5q_args, 0, sizeof(r5q_args));
	memcpy(r5q_args.magic, R5Q_ARGS_MAGIC, 16);
	for (uint64_t i = 0; i < len; i++) {           /* one line of printable text; anything else a space */
		char c = text[i];
		r5q_args.text[i] = (c >= 0x21 && c <= 0x7e) ? c : ' ';
	}
	r5q_args.len = (uint32_t)len;
	r5q_args.sum = r5q_args_sum(&r5q_args);
	memset(block, 0, sizeof(block));
	memcpy(block, &r5q_args, sizeof(r5q_args));
	s = bio->write_blocks(bio, bio->media->media_id, R5Q_ARGS_LBA, 4096, block);
	if (!EFI_ERROR(s)) {
		s = bio->flush_blocks(bio);
	}
	print("args: %s: '%s'\n", EFI_ERROR(s) ? "NOT WRITTEN" : "kept", r5q_args.text);
	return !EFI_ERROR(s);
}

/* the key of the token at t: up to '=' or the end of the token */
static uint32_t
r5q_key_len(const char *t)
{
	uint32_t n = 0;

	while (t[n] != 0 && t[n] != ' ' && t[n] != '=') {
		n++;
	}
	return n;
}

/* whether the extra args name this key (as "key", "key=..." or "-key") */
static int
r5q_args_name(const char *key, uint32_t klen)
{
	const char *t = r5q_args.text;

	while (*t) {
		while (*t == ' ') {
			t++;
		}
		if (*t == 0) {
			break;
		}
		const char *k = (*t == '-') ? t + 1 : t;
		uint32_t n = r5q_key_len(k);

		if (n == klen && memcmp(k, key, n) == 0) {
			return 1;
		}
		while (*t && *t != ' ') {
			t++;
		}
	}
	return 0;
}

/* the payload's cmdline with the extra args put in; the payload's own when there are none */
static const char *
r5q_args_merge(const char *base)
{
	uint32_t n = 0;
	const char *t = base;

	if (r5q_args.len == 0) {
		return base;
	}
	while (*t) {
		while (*t == ' ') {
			t++;
		}
		if (*t == 0) {
			break;
		}
		uint32_t klen = r5q_key_len(t), tlen = 0;

		while (t[tlen] && t[tlen] != ' ') {
			tlen++;
		}
		if (!r5q_args_name(t, klen) && n + tlen + 1 < sizeof(r5q_cmdline_merged) - R5Q_ARGS_MAX - 2) {
			memcpy(r5q_cmdline_merged + n, t, tlen);
			n += tlen;
			r5q_cmdline_merged[n++] = ' ';
		}
		t += tlen;
	}
	for (t = r5q_args.text; *t; ) {
		uint32_t tlen = 0;

		while (*t == ' ') {
			t++;
		}
		while (t[tlen] && t[tlen] != ' ') {
			tlen++;
		}
		if (tlen != 0 && *t != '-') {
			memcpy(r5q_cmdline_merged + n, t, tlen);
			n += tlen;
			r5q_cmdline_merged[n++] = ' ';
		}
		t += tlen;
	}
	if (n > 0) {
		n--;
	}
	r5q_cmdline_merged[n] = 0;
	print("args: the kernel's arguments with the PC's: %s\n", r5q_cmdline_merged);
	return r5q_cmdline_merged;
}

/*
 * r5q (Test 83): the modem (MPSS), started from here, before the kernel: Wi-Fi on this SoC is run by the modem's DSP
 * (work/ios-r5q/wifi/WIFI-PLAN.md, stage 4). Boot-arg modem=1: its image is read from the phone's own "modem" partition
 * (FAT16: image/modem.mdt and image/modem.bNN) and checked against its header, nothing else; modem=2: then started, as
 * Linux's qcom_q6v5_pas does it for qcom,pas-id 4 with qcom,signal-aop: the always-on processor told "load_state on"
 * through its mailbox (message RAM 0x0c300000, doorbell bit 0 of 0x17c0000c), TrustZone given the header (PAS
 * init_image), the place (mem_setup: the live DT's modem_region, 0x8d800000, 160 MB), the segments copied there, and
 * TrustZone asked to check them and let the modem out of reset (auth_and_reset). modem=2 runs only with a PC on the
 * cable: if it takes the phone down, the next start's listening window is where it is undone (phone.py args --clear).
 * No PMIC is touched; the AOP message and the TrustZone calls are the same requests Android makes.
 */
#define MPSS_PAS_ID             4
#define MPSS_MEM                0x8d800000ULL
#define MPSS_MEM_SIZE           0x0a000000ULL
#define MDT_MAX_SEGMENTS        64

static uint32_t modem_load_ms;
static volatile int modem_running, modem_stopped;      /* (Test 97: oem modemstop, then peeks of its memory) */

struct fat16 {
	efi_block_io *bio;
	uint32_t bs, spc, fat_start, fat_sectors, root_start, root_sectors, data_start, clusters;
	uint16_t *fat;
};

struct fat_file {
	uint32_t cluster, size;
};

static uint8_t fat_bounce[64 * 1024] __attribute__((aligned(4096)));

static int
fat_open(struct fat16 *f, const char *partition)
{
	uint8_t *b = fat_bounce;
	uint64_t a = 0;
	uint32_t nfats, root_entries, total;

	memset(f, 0, sizeof(*f));
	f->bio = maint_part(partition);
	if (f->bio == NULL || f->bio->media->block_size > sizeof(fat_bounce) ||
	    EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id, 0, f->bio->media->block_size, b))) {
		print("modem: no partition '%s' to read\n", partition);
		return 0;
	}
	f->bs = b[11] | b[12] << 8;
	f->spc = b[13];
	nfats = b[16];
	root_entries = b[17] | b[18] << 8;
	f->fat_sectors = b[22] | b[23] << 8;
	total = (b[19] | b[20] << 8) ? (uint32_t)(b[19] | b[20] << 8) : (uint32_t)(b[32] | b[33] << 8 | b[34] << 16 | (uint32_t)b[35] << 24);
	f->fat_start = b[14] | b[15] << 8;
	if (f->bs != f->bio->media->block_size || f->spc == 0 || nfats == 0 || f->fat_sectors == 0 || b[510] != 0x55 || b[511] != 0xaa) {
		print("modem: '%s' is not FAT16 as expected (bytes/sector %d, sectors/cluster %d, FATs %d)\n", partition,
		    (uint64_t)f->bs, (uint64_t)f->spc, (uint64_t)nfats);
		return 0;
	}
	f->root_start = f->fat_start + nfats * f->fat_sectors;
	f->root_sectors = (root_entries * 32 + f->bs - 1) / f->bs;
	f->data_start = f->root_start + f->root_sectors;
	f->clusters = (total - f->data_start) / f->spc + 2;
	if (f->clusters >= 65525 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData,
	    ((uint64_t)f->fat_sectors * f->bs + 4095) >> 12, &a))) {
		return 0;
	}
	f->fat = (uint16_t *)(uintptr_t)a;
	if (EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id, f->fat_start, (uint64_t)f->fat_sectors * f->bs, f->fat))) {
		return 0;
	}
	return 1;
}

static uint32_t
fat_next(const struct fat16 *f, uint32_t c)
{
	return (c >= 2 && c < f->clusters) ? f->fat[c] : 0xffff;
}

/* an 8.3 name ("MODEM   MDT") in a directory: the root (cluster 0) or a subdirectory's chain */
static int
fat_find(struct fat16 *f, uint32_t dir_cluster, const char name83[11], struct fat_file *out)
{
	uint32_t cluster = dir_cluster, sector = 0, sectors = dir_cluster == 0 ? f->root_sectors : f->spc;

	for (;;) {
		for (uint32_t s = 0; s < sectors; s++) {
			uint64_t lba = dir_cluster == 0 ? f->root_start + s : f->data_start + (uint64_t)(cluster - 2) * f->spc + s;

			if (EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id, lba, f->bs, fat_bounce))) {
				return 0;
			}
			for (uint32_t e = 0; e < f->bs; e += 32) {
				const uint8_t *d = fat_bounce + e;

				if (d[0] == 0) {
					return 0;
				}
				if (d[0] != 0xe5 && (d[11] & 0x0f) != 0x0f && memcmp(d, name83, 11) == 0) {
					out->cluster = d[26] | d[27] << 8;
					out->size = d[28] | d[29] << 8 | d[30] << 16 | (uint32_t)d[31] << 24;
					return 1;
				}
			}
		}
		(void)sector;
		if (dir_cluster == 0) {
			return 0;
		}
		cluster = fat_next(f, cluster);
		if (cluster < 2 || cluster >= 0xfff8) {
			return 0;
		}
	}
}

/*
 * (Test 139) The same by the long (VFAT) name, case ignored: apnhlos, where the audio DSP's image is, was made by a
 * tool that gives every name a made-up 8.3 alias ("PCAFUI~2" for image/), so only the long names say what is what.
 * A long name is the LFN entries before its short entry (13 UCS-2 characters each, ordinal in bits 4:0), kept across
 * sectors and clusters; ASCII only.
 */
static int
fat_find_long(struct fat16 *f, uint32_t dir_cluster, const char *want, struct fat_file *out)
{
	uint32_t cluster = dir_cluster, sectors = dir_cluster == 0 ? f->root_sectors : f->spc;
	static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	char name[260];
	int have = 0;

	for (;;) {
		for (uint32_t s = 0; s < sectors; s++) {
			uint64_t lba = dir_cluster == 0 ? f->root_start + s : f->data_start + (uint64_t)(cluster - 2) * f->spc + s;

			if (EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id, lba, f->bs, fat_bounce))) {
				return 0;
			}
			for (uint32_t e = 0; e < f->bs; e += 32) {
				const uint8_t *d = fat_bounce + e;

				if (d[0] == 0) {
					return 0;
				}
				if (d[0] == 0xe5) {
					have = 0;
					continue;
				}
				if ((d[11] & 0x3f) == 0x0f) {           /* a piece of a long name */
					uint32_t ord = d[0] & 0x1f;

					if (d[0] & 0x40) {
						memset(name, 0, sizeof(name));
					}
					if (ord == 0 || ord > 19) {
						have = 0;
						continue;
					}
					for (uint32_t k = 0; k < 13; k++) {
						uint16_t c = d[at[k]] | d[at[k] + 1] << 8;

						name[(ord - 1) * 13 + k] = (c == 0 || c == 0xffff) ? 0 : (c < 128 ? (char)c : '?');
					}
					have = 1;
					continue;
				}
				if (have) {
					uint32_t k = 0;

					while (want[k] && name[k] && (want[k] | 0x20) == (name[k] | 0x20)) {
						k++;
					}
					if (want[k] == 0 && name[k] == 0) {
						out->cluster = d[26] | d[27] << 8;
						out->size = d[28] | d[29] << 8 | d[30] << 16 | (uint32_t)d[31] << 24;
						return 1;
					}
				}
				have = 0;
			}
		}
		if (dir_cluster == 0) {
			return 0;
		}
		cluster = fat_next(f, cluster);
		if (cluster < 2 || cluster >= 0xfff8) {
			return 0;
		}
	}
}

/* by the 8.3 name, or else by the long one ("base.ext", or "base" when ext is empty) */
static void name83(const char *base, const char *ext, char out[11]);
static int
fat_find_name(struct fat16 *f, uint32_t dir_cluster, const char *base, const char *ext, struct fat_file *out)
{
	char n83[11], longname[64];
	uint32_t k = 0;

	name83(base, ext, n83);
	if (fat_find(f, dir_cluster, n83, out)) {
		return 1;
	}
	for (uint32_t j = 0; base[j] && k < 40; j++) {
		longname[k++] = base[j];
	}
	if (ext[0]) {
		longname[k++] = '.';
		for (uint32_t j = 0; ext[j] && k < 60; j++) {
			longname[k++] = ext[j];
		}
	}
	longname[k] = 0;
	return fat_find_long(f, dir_cluster, longname, out);
}

/* a file's first `len` bytes (at most its size) to `dest`: runs of clusters read whole, the last partial one bounced */
static int
fat_read(struct fat16 *f, const struct fat_file *file, uint8_t *dest, uint32_t len)
{
	uint32_t cs = f->bs * f->spc, done = 0, c = file->cluster;

	if (len > file->size) {
		len = file->size;
	}
	while (done < len) {
		uint32_t run = 1, first = c;

		if (c < 2 || c >= f->clusters) {
			print("modem: a broken cluster chain\n");
			return 0;
		}
		while ((done + run * cs) < len && fat_next(f, c) == c + 1) {
			c++;
			run++;
		}
		uint64_t lba = f->data_start + (uint64_t)(first - 2) * f->spc;
		uint32_t whole = len - done >= run * cs ? run : (len - done) / cs;

		if (whole != 0 && EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id, lba, (uint64_t)whole * cs, dest + done))) {
			return 0;
		}
		done += whole * cs;
		if (whole < run) {               /* the tail: one cluster through the bounce buffer */
			if (cs > sizeof(fat_bounce) || EFI_ERROR(f->bio->read_blocks(f->bio, f->bio->media->media_id,
			    lba + (uint64_t)whole * f->spc, cs, fat_bounce))) {
				return 0;
			}
			memcpy(dest + done, fat_bounce, len - done);
			done = len;
			break;
		}
		c = fat_next(f, c);
	}
	return 1;
}

/* "modem.b07" -> "MODEM   B07" */
static void
name83(const char *base, const char *ext, char out[11])
{
	int i;

	memset(out, ' ', 11);
	for (i = 0; i < 8 && base[i]; i++) {
		out[i] = (base[i] >= 'a' && base[i] <= 'z') ? (char)(base[i] - 32) : base[i];
	}
	for (i = 0; i < 3 && ext[i]; i++) {
		out[8 + i] = (ext[i] >= 'a' && ext[i] <= 'z') ? (char)(ext[i] - 32) : ext[i];
	}
}

/* TrustZone, as Linux's qcom_scm-smc.c calls it (and the donor's redwood_wifi.c, proven on SM7325) */
static int64_t
scm_call(const char *what, uint64_t function, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *answer)
{
	uint64_t session = 0;

	function |= (1ULL << 30) | (2ULL << 24);
	print("modem: TrustZone %s: smc %x (%x, %x, %x)\n", what, function, a, b, c);
	flush_log();
	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : 1;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = a;
		register uint64_t x3 __asm__("x3") = b;
		register uint64_t x4 __asm__("x4") = c;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != 1) {
			*answer = x1;
			print("modem: TrustZone returns %x, answer %x\n", x0, x1);
			return (int64_t)x0;
		}
	}
	print("modem: TrustZone keeps saying the call was interrupted\n");
	return -1;
}

/*
 * r5q (Test 88): the modem's remote file system memory (rmtfs). Live DT qcom,rmtfs_sharedmem: 2 MB that the AP picks
 * (reg <0 0x200000>, client 1, guard pages), given to the modem by TrustZone's memory assignment (Linux
 * qcom_scm_assign_mem: HLOS VMID 3 -> HLOS and MSS_MSA VMID 15, read-write), whose address the kernel's rmtfs server
 * (bsd/dev/redwood_wifi.c) hands the modem when it asks (QMI alloc_buff). Below 4 GB: the modem is 32-bit.
 */
#define RMTFS_SIZE              0x200000ULL
static uint64_t r5q_rmtfs_pa;

/* a TrustZone call with more than four arguments: the fourth on are in memory, x5 its address (qcom_scm-smc.c) */
static int64_t
scm_call_args(const char *what, uint64_t function, uint64_t arginfo, const uint64_t *args, int nargs, uint64_t *answer)
{
	static uint64_t ext[8] __attribute__((aligned(64)));
	uint64_t session = 0, x5v;

	memset(ext, 0, sizeof(ext));
	for (int i = 3; i < nargs && i < 11; i++) {
		ext[i - 3] = args[i];
	}
	clean_dcache((uint64_t)(uintptr_t)ext, (uint64_t)(uintptr_t)ext + sizeof(ext));
	x5v = nargs > 4 ? (uint64_t)(uintptr_t)ext : (nargs > 3 ? args[3] : 0);
	function |= (1ULL << 30) | (2ULL << 24);
	print("modem: TrustZone %s: smc %x (%d arguments)\n", what, function, (uint64_t)nargs);
	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : 1;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = nargs > 0 ? args[0] : 0;
		register uint64_t x3 __asm__("x3") = nargs > 1 ? args[1] : 0;
		register uint64_t x4 __asm__("x4") = nargs > 2 ? args[2] : 0;
		register uint64_t x5 __asm__("x5") = x5v;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != 1) {
			*answer = x1;
			print("modem: TrustZone returns %x, answer %x\n", x0, x1);
			return (int64_t)x0;
		}
	}
	return -1;
}

static void
r5q_rmtfs_setup(void)
{
	static uint64_t map[2] __attribute__((aligned(64)));
	static uint32_t src[2] __attribute__((aligned(64)));
	static struct {
		uint32_t vmid, perm;
		uint64_t ctx;
		uint32_t ctx_size, unused;
	} dest[2] __attribute__((aligned(64)));
	uint64_t a = 0xffffffffULL, answer = ~0ULL, args[7];
	int64_t r;

	/* 2 MB with a guard page each side (qcom,guard-memory), 2 MB aligned inside */
	if (EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, (RMTFS_SIZE + 0x400000ULL) >> 12, &a))) {
		print("modem: no memory below 4 GB for rmtfs\n");
		return;
	}
	r5q_rmtfs_pa = (a + 0x1fffffULL + 0x1000ULL) & ~0x1fffffULL;
	memset((void *)(uintptr_t)r5q_rmtfs_pa, 0, RMTFS_SIZE);
	clean_dcache(r5q_rmtfs_pa, r5q_rmtfs_pa + RMTFS_SIZE);
	map[0] = r5q_rmtfs_pa;
	map[1] = RMTFS_SIZE;
	src[0] = 3;                                     /* HLOS */
	dest[0].vmid = 3;
	dest[0].perm = 6;                               /* read, write */
	dest[1].vmid = 15;                              /* MSS_MSA */
	dest[1].perm = 6;
	clean_dcache((uint64_t)(uintptr_t)map, (uint64_t)(uintptr_t)map + sizeof(map));
	clean_dcache((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)src + sizeof(src));
	clean_dcache((uint64_t)(uintptr_t)dest, (uint64_t)(uintptr_t)dest + sizeof(dest));
	args[0] = (uint64_t)(uintptr_t)map;
	args[1] = sizeof(map);
	args[2] = (uint64_t)(uintptr_t)src;
	args[3] = 4;
	args[4] = (uint64_t)(uintptr_t)dest;
	args[5] = sizeof(dest);
	args[6] = 0;
	r = scm_call_args("assign_mem (rmtfs to HLOS + MSS)", 0x0c16, 0x1117, args, 7, &answer);
	if (r != 0 || answer != 0) {
		print("modem: TRUSTZONE DOES NOT ASSIGN THE RMTFS MEMORY (%x, %x): no rmtfs\n", (uint64_t)r, answer);
		r5q_rmtfs_pa = 0;
		return;
	}
	print("modem: rmtfs memory at %x, 2 MB, shared with the modem (VMID 15)\n", r5q_rmtfs_pa);
}

/* the always-on processor's mailbox (Linux qcom_aoss.c qmp_open/qmp_send) */
#define QMP_BASE                0x0c300000ULL
#define QMP_DOORBELL            0x17c0000cULL

static int
qmp_wait(uint32_t off, uint32_t want, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (mmio_read32(QMP_BASE + off) == want) {
			return 1;
		}
		BS->stall(100);
	}
	return mmio_read32(QMP_BASE + off) == want;
}

static uint32_t qmp_box;

static int
qmp_open(void)
{
	const uint32_t up = 0x0000ffff;

	if (mmio_read32(QMP_BASE + 0x0) != 0x4d41494c || mmio_read32(QMP_BASE + 0x4) != 1) {
		print("modem: the AOP mailbox is not one this knows (%x, version %x)\n", (uint64_t)mmio_read32(QMP_BASE),
		    (uint64_t)mmio_read32(QMP_BASE + 4));
		return 0;
	}
	qmp_box = mmio_read32(QMP_BASE + 0x38);
	if (mmio_read32(QMP_BASE + 0x34) < 68 || qmp_box < 0x3c || qmp_box > 0x1000 - 68) {
		print("modem: the AOP mailbox's box is not usable\n");
		qmp_box = 0;
		return 0;
	}
	if (mmio_read32(QMP_BASE + 0x24) == up && mmio_read32(QMP_BASE + 0x28) == up && mmio_read32(QMP_BASE + 0x2c) == up &&
	    mmio_read32(QMP_BASE + 0x14) == up && mmio_read32(QMP_BASE + 0x30) == up) {
		print("modem: the AOP mailbox is open already (the firmware opened it)\n");
		return 1;
	}
	mmio_write32(QMP_BASE + 0x10, mmio_read32(QMP_BASE + 0x0c));
	mmio_write32(QMP_BASE + 0x24, up);
	mmio_write32(QMP_DOORBELL, 1);
	if (!qmp_wait(0x28, up, 1000)) {
		print("modem: the AOP does not acknowledge our link\n");
		return 0;
	}
	mmio_write32(QMP_BASE + 0x2c, up);
	mmio_write32(QMP_DOORBELL, 1);
	if (!qmp_wait(0x14, up, 1000)) {
		print("modem: the AOP does not open its channel\n");
		return 0;
	}
	mmio_write32(QMP_BASE + 0x18, up);
	mmio_write32(QMP_DOORBELL, 1);
	if (!qmp_wait(0x30, up, 1000)) {
		print("modem: the AOP does not acknowledge our channel\n");
		return 0;
	}
	print("modem: the AOP mailbox is open\n");
	return 1;
}

static int
qmp_send(const char *text)
{
	uint32_t words[16];

	if (qmp_box == 0 || mmio_read32(QMP_BASE + qmp_box) != 0) {
		print("modem: the AOP mailbox is not free: '%s' not sent\n", text);
		return 0;
	}
	memset(words, 0, sizeof(words));
	memcpy(words, text, str_len(text) < sizeof(words) - 1 ? str_len(text) : sizeof(words) - 1);
	for (unsigned i = 0; i < 16; i++) {
		mmio_write32(QMP_BASE + qmp_box + 4 + 4 * i, words[i]);
	}
	mmio_write32(QMP_BASE + qmp_box, 64);
	(void)mmio_read32(QMP_BASE + qmp_box);
	mmio_write32(QMP_DOORBELL, 1);
	if (!qmp_wait(qmp_box, 0, 1000)) {
		mmio_write32(QMP_BASE + qmp_box, 0);
		print("modem: the AOP did not take '%s'\n", text);
		return 0;
	}
	print("modem: the AOP took '%s'\n", text);
	return 1;
}

struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

/*
 * A DSP of the SoC's, started through TrustZone's PAS from its signed image in the modem partition's image/ (Test 83 for
 * the modem; Test 139 the same for the audio DSP: sound's first step). who: the file name and the AOP's name for it
 * ("modem", "adsp"); pas_id: TrustZone's number for the image (4 the modem, 1 the audio DSP); mem, mem_size: its
 * reserved memory (the live DT's no-map region). start 0: read and check only; 1/2: and start it.
 */
static void
r5q_pas_boot(const char *who, const char *partition, uint32_t pas_id, uint64_t mem, uint64_t mem_size, int start)
{
	static char aop_on[80], aop_off[80];
	static char mdt_name[11];
	struct fat16 f;
	struct fat_file image_dir, mdt, seg;
	struct elf32_phdr ph[MDT_MAX_SEGMENTS];
	uint8_t *meta, *file;
	uint64_t a = 0, fa = 0, answer = ~0ULL, lo = ~0ULL, hi = 0, files = 0, t0 = READ_SYSREG(CNTVCT_EL0);
	uint32_t count, hash = ~0u, meta_len;
	int relocatable = 0;
	int64_t r;

	/*
	 * (Test 124) modem=3: started with or without a PC on the cable -- for Wi-Fi on a phone in a pocket. Tests 85-123
	 * started it every time without a fault; a start that went wrong is still undone from the next start's listening
	 * window with a PC (it comes before this), or from Download Mode.
	 */
	print("%s: %s\n", who, start == 2 ? "STARTING IT (with or without a PC)" : start ? "STARTING IT (only with a PC)" :
	    "reading its image (a check only)");
	if (start == 1 && !usb_connected) {
		print("modem: not started: modem=2 runs only with a PC on the cable (so a bad start can be undone)\n");
		start = 0;
	}
	{
		const char *head = "{class: image, res: load_state, name: ", *on = ", val: on}", *off = ", val: off}";
		uint32_t k = 0, j;

		for (j = 0; head[j]; j++) {
			aop_on[k] = aop_off[k] = head[j];
			k++;
		}
		for (j = 0; who[j] && k < 60; j++) {
			aop_on[k] = aop_off[k] = who[j];
			k++;
		}
		for (j = 0; on[j]; j++) {
			aop_on[k + j] = on[j];
		}
		aop_on[k + j] = 0;
		for (j = 0; off[j]; j++) {
			aop_off[k + j] = off[j];
		}
		aop_off[k + j] = 0;
	}
	name83("image", "", mdt_name);
	if (!fat_open(&f, partition) || !fat_find_name(&f, 0, "image", "", &image_dir)) {
		print("modem: no image/ directory in the modem partition\n");
		return;
	}
	name83(who, "mdt", mdt_name);
	a = 0xffffffffULL;                      /* (TrustZone reads the metadata: below 4 GB, in one piece) */
	if (!fat_find_name(&f, image_dir.cluster, who, "mdt", &mdt)) {
		print("%s: image/%s.mdt not found in the modem partition\n", who, who);
		return;
	}
	if (mdt.size < 52 || mdt.size > 1024 * 1024) {
		print("%s: its .mdt has an odd size (%d bytes)\n", who, (uint64_t)mdt.size);
		return;
	}
	if (EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (mdt.size + 4095) >> 12, &fa)) ||
	    EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, (1024 * 1024) >> 12, &a))) {
		print("%s: no memory for its .mdt and metadata\n", who);
		return;
	}
	if (!fat_read(&f, &mdt, (file = (uint8_t *)(uintptr_t)fa), mdt.size)) {
		print("%s: its .mdt (%d bytes) not read\n", who, (uint64_t)mdt.size);
		return;
	}
	meta = (uint8_t *)(uintptr_t)a;
	count = file[44] | file[45] << 8;
	if (memcmp(file, "\177ELF\1", 5) != 0 || count == 0 || count > MDT_MAX_SEGMENTS || 52 + 32 * count > mdt.size) {
		print("modem: modem.mdt is not a 32-bit ELF header (%d segments, %d bytes)\n", (uint64_t)count, (uint64_t)mdt.size);
		return;
	}
	memcpy(ph, file + 52, 32 * count);
	memcpy(meta, file, 52 + 32 * count);
	for (uint32_t i = 0; i < count; i++) {
		if (((ph[i].flags >> 24) & 7) == 2) {
			hash = i;
		}
		relocatable |= (ph[i].flags >> 27) & 1;
		if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || ph[i].memsz == 0) {
			continue;
		}
		lo = ph[i].paddr < lo ? ph[i].paddr : lo;
		hi = (uint64_t)ph[i].paddr + ph[i].memsz > hi ? (uint64_t)ph[i].paddr + ph[i].memsz : hi;
	}
	hi = (hi + 0xfff) & ~0xfffULL;
	/* the metadata: the header and segment table, then the hash segment (from the .mdt, or its own file) */
	meta_len = 52 + 32 * count;
	if (hash != ~0u) {
		if (ph[hash].filesz > 1024 * 1024 - meta_len) {
			print("modem: the hash segment is too big (%d bytes)\n", (uint64_t)ph[hash].filesz);
			return;
		}
		if ((uint64_t)ph[hash].offset + ph[hash].filesz <= mdt.size) {
			memcpy(meta + meta_len, file + ph[hash].offset, ph[hash].filesz);
		} else {
			char n[11], e[4] = { 'b', (char)('0' + hash / 10), (char)('0' + hash % 10), 0 };

			name83(who, e, n);
			if (!fat_find_name(&f, image_dir.cluster, who, e, &seg) || !fat_read(&f, &seg, meta + meta_len, ph[hash].filesz)) {
				print("modem: the hash segment's file is missing\n");
				return;
			}
		}
		meta_len += ph[hash].filesz;
	}
	print("modem: modem.mdt: %d bytes, %d segments, hash segment %d (%d bytes), %s; loadable %x..%x (%d MB)\n",
	    (uint64_t)mdt.size, (uint64_t)count, (uint64_t)hash, (uint64_t)(hash != ~0u ? ph[hash].filesz : 0),
	    relocatable ? "relocatable" : "fixed", lo, hi, (hi - lo) >> 20);
	if (hi - lo > mem_size || (!relocatable && (lo < mem || hi > mem + mem_size))) {
		print("modem: the image does not fit the modem's memory (%x, %x bytes)\n", mem, mem_size);
		return;
	}
	clean_dcache(a, a + ((meta_len + 4095) & ~4095u));

	if (start) {
		if (!qmp_open() || !qmp_send(aop_on)) {
			print("modem: not started: the AOP was not told\n");
			return;
		}
		r = scm_call("init_image (the header and its signature)", 0x201, 2 | (2 << 6), pas_id, a, 0, &answer);
		if (r != 0 || answer != 0) {
			print("modem: TRUSTZONE REFUSES THE MODEM'S HEADER (%x, %x)\n", (uint64_t)r, answer);
			qmp_send(aop_off);
			return;
		}
		print("modem: TRUSTZONE ACCEPTS THE MODEM'S SIGNATURE\n");
		if (relocatable) {
			r = scm_call("mem_setup (where it goes)", 0x202, 3, pas_id, mem, hi - lo, &answer);
			if (r != 0 || answer != 0) {
				print("modem: TRUSTZONE REFUSES THE PLACE (%x, %x)\n", (uint64_t)r, answer);
				scm_call("shutdown", 0x206, 1, pas_id, 0, 0, &answer);
				qmp_send(aop_off);
				return;
			}
		}
	}
	/* the segments, into the modem's memory (they go there in modem=1 as well: nobody else uses it) */
	for (uint32_t i = 0; i < count; i++) {
		uint64_t at;
		char n[11], e[4] = { 'b', (char)('0' + i / 10), (char)('0' + i % 10), 0 };

		if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || ph[i].memsz == 0) {
			continue;
		}
		at = relocatable ? mem + (ph[i].paddr - lo) : ph[i].paddr;
		if (ph[i].filesz > ph[i].memsz) {
			print("modem: segment %d: more in the file than in memory\n", (uint64_t)i);
			return;
		}
		if (ph[i].filesz != 0) {
			if ((uint64_t)ph[i].offset + ph[i].filesz <= mdt.size) {
				memcpy((void *)(uintptr_t)at, file + ph[i].offset, ph[i].filesz);   /* (an .mdt that is the whole ELF) */
			} else {
				name83(who, e, n);
				if (!fat_find_name(&f, image_dir.cluster, who, e, &seg) || seg.size < ph[i].filesz ||
				    !fat_read(&f, &seg, (uint8_t *)(uintptr_t)at, ph[i].filesz)) {
					print("modem: segment %d: modem.%s missing or short\n", (uint64_t)i, e);
					return;
				}
			}
			files += ph[i].filesz;
		}
		if (ph[i].memsz > ph[i].filesz) {
			memset((void *)(uintptr_t)(at + ph[i].filesz), 0, ph[i].memsz - ph[i].filesz);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	modem_load_ms = (uint32_t)((READ_SYSREG(CNTVCT_EL0) - t0) * 1000 / READ_SYSREG(CNTFRQ_EL0));
	print("modem: the image is in the modem's memory: %d MB from %d files in %d ms\n", files >> 20, (uint64_t)count,
	    (uint64_t)modem_load_ms);
	if (!start) {
		return;
	}
	if (pas_id == MPSS_PAS_ID) {
		r5q_rmtfs_setup();                      /* (Test 88: its EFS memory, before it starts and asks for it) */
	/* (Test 96) the QDSS (debug trace) clock the Wi-Fi firmware writes to, voted with the AOP as Linux's sm8150 wifi node
	 * asks for it ("qdss" from aoss_qmp; qcom_aoss.c's clock message): a request to the AOP, no register of ours */
	print("modem: the QDSS clock asked of the AOP: %s\n", qmp_send("{class: clock, res: qdss, val: 1}") ? "taken" : "NOT TAKEN");
	}
	if (start == 3) {
		/* (Test 212) slpi=5: everything but the release -- the kernel calls auth_and_reset itself once its FastRPC
		 * listener is ready to serve the SLPI's start-up (its sensor registry is read through it then, and only then) */
		print("%s: image in place and accepted; NOT released: the kernel will (auth_and_reset, PAS %d)\n", who,
		    (uint64_t)pas_id);
		return;
	}
	r = scm_call("auth_and_reset (check it all, let it out of reset)", 0x205, 1, pas_id, 0, 0, &answer);
	if (r != 0 || answer != 0) {
		print("modem: TRUSTZONE DOES NOT START THE MODEM (%x, %x)\n", (uint64_t)r, answer);
		scm_call("shutdown", 0x206, 1, pas_id, 0, 0, &answer);
		qmp_send(aop_off);
		return;
	}
	if (pas_id == MPSS_PAS_ID) {
		print("modem: THE MODEM IS OUT OF RESET (TrustZone checked its image); the kernel looks for its SMP2P item 428\n");
		modem_running = 1;
	} else {
		print("%s: OUT OF RESET (TrustZone checked its image, PAS %d)\n", who, (uint64_t)pas_id);
	}
}

/* modem=1: read and check; modem=2: and start it (modem=3: without a PC as well) */
static void
r5q_modem_boot(int start)
{
	r5q_pas_boot("modem", "modem", MPSS_PAS_ID, MPSS_MEM, MPSS_MEM_SIZE, start);
}

/* (Test 139) adsp=1: the audio DSP's image read and checked; adsp=2: and started (live DT pil_adsp_region) */
#define ADSP_PAS_ID             1
#define ADSP_MEM                0x99300000ULL
#define ADSP_MEM_SIZE           0x02700000ULL

static void
r5q_adsp_boot(int start)
{
	r5q_pas_boot("adsp", "apnhlos", ADSP_PAS_ID, ADSP_MEM, ADSP_MEM_SIZE, start ? 2 : 0);
}

/*
 * (Test 200) slpi=1: the sensors' DSP image (image/slpi.mdt in the apnhlos partition) read and checked; slpi=2: and
 * started. Live DT qcom,ssc@5c00000: "qcom,pil-tz-generic", pas-id 12, firmware "slpi", memory pil_slpi_region
 * 0x8c400000 + 20 MB, its rails vdd_cx/vdd_mx (on while the system runs) and sensor_vdd (1.8 V, the sensors').
 * The motion sensors are behind it (qcom,msm-ssc-sensors: Qualcomm's SEE); nothing on the AP's own buses.
 */
#define SLPI_PAS_ID             12
#define SLPI_MEM                0x8c400000ULL
#define SLPI_MEM_SIZE           0x01400000ULL

static void
r5q_slpi_boot(int start)
{
	r5q_pas_boot("slpi", "apnhlos", SLPI_PAS_ID, SLPI_MEM, SLPI_MEM_SIZE, start >= 5 ? 3 : start ? 2 : 0);
}

/*
 * Test 100: the S2MU106 charger's I2C (QUP0 SE4, i2c@890000) driven in FIFO mode from here, as Linux's i2c-qcom-geni
 * and qcom-geni-se drive it: TrustZone keeps the pins GPIO51/52 (a read of their TLMM registers reset the phone, Test
 * 87) and has them muxed to the engine, which is the AP's (Android's i2c@890000 is an AP driver).
 */
#define SE4                     0x00890000ULL
static int i2c4_ready;

static void
spin_us(uint32_t us)
{
	uint64_t until = READ_SYSREG(CNTVCT_EL0) + READ_SYSREG(CNTFRQ_EL0) * us / 1000000;

	while (READ_SYSREG(CNTVCT_EL0) < until) {
	}
}

static int
qup0_clocks_on(void)
{
	uint32_t m = mmio_read32(0x00117004ULL), s = mmio_read32(0x00117008ULL), e = mmio_read32(0x00117604ULL);

	/* (s_ahb is hardware-gated, bit 1: its "off" bit says nothing then, as Linux's clk-branch treats it) */
	return !(m & 0x80000000u) && (!(s & 0x80000000u) || (s & 2)) && !(e & 0x80000000u);
}

/* the engine checked and set up for I2C in FIFO mode, 100 kHz from the 19.2 MHz source; a reason if it cannot be */
static const char *
i2c4_init(void)
{
	uint32_t fw, ifdis, hw;

	if (!qup0_clocks_on()) {
		return "QUP0 clocks off (oem qupclk first)";
	}
	fw = mmio_read32(SE4 + 0x68);           /* GENI_FW_REVISION_RO: protocol in bits 15:8 */
	ifdis = mmio_read32(SE4 + 0x64);        /* GENI_IF_DISABLE_RO: bit 0 FIFO interface disabled */
	hw = mmio_read32(SE4 + 0xe24);          /* SE_HW_PARAM_0 */
	print("i2c4: fw %x (protocol %d), if_disable %x, hw_param0 %x, status %x\n", (uint64_t)fw, (uint64_t)((fw >> 8) & 0xff),
	    (uint64_t)ifdis, (uint64_t)hw, (uint64_t)mmio_read32(SE4 + 0x40));
	if (((fw >> 8) & 0xff) != 3) {
		return "SE4 does not run the I2C protocol";
	}
	if (ifdis & 1) {
		return "SE4 is GSI-only (FIFO interface disabled)";
	}
	mmio_write32(SE4 + 0x28, mmio_read32(SE4 + 0x28) | 0x7f);      /* GENI_CGC_CTRL: DEFAULT_CGC_EN */
	mmio_write32(SE4 + 0x24, 0x7f);                                 /* GENI_OUTPUT_CTRL */
	mmio_write32(SE4 + 0x20, 1);                                    /* GENI_FORCE_DEFAULT_REG */
	mmio_write32(SE4 + 0xe1c, mmio_read32(SE4 + 0xe1c) | 0xc);     /* SE_IRQ_EN: GENI M and S */
	mmio_write32(SE4 + 0x258, 0);                                   /* SE_GENI_DMA_MODE_EN: FIFO */
	mmio_write32(SE4 + 0xe18, 0);                                   /* SE_GSI_EVENT_EN */
	mmio_write32(SE4 + 0x614, 0xffffffffu);                         /* SE_GENI_M_IRQ_EN: all, polled */
	mmio_write32(SE4 + 0x618, 0xffffffffu);                         /* SE_GENI_M_IRQ_CLEAR */
	mmio_write32(SE4 + 0x80c, 1);                                   /* TX watermark */
	mmio_write32(SE4 + 0x810, 1);                                   /* RX watermark */
	mmio_write32(SE4 + 0x260, 0x7f8fe);                             /* packing: 8 bits, 4 a word (Linux's values) */
	mmio_write32(SE4 + 0x264, 0xffefe);
	mmio_write32(SE4 + 0x284, 0x7f8fe);
	mmio_write32(SE4 + 0x288, 0xffefe);
	mmio_write32(SE4 + 0x278, (10u << 20) | (11u << 10) | 26u);    /* SE_I2C_SCL_COUNTERS: 100 kHz at 19.2 MHz */
	mmio_write32(SE4 + 0x48, (7u << 4) | 1u);                       /* GENI_SER_M_CLK_CFG: divider 7, enabled */
	i2c4_ready = 1;
	return NULL;
}

/* one command (1 write, 2 read) of n bytes to/from addr; stretch: no stop after it; NULL when done */
static const char *
i2c4_xfer(uint32_t op, uint32_t addr, uint8_t *data, uint32_t n, int stretch)
{
	uint32_t st = 0, got = 0;

	mmio_write32(SE4 + 0x618, 0xffffffffu);
	mmio_write32(SE4 + (op == 1 ? 0x26c : 0x270), n);              /* SE_I2C_TX_TRANS_LEN / RX_TRANS_LEN */
	mmio_write32(SE4 + 0x600, (op << 27) | (addr << 9) | (stretch ? 4u : 0u));     /* SE_GENI_M_CMD0 */
	if (op == 1) {
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t w = 0;

			for (uint32_t k = 0; k < 4 && i + k < n; k++) {
				w |= (uint32_t)data[i + k] << (8 * k);
			}
			mmio_write32(SE4 + 0x700, w);                   /* SE_GENI_TX_FIFOn */
		}
		mmio_write32(SE4 + 0x80c, 0);
	}
	for (int t = 0; t < 20000; t++) {                               /* 200 ms at most */
		st = mmio_read32(SE4 + 0x610);                          /* SE_GENI_M_IRQ_STATUS */
		if (op == 2) {
			uint32_t fs = mmio_read32(SE4 + 0x804);         /* RX_FIFO_STATUS: words in 24:0 */

			for (uint32_t w = 0; w < (fs & 0x1ffffff) && got < n; w++) {
				uint32_t v = mmio_read32(SE4 + 0x780);

				for (uint32_t k = 0; k < 4 && got < n; k++) {
					data[got++] = (uint8_t)(v >> (8 * k));
				}
			}
		}
		if (st & 1) {
			break;
		}
		spin_us(10);
	}
	mmio_write32(SE4 + 0x618, st);
	mmio_write32(SE4 + 0x80c, 1);
	if (!(st & 1)) {
		mmio_write32(SE4 + 0x604, 1u << 1);                     /* SE_GENI_M_CMD_CTRL_REG: abort */
		return "timeout";
	}
	if (st & (1u << 10)) {
		return "NACK";
	}
	if (st & 0x2e) {
		return "command failed";
	}
	return (op == 2 && got < n) ? "short read" : NULL;
}

static void
qup0_vote(void)
{
	mmio_write32(0x0015200cULL, mmio_read32(0x0015200cULL) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14));
	for (int i = 0; i < 1000 && !qup0_clocks_on(); i++) {
		spin_us(10);
	}
}

/*
 * Test 102: at every start, the S2MU106 set to charge as Samsung's s2mu106_charger.c leaves it on a USB port, and its
 * watchdog off (iOS has nobody to kick it: when it ran out the charger went to WD_SUSPEND, CHG_STATUS1 0x10, and the
 * phone ran on the battery with the cable in). QUP0 SE4's clocks voted, GPIO51/52 muxed to qup4 (Android's pinctrl
 * state), then: CTRL12 (0x24) WDT_CLR and EN_WDT off; CTRL1 (0x19) input limit 900 mA; CTRL7 (0x1f) fast charge 1500 mA.
 * Measured (Test 101, PC USB port): -74 mA before, +300 mA into the battery after.
 */
static void
r5q_charger_setup(void)
{
	static const uint8_t writes[][2] = { { 0x24, 0x1b }, { 0x24, 0x0a }, { 0x19, 0x22 }, { 0x1f, 0x1d } };
	uint8_t st[2] = { 0x0e, 0 };
	const char *why;

	qup0_vote();
	mmio_write32(0x03d33000ULL, 0x4);
	mmio_write32(0x03d34000ULL, 0x4);
	if ((why = i2c4_ready ? NULL : i2c4_init()) == NULL) {
		why = i2c4_xfer(1, 0x3d, st, 1, 1);
		if (why == NULL) {
			why = i2c4_xfer(2, 0x3d, st, 2, 0);
		}
	}
	if (why != NULL) {
		print("charger: S2MU106 not reached (%s): left alone\n", why);
		return;
	}
	print("charger: S2MU106 status0 %x status1 %x (fault %d) before\n", (uint64_t)st[0], (uint64_t)st[1], (uint64_t)(st[1] >> 4));
	for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
		uint8_t b[2] = { writes[i][0], writes[i][1] };

		if ((why = i2c4_xfer(1, 0x3d, b, 2, 0)) != NULL) {
			print("charger: write %x to %x: %s\n", (uint64_t)b[1], (uint64_t)b[0], why);
			return;
		}
	}
	print("charger: S2MU106 set: watchdog off, input 900 mA, charge 1500 mA\n");
}

/* The maintenance commands; 1 if the command was one of them. */
static int
maint_command(const char *cmd)
{
	char name[40], msg[96];
	const char *p;
	efi_block_io *bio;

	if (!maint_mode && !(running_live != NULL && (starts_with(cmd, "oem fwlog") || starts_with(cmd, "oem peek ") ||
	    starts_with(cmd, "oem charger") || starts_with(cmd, "oem modemstop") || starts_with(cmd, "oem qupclk") ||
	    starts_with(cmd, "oem i2c4 ") || starts_with(cmd, "oem pinmux4")))) {
		return 0;                       /* (live mode, iOS running: only the firmware ring, peeks and the charger's state) */
	}
	if (starts_with(cmd, "oem pinmux4")) {
		/*
		 * Test 101: GPIO51/52 (SOUTH tile 0x03d00000) to function qup4, 2 mA, no pull, as Android's pinctrl state
		 * qupv3_se4_i2c_active: the engine's inputs read 0 (SE_GENI_IOS) until the pins are its.
		 */
		uint32_t a = mmio_read32(0x03d33000ULL), b = mmio_read32(0x03d34000ULL);

		print("pinmux4: GPIO51 cfg %x io %x, GPIO52 cfg %x io %x before\n", (uint64_t)a, (uint64_t)mmio_read32(0x03d33004ULL),
		    (uint64_t)b, (uint64_t)mmio_read32(0x03d34004ULL));
		mmio_write32(0x03d33000ULL, 0x4);
		mmio_write32(0x03d34000ULL, 0x4);
		print("pinmux4: now %x, %x; SE4 sees %x\n", (uint64_t)mmio_read32(0x03d33000ULL), (uint64_t)mmio_read32(0x03d34000ULL),
		    (uint64_t)(qup0_clocks_on() ? mmio_read32(SE4 + 0x908) : 0xdead));
		fb_reply("OKAY", "GPIO51/52 muxed to qup4");
		return 1;
	}
	if (starts_with(cmd, "oem i2c4 ")) {
		/*
		 * Test 100: "oem i2c4 rd ADDR REG N" (N bytes from REG, staged), "oem i2c4 wr ADDR REG VAL" (one byte), hex.
		 * Bus QUP0 SE4: the S2MU106 charger is 0x3d.
		 */
		const char *why;
		uint8_t buf[64];
		uint64_t addr, reg, n;
		int rd;

		p = cmd + 9;
		rd = starts_with(p, "rd ");
		p += 3;
		addr = maint_hex(&p);
		reg = maint_hex(&p);
		n = maint_hex(&p);
		if ((why = i2c4_ready ? NULL : i2c4_init()) != NULL) {
			fb_reply("FAIL", why);
			return 1;
		}
		buf[0] = (uint8_t)reg;
		if (rd) {
			if (n == 0 || n > sizeof(buf) || maint_buf == NULL) {
				fb_reply("FAIL", "i2c4 rd: 1..64 bytes");
				return 1;
			}
			why = i2c4_xfer(1, (uint32_t)addr, buf, 1, 1);
			if (why == NULL) {
				why = i2c4_xfer(2, (uint32_t)addr, buf, (uint32_t)n, 0);
			}
			if (why == NULL) {
				for (uint64_t i = 0; i < n; i++) {
					maint_buf[i] = (char)buf[i];
				}
				staged_data = (const char *)maint_buf;
				staged_len = n;
			}
		} else {
			buf[1] = (uint8_t)n;
			why = i2c4_xfer(1, (uint32_t)addr, buf, 2, 0);
			print("i2c4: wrote %x to %x of %x: %s\n", n, reg, addr, why ? why : "done");
		}
		fb_reply(why ? "FAIL" : "OKAY", why ? why : rd ? "staged" : "written");
		return 1;
	}
	if (starts_with(cmd, "oem qupclk")) {
		/*
		 * Test 99: the clocks of QUP wrapper 0 and its serial engine 4 (the S2MU106 charger's I2C, i2c@890000) voted on
		 * in GCC's apps branch-vote register as Linux's gcc-sm8150 does: m/s AHB (bits 6, 7), core/core-2x (8, 9), S4
		 * (14). The engine's clock source stays as the boot chain left it (0x17608).
		 */
		uint32_t v = mmio_read32(0x0015200cULL);

		mmio_write32(0x0015200cULL, v | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14));
		for (int i = 0; i < 1000 && !qup0_clocks_on(); i++) {
			uint64_t until = READ_SYSREG(CNTVCT_EL0) + READ_SYSREG(CNTFRQ_EL0) / 100000;      /* 10 us */

			while (READ_SYSREG(CNTVCT_EL0) < until) {
			}
		}
		print("qup0: votes %x -> %x; m_ahb %x s_ahb %x s4 %x; s4 rcg cmd %x cfg %x\n", (uint64_t)v,
		    (uint64_t)mmio_read32(0x0015200cULL), (uint64_t)mmio_read32(0x00117004ULL), (uint64_t)mmio_read32(0x00117008ULL),
		    (uint64_t)mmio_read32(0x00117604ULL), (uint64_t)mmio_read32(0x00117608ULL), (uint64_t)mmio_read32(0x0011760cULL));
		fb_reply(!qup0_clocks_on() ? "FAIL" : "OKAY",
		    "QUP0 AHB + SE4 clocks voted");
		return 1;
	}
	if (starts_with(cmd, "oem modemstop")) {
		/*
		 * Test 97: the modem (started by this loader, modem=2) stopped through TrustZone, as Linux's remoteproc stops it
		 * after a fatal error before it takes the crash dump: PAS shutdown lifts the protection of its memory, which
		 * "oem peek" may then read (0x8d800000, 160 MB). Then the AOP is told the image is off.
		 */
		uint64_t answer = 0;
		long r;

		if (!modem_running) {
			fb_reply("FAIL", "modemstop: the modem was not started by this loader");
			return 1;
		}
		r = scm_call("shutdown (after its fatal error, for the dump)", 0x206, 1, MPSS_PAS_ID, 0, 0, &answer);
		print("modem: shutdown for the dump: %x, answer %x\n", (uint64_t)r, answer);
		if (r == 0 && answer == 0) {
			modem_running = 0;
			modem_stopped = 1;
			qmp_send("{class: image, res: load_state, name: modem, val: off}");
		}
		fb_reply(r == 0 && answer == 0 ? "OKAY" : "FAIL", r == 0 && answer == 0 ? "modem stopped; its memory may be peeked" :
		    "TrustZone refused the shutdown");
		return 1;
	}
	if (starts_with(cmd, "oem charger")) {
		r5q_charger_report();
		staged_data = chg_text;
		staged_len = chg_at;
		fb_reply("OKAY", "staged");
		return 1;
	}
	if (starts_with(cmd, "oem peek ")) {
		/*
		 * Test 84: memory read while iOS runs, for the PC to decode (phone.py smem): the shared memory with the
		 * modem and the other processors (SMEM, 2 MB) and the AOP's message RAM only; reads, nothing written.
		 */
		uint64_t at, len;

		p = cmd + 9;
		at = maint_hex(&p);
		len = maint_hex(&p);
		if (len == 0 || len > 0x200000 || maint_buf == NULL || (at & 3) || (len & 3) ||
		    !((at >= 0x86000000ULL && at + len <= 0x86200000ULL) || (at >= 0x0c300000ULL && at + len <= 0x0c301000ULL) ||
		    (modem_stopped && at >= MPSS_MEM && at + len <= (uint64_t)MPSS_MEM + MPSS_MEM_SIZE) ||
		    (at >= 0x00100000ULL && at + len <= 0x001f0000ULL) ||               /* (Test 98) GCC: clock states */
		    (at >= 0x00880000ULL && at + len <= 0x008c4000ULL))) {              /* (Test 98) QUP wrapper 0, its SEs */
			fb_reply("FAIL", "peek: SMEM, AOP msg RAM, the stopped modem, GCC, QUP wrapper 0; at most 2 MiB");
			return 1;
		}
		if (at >= 0x00880000ULL && at < 0x008c4000ULL && !qup0_clocks_on()) {
			fb_reply("FAIL", "peek: QUP wrapper 0's AHB clocks are off (a read would hang the bus)");
			return 1;
		}
		/*
		 * Test 85: within SMEM only what the AP may read -- the table of contents (the last 4 KiB), the header page, the
		 * global partition and the partitions we are one end of. Others (the modem's with the ADSP, ...) are protected:
		 * a read of one took the loader down for good (Test 84).
		 */
		if (at >= 0x86000000ULL && at + len <= 0x86200000ULL) {
			uint64_t toc = 0x861ff000ULL;
			int ok = (at >= toc) || (at + len <= 0x86001000ULL);

			if (!ok && mmio_read32(toc) == 0x434f5424u) {       /* "$TOC" */
				uint32_t n = mmio_read32(toc + 8);

				for (uint32_t i = 0; i < n && i < 64 && !ok; i++) {
					uint64_t e = toc + 32 + 48ULL * i;
					uint64_t po = 0x86000000ULL + mmio_read32(e), ps = mmio_read32(e + 4);
					uint32_t hosts = mmio_read32(e + 12);
					uint16_t h0 = (uint16_t)hosts, h1 = (uint16_t)(hosts >> 16);

					ok = (h0 == 0 || h1 == 0 || h0 == 0xfffe) && at >= po && at + len <= po + ps;
				}
			}
			if (!ok) {
				fb_reply("FAIL", "peek: not the TOC, nor a partition the AP is one end of (others are protected)");
				return 1;
			}
		}
		for (uint64_t i = 0; i < len; i += 4) {
			*(uint32_t *)(maint_buf + i) = mmio_read32(at + i);
		}
		staged_data = (const char *)maint_buf;
		staged_len = len;
		fb_reply("OKAY", "staged");
		return 1;
	}
	if (starts_with(cmd, "oem fwlog")) {
		staged_data = (const char *)(uintptr_t)0xC1600000ULL;          /* the firmware's SEC_LOG_BASE ring */
		staged_len = 0x200000;
		fb_reply("OKAY", "2 MiB firmware ring staged");
		return 1;
	}
	if (starts_with(cmd, "oem part ")) {
		p = cmd + 9;
		bio = maint_part(maint_word(&p, name, sizeof(name)));
		if (bio == NULL) {
			fb_reply("FAIL", "no such partition");
			return 1;
		}
		uint32_t mark = log_len;
		print("%s: %d blocks of %d", name, bio->media->last_block + 1, (uint64_t)bio->media->block_size);
		log_buf[log_len] = 0;
		fb_reply("OKAY", log_buf + mark);
		log_len = mark;
		return 1;
	}
	if (starts_with(cmd, "oem readpart ")) {
		uint64_t off, len, bs;

		p = cmd + 13;
		bio = maint_part(maint_word(&p, name, sizeof(name)));
		off = maint_hex(&p);
		len = maint_hex(&p);
		bs = bio != NULL ? bio->media->block_size : 0;
		if (bio == NULL || bs == 0 || off % bs || len % bs || len == 0 || len > MAINT_BUF_SIZE ||
		    off + len > (bio->media->last_block + 1) * bs) {
			fb_reply("FAIL", "bad partition, offset or length");
			return 1;
		}
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, off / bs, len, maint_buf))) {
			fb_reply("FAIL", "read failed");
			return 1;
		}
		staged_data = (const char *)maint_buf;
		staged_len = len;
		fb_reply("OKAY", "staged");
		return 1;
	}
	if (starts_with(cmd, "oem writepart ") || starts_with(cmd, "flash:boot")) {
		uint64_t off = 0, bs, done;
		efi_status s;

		if (starts_with(cmd, "flash:boot")) {
			name[0] = 'b'; name[1] = 'o'; name[2] = 'o'; name[3] = 't'; name[4] = 0;
		} else {
			p = cmd + 14;
			maint_word(&p, name, sizeof(name));
			off = maint_hex(&p);
		}
		if (!str_eq(name, "boot") && !str_eq(name, "userdata")) {
			fb_reply("FAIL", "only boot and userdata are written");
			return 1;
		}
		bio = maint_part(name);
		bs = bio != NULL ? bio->media->block_size : 0;
		if (bio == NULL || bs == 0 || maint_len == 0 || off % bs || maint_len % bs ||
		    off + maint_len > (bio->media->last_block + 1) * bs) {
			fb_reply("FAIL", "bad partition, offset or download size");
			return 1;
		}
		if (str_eq(name, "boot") && (off != 0 || maint_len != (bio->media->last_block + 1) * bs ||
		    maint_buf[0] != 'A' || maint_buf[1] != 'N' || maint_buf[2] != 'D' || maint_buf[7] != '!')) {
			fb_reply("FAIL", "boot takes a whole Android boot image of the partition's size");
			return 1;
		}
		print("maintenance: writing %d bytes to %s at %x\n", maint_len, name, off);
		s = bio->write_blocks(bio, bio->media->media_id, off / bs, maint_len, maint_buf);
		if (!EFI_ERROR(s)) {
			s = bio->flush_blocks(bio);
		}
		for (done = 0; !EFI_ERROR(s) && done < maint_len; done += sizeof(maint_check)) {
			uint64_t n = maint_len - done > sizeof(maint_check) ? sizeof(maint_check) : maint_len - done;

			s = bio->read_blocks(bio, bio->media->media_id, (off + done) / bs, n, maint_check);
			if (!EFI_ERROR(s) && memcmp(maint_check, maint_buf + done, (size_t)n) != 0) {
				s = EFI_ERROR_BIT | 27;   /* EFI_VOLUME_CORRUPTED */
			}
		}
		print("maintenance: %s %s\n", name, EFI_ERROR(s) ? "WRITE FAILED" : "written and read back");
		fb_reply(EFI_ERROR(s) ? "FAIL" : "OKAY", EFI_ERROR(s) ? "write or read-back failed" : "written and verified");
		return 1;
	}
	if (starts_with(cmd, "oem args set")) {
		/* the text: the last download (python phone.py args TEXT) */
		if (maint_len == 0 || maint_len > R5Q_ARGS_MAX) {
			fb_reply("FAIL", "download the arguments first (at most 400 bytes)");
			return 1;
		}
		fb_reply(r5q_args_store((const char *)maint_buf, maint_len) ? "OKAY" : "FAIL", "kept for every start");
		return 1;
	}
	if (starts_with(cmd, "oem args clear")) {
		fb_reply(r5q_args_store("", 0) ? "OKAY" : "FAIL", "no extra arguments");
		return 1;
	}
	if (starts_with(cmd, "oem args show")) {
		r5q_args_load();
		memcpy(out_buf, r5q_args.text, r5q_args.len);
		out_buf[r5q_args.len] = '\n';
		staged_data = out_buf;
		staged_len = r5q_args.len + 1;
		fb_reply("OKAY", "staged");
		return 1;
	}
	if (starts_with(cmd, "oem settime ")) {
		uint64_t wall = 0;

		for (p = cmd + 12; *p >= '0' && *p <= '9'; p++) {
			wall = wall * 10 + (uint64_t)(*p - '0');
		}
		if (wall < 1700000000ULL || wall > 4000000000ULL) {
			fb_reply("FAIL", "settime takes seconds since 1970 (UTC)");
			return 1;
		}
		fb_reply(r5q_clock_set(wall) ? "OKAY" : "FAIL", r5q_rtc_valid ? "clock set; anchor kept with the RTC" :
		    "clock set for this start; the RTC is not usable");
		return 1;
	}
	if (starts_with(cmd, "oem continue")) {
		fb_reply("OKAY", "starting iOS");
		maint_continue = 1;
		return 1;
	}
	if (starts_with(cmd, "oem poweroff")) {
		fb_reply("OKAY", "");
		BS->stall(200000);
		ST->rt->reset_system(2 /* EfiResetShutdown */, 0, 0, NULL);
		return 1;
	}
	(void)msg;
	return 0;
}

static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	if (maint_command(cmd)) {
		return;
	}
	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem touch ")) {
			/* (Test 126) a gesture for iOS: KIND X0 Y0 X1 Y1 MS, hex; seq written last */
			const char *p = cmd + 10;
			uint32_t k = (uint32_t)maint_hex(&p), x0 = (uint32_t)maint_hex(&p), y0 = (uint32_t)maint_hex(&p);
			uint32_t x1 = (uint32_t)maint_hex(&p), y1 = (uint32_t)maint_hex(&p), ms = (uint32_t)maint_hex(&p);

			if (!busy || kernel_touch_box == NULL) {
				fb_reply("FAIL", "no running kernel with a touch mailbox");
			} else if (kernel_touch_box->seq != kernel_touch_box->done) {
				fb_reply("FAIL", "the last gesture is still being played");
			} else {
				kernel_touch_box->kind = k;
				kernel_touch_box->x0 = x0;
				kernel_touch_box->y0 = y0;
				kernel_touch_box->x1 = x1;
				kernel_touch_box->y1 = y1;
				kernel_touch_box->ms = ms;
				__asm__ volatile ("dsb sy" ::: "memory");
				kernel_touch_box->seq = kernel_touch_box->seq + 1;
				__asm__ volatile ("dsb sy" ::: "memory");
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : maint_mode ? "0x05000000" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row (formatted here: during a live run print() does not write the log) */
				char geo[48];
				uint64_t v3[3] = { SCREEN.width, SCREEN.height, SCREEN.stride };
				int at = 0;

				for (int k = 0; k < 3; k++) {
					char tmp[24];
					int i = 0;
					uint64_t v = v3[k];

					do {
						tmp[i++] = (char)('0' + v % 10);
						v /= 10;
					} while (v && i < 20);
					if (k) {
						geo[at++] = ' ';
					}
					while (i) {
						geo[at++] = tmp[--i];
					}
				}
				geo[at] = 0;
				fb_reply("OKAY", geo);
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = maint_mode ? MAINT_BUF_SIZE : PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = maint_mode ? maint_buf : (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				if (maint_mode) {
					maint_len = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			if (staged_len == 0) {
				/* (r5q: a DATA00000000 phase left Windows' fastboot and this loader out of step: "no link") */
				fb_reply("FAIL", "nothing staged");
				return;
			}
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	if (maint_probe && !maint_mode) {
		maint_enter();          /* r5q: the PC asked something while the loader listened: maintenance mode */
	}
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	while (!maint_continue) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
/* HACKIOS_RAMFB_HEIGHT: QEMU's ramfb refuses a mode taller than 2048 (bochs-vbe's
 * VBE_DISPI_MAX_YRES), so the panel is asked for 2048 rows and the kernel scales the
 * 1125x2436 display into it -- the same thing it does on a real phone. */
#define RAMFB_HEIGHT    2048
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * RAMFB_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(RAMFB_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		/* HACKIOS: this is the value we wrote ourselves; nothing writes back into it, so it
		 * says nothing about whether the request worked. QEMU's ramfb refuses a mode taller
		 * than 2048 (VBE_DISPI_MAX_YRES), and the mode it takes is the one in the panel. */
		print("ramfb: control %x after the request (the mode is taken as it stands)\n",
		    (uint64_t)be32(dma.control));
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = RAMFB_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

#define AUTOBOOT_WAIT_MS        500     /* (Test 153: was 2000; a key held from the start is seen at once) */

#ifdef R5Q_LAB_TIMED
/*
 * r5q (Test 77): a volume key from any key input there is. The console's (ConIn) is never connected on this phone
 * ("[Bds] Connect ConIn failed - Unsupported"), but Qualcomm's ButtonsDxe installs a text input of its own.
 * Returns the arrow (1 up, 2 down) or 0.
 */
static int
r5q_volume_key(void)
{
	static const efi_guid sti_guid = { 0x387477c1, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
	efi_handle *handles;
	uint64_t count;
	efi_input_key key;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &sti_guid, NULL, &count, &handles))) {
		return 0;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_text_input *in;

		/*
		 * (Test 159) not the console's own input: reading it makes BDS try to connect ConIn on demand, which fails on
		 * this phone and says "[Bds] Connect ConIn failed - Unsupported" on the screen. The keys are ButtonsDxe's.
		 */
		if (handles[i] == ST->con_in_handle ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &sti_guid, (void **)&in)) || in == NULL || in == ST->con_in) {
			continue;
		}
		while (in->read_key_stroke(in, &key) == 0) {
			if (key.scan_code == 1 || key.scan_code == 2) {
				return key.scan_code;
			}
		}
	}
	return 0;
}
#endif

/* true if a volume key (an arrow) is pressed within AUTOBOOT_WAIT_MS */
static int
autoboot_interrupted(void)
{
	efi_text_input *in = ST->con_in;
	efi_input_key key;

#ifdef R5Q_LAB_TIMED
	for (unsigned ms = 0; ms < AUTOBOOT_WAIT_MS; ms += 10) {
		int k = r5q_volume_key();

		if (k != 0) {
			print("autoboot: volume %s: USB commands\n", k == 1 ? "up" : "down");
			return 1;
		}
		BS->stall(10000);
	}
	return 0;
#endif
	if (in == NULL) {
		return 0;
	}
	print("autoboot: the kernel starts in %d s; a volume key now for the USB commands instead\n",
	    (uint64_t)(AUTOBOOT_WAIT_MS / 1000));
	flush_log();
	while (in->read_key_stroke(in, &key) == 0) {
#ifdef R5Q_LAB_TIMED
		/* r5q: a volume key already held when the phone was started counts too */
		if (key.scan_code == 1 || key.scan_code == 2) {
			print("autoboot: volume %s held: USB commands\n", key.scan_code == 1 ? "up" : "down");
			return 1;
		}
#endif
	}
	for (unsigned ms = 0; ms < AUTOBOOT_WAIT_MS; ms += 10) {
		if (in->read_key_stroke(in, &key) == 0 && (key.scan_code == 1 || key.scan_code == 2)) {
			print("autoboot: volume %s pressed: USB commands\n", key.scan_code == 1 ? "up" : "down");
			return 1;
		}
		BS->stall(10000);
	}
	print("autoboot: starting the kernel\n");
	return 0;
}

#ifdef R5Q_LAB_TIMED
/*
 * r5q, installed (Test 73+): a volume key held while the loader starts gives the PC its fastboot commands
 * (maint_command()) before iOS: logs, partitions read and written, BOOT updated without Download Mode.
 */
static void
r5q_maintenance(int keep_usb)
{
	int key = autoboot_interrupted();
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), t0;

	if (!usb_start()) {
		print("maintenance: USB did not start: starting iOS\n");
		return;
	}
	if (key) {
		maint_enter();
	} else {
		/*
		 * Without a key: a PC that is waiting (python phone.py ...: fastboot keeps asking until the phone is there)
		 * sends its first command within seconds of the phone showing up. A charger, or no cable, never connects.
		 */
		print("maintenance: listening for a PC for a few seconds\n");
		flush_log();
		maint_probe = 1;
		t0 = READ_SYSREG(CNTVCT_EL0);
		while (!maint_mode && READ_SYSREG(CNTVCT_EL0) - t0 < (usb_connected ? 15 * freq : 3 * freq / 2)) {   /* (Test 153: 1.5 s, was 5, without a PC) */
			usb_poll_command();
			BS->stall(1000);
		}
		maint_probe = 0;
	}
	if (keep_usb) {
		maint_alloc();          /* (Test 80: room for the whole live kernel log) */
	}
	if (!maint_mode && keep_usb) {
		/* Test 79: live mode (bootcpu=N): this core keeps serving fastboot while iOS runs on core N */
		print("maintenance: no PC asked: starting iOS; fastboot stays up while it runs (oem log, oem stop)\n");
		return;
	}
	if (!maint_mode) {
		print("maintenance: no PC asked: starting iOS\n");
		USB->stop();            /* (iOS starts with the USB controller as it always has: stopped) */
		USB = NULL;
		usb_connected = 0;
		usb_rx_queued = 0;
		return;
	}
	print("MAINTENANCE MODE: fastboot over USB (oem fwlog, oem readpart, oem writepart, flash:boot, oem continue)\n");
	flush_log();
	usb_command_loop();
	print("maintenance: leaving the commands, starting iOS\n");
	maint_mode = 0;
	screen_quiet = 0;
}
#endif

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
#ifdef R5Q_FIRST_TEST
	print("TEST1 XNU LOADER ENTRY OK (diagnostic only)\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d\n", READ_SYSREG(CurrentEL) >> 2,
	    READ_SYSREG(MIDR_EL1), READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0));
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP == NULL ||
	    GOP->mode == NULL || GOP->mode->info == NULL) {
		print("TEST1 FAIL: graphics protocol is missing\n");
		return EFI_ERROR_BIT | 3;
	}
	print("TEST1 DISPLAY %d x %d stride=%d base=%x bytes=%x\n",
	    (uint64_t)GOP->mode->info->width, (uint64_t)GOP->mode->info->height,
	    (uint64_t)GOP->mode->info->pixels_per_scanline, GOP->mode->fb_base, GOP->mode->fb_size);
	if (GOP->mode->fb_base != platform_r5q.fb_base || GOP->mode->info->width != 1080 ||
	    GOP->mode->info->height != 2400) {
		print("TEST1 FAIL: display metadata does not match R5Q\n");
		return EFI_ERROR_BIT | 3;
	}
	print("TEST1 HANDOFF OK; storage, USB and kernel paths were not entered\n");
	return 0;
#endif
	print("== XNULOADER v3 ==\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

#ifndef R5Q_XNU_TEST
	install_exception_hook();
#endif
#ifdef R5Q_XNU_TEST
	/*
	 * Which kernel checkpoint the lab profile stops at. Hardware profiles use 1; later values
	 * are emulation-only (run_source_kernel_emulation.py). Pasted as text, so the default
	 * build's strings and code are unchanged.
	 */
#ifndef R5Q_XNU_CHECKPOINT
#define R5Q_XNU_CHECKPOINT 1
#endif
#define R5Q_CP_STR2(n) #n
#define R5Q_CP_STR(n) R5Q_CP_STR2(n)
#ifndef R5Q_FULLFW_PROBE   /* the probe reports whatever display the full firmware set up */
	if (PLAT != &platform_r5q) {
		print("TEST6 REFUSED: this is not the R5Q framebuffer platform\n");
		return EFI_ERROR_BIT | 3;
	}
#endif
#ifdef R5Q_MDSS_DUMP
	{
		/*
		 * Test 52/53: why the panel shows the framebuffer shifted under the full firmware (Tests 43, 51) but not
		 * under the minimal one (Test 35). Reads only (MMIO of the display blocks the boot chain left running,
		 * the ones BootShim and the boot manager already use), then one kick like the boot manager's
		 * KickDisplay, sampling the MDP interrupt status (underruns) around it. Run under both firmwares; diff.
		 */
		static const struct { const char *name; uint64_t base, len; } blk[] = {
			{ "mdp_top", 0x0AE01000, 0x400 }, { "ctl0", 0x0AE02000, 0x200 }, { "sspp_dma0", 0x0AE25000, 0x100 },
			{ "pp0", 0x0AE71000, 0x100 }, { "intf", 0x0AE6B000, 0x300 }, { "intf1", 0x0AE6B800, 0x300 },
			{ "dsi0", 0x0AE94000, 0x400 },
		};
		volatile uint32_t *intr = (volatile uint32_t *)(uintptr_t)(0x0AE01000 + 0x14);
		for (unsigned b = 0; b < sizeof(blk) / sizeof(blk[0]); b++) {
			for (uint64_t o = 0; o < blk[b].len; o += 16) {
				volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(blk[b].base + o);
				uint64_t v0 = r[0], v1 = r[1], v2 = r[2], v3 = r[3];
				if (v0 | v1 | v2 | v3) {
					print("[MDSS] %s+%x: %x %x %x %x\n", blk[b].name, o, v0, v1, v2, v3);
				}
			}
		}
		for (int phase = 0; phase < 2; phase++) {
			if (phase == 1) {
				*(volatile uint32_t *)(uintptr_t)(0x0AE6B000 + 0x2B4) = 0x80000001u;   /* as KickDisplay */
				__asm__ volatile ("dsb sy" ::: "memory");
				if ((*(volatile uint32_t *)(uintptr_t)(0x0AE02000 + 0x1C) & 1) == 0) {
					*(volatile uint32_t *)(uintptr_t)(0x0AE02000 + 0x1C) = 1;          /* CTL0 START */
				}
				__asm__ volatile ("dsb sy" ::: "memory");
				print("[MDSS] kicked\n");
			}
			for (int i = 0; i < 10; i++) {
				print("[MDSS] t%d.%d intr %x dsi_status %x dsi_err %x intf_status %x ctl_start %x\n", (uint64_t)phase,
				    (uint64_t)i, (uint64_t)*intr, (uint64_t)*(volatile uint32_t *)(uintptr_t)(0x0AE94000 + 0x08),
				    (uint64_t)*(volatile uint32_t *)(uintptr_t)(0x0AE94000 + 0x10C),
				    (uint64_t)*(volatile uint32_t *)(uintptr_t)(0x0AE6B000 + 0x2B8),
				    (uint64_t)*(volatile uint32_t *)(uintptr_t)(0x0AE02000 + 0x1C));
				BS->stall(20000);
			}
		}
		print(R5Q_PREPARE_LABEL " MDSS DUMP DONE\n");
		return 0;
	}
#endif
#ifdef R5Q_FULLFW_PROBE
	{
		/*
		 * Test 37: what the full r5q firmware (r5qfullPkg: every Qualcomm DXE) gives a boot application.
		 * Read-only: locate and look; no USB start, no block writes, no allocation; then return.
		 */
		static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
		static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
		static uint8_t probe_map[32768];
		void *usb = NULL;
		efi_handle *handles = NULL;
		uint64_t count = 0, units = 0, size = sizeof(probe_map), key = 0, dsize = 0, total = 0, best = 0, best_at = 0;
		uint32_t dver = 0;
		efi_status s = BS->locate_protocol(&usb_guid, NULL, &usb);
		print(R5Q_PREPARE_LABEL " PROBE usb device protocol %s (status %x)\n", EFI_ERROR(s) ? "MISSING" : "PRESENT", s);
		if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
			for (uint64_t i = 0; i < count; i++) {
				efi_block_io *bio;
				if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
					continue;
				}
				units++;
				print(R5Q_PREPARE_LABEL " PROBE unit %d: %d blocks of %d bytes\n", units, bio->media->last_block + 1,
				    (uint64_t)bio->media->block_size);
			}
		}
		print(R5Q_PREPARE_LABEL " PROBE block io handles %d, raw units %d\n", count, units);
		if (EFI_ERROR(BS->get_memory_map(&size, probe_map, &key, &dsize, &dver)) || dsize < sizeof(efi_memory_descriptor)) {
			print(R5Q_PREPARE_LABEL " PROBE memory map failed\n");
		} else {
			for (uint64_t off = 0; off + dsize <= size; off += dsize) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(probe_map + off);
				uint64_t len = d->pages << 12;
				if (d->type != EfiConventionalMemory) {
					continue;
				}
				total += len;
				if (len >= 0x2000000ULL) {
					print(R5Q_PREPARE_LABEL " PROBE conventional %x size %x\n", d->phys_start, len);
				}
				if (len > best) {
					best = len;
					best_at = d->phys_start;
				}
			}
			print(R5Q_PREPARE_LABEL " PROBE conventional total %x, largest %x at %x\n", total, best, best_at);
		}
		print(R5Q_PREPARE_LABEL " FULL FIRMWARE PROBE DONE\n");
		return 0;
	}
#endif
#ifdef R5Q_PREPARE_ONLY
	print(R5Q_PREPARE_LABEL " PREPARE XNU ONLY; no CPU/MMU switch or iOS userland\n");
#elif defined(R5Q_RETURN_SELFTEST)
	print(R5Q_PREPARE_LABEL " FIRMWARE RETURN PATH ONLY; XNU is not entered\n");
#elif defined(R5Q_LAB_USERLAND)
	print(R5Q_PREPARE_LABEL " REAL XNU USERLAND RUN " R5Q_CP_STR(R5Q_LAB_TIMED) " S\n");
#elif defined(R5Q_LAB_TIMED)
	print(R5Q_PREPARE_LABEL " REAL XNU TIMED RUN " R5Q_CP_STR(R5Q_LAB_TIMED) " S; no iOS userland\n");
#else
	print(R5Q_PREPARE_LABEL " REAL XNU EARLY CHECKPOINT " R5Q_CP_STR(R5Q_XNU_CHECKPOINT) "; no iOS userland\n");
#endif
#ifdef R5Q_RETURN_SELFTEST
	int started = run_kernel(1, CHECKPOINT_SELFTEST);
	print(R5Q_PREPARE_LABEL " RETURN PATH SELF TEST %s\n", started ? "VERIFIED" : "FAILED");
	return started ? 0 : EFI_ERROR_BIT | 1;
#else
	int started = run_kernel(1, R5Q_XNU_CHECKPOINT);
#endif
#ifdef R5Q_PREPARE_ONLY
	print(R5Q_PREPARE_LABEL " PREPARATION RESULT %d\n", (uint64_t)started);
	return started == 2 ? 0 : EFI_ERROR_BIT | 1;
#endif
	print(R5Q_PREPARE_LABEL " XNU RETURN started=%d reason=%d checkpoint=%d\n", (uint64_t)started,
	    loader_ctx[CTX_REASON], loader_ctx[CTX_ESR]);
#ifdef R5Q_LAB_TIMED
	/* reason 0: the kernel came back by itself (its timer, or a panic: the log says which) */
	if (started && loader_ctx[CTX_REASON] == 0) {
		print(R5Q_PREPARE_LABEL " REAL XNU TIMED RUN RETURNED\n");
		/*
		 * r5q, installed (BOOT): iOS's "slide to power off" ends here with the kernel's request to turn the phone
		 * off; without this the firmware resets and iOS starts again. The firmware's own shutdown, as Android's.
		 */
		if (kernel_power == KERNEL_POWER_OFF) {
			/* (Test 72+: the firmware saves the whole log first and then turns the phone off itself) */
			print(R5Q_PREPARE_LABEL " iOS ASKED TO POWER OFF\n");
			flush_log();
			return 0x504F4646;      /* "POFF": the firmware's R5Q_STATUS_POWER_OFF */
		}
		return 0;
	}
	return EFI_ERROR_BIT | 1;
#endif
	if (started && loader_ctx[CTX_REASON] == 2 && loader_ctx[CTX_ESR] == R5Q_XNU_CHECKPOINT) {
		print(R5Q_PREPARE_LABEL " REAL XNU CHECKPOINT " R5Q_CP_STR(R5Q_XNU_CHECKPOINT) " VERIFIED\n");
		return 0;
	}
	return EFI_ERROR_BIT | 1;
#endif
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	/*
	 * A payload with a kernel and no "usb" is a boot without the computer (mkpayload.py --autoboot):
	 * the kernel starts by itself. A volume key held in the first seconds takes the loader to its USB
	 * commands instead, so that the phone can still be worked on from the computer.
	 */
	if (have_payload && !pi.usb_mode && pi.kernel != NULL && autoboot_interrupted()) {
		pi.usb_mode = 1;
	}
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/*
	 * One-shot mode: run the kernel that came with the image, then reset -- or, when iOS was turned
	 * off ("slide to power off"), turn the phone off.
	 */
	if (have_payload) {
		run_kernel(0, 0);
		if (kernel_power == KERNEL_POWER_OFF) {
			power_off();
		}
	}
	reboot();
	return 0;
}
