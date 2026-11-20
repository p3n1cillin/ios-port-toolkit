/* Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026, from the donor G986N-iOS (commit 946de0fed8e5, a port of HackiOS): Apple Public Source License 2.0, see LICENSE and MODIFICATIONS.md in this directory. */
/*
 * The Wi-Fi of the POCO X5 Pro (redwood), from nothing and step by step. Every address below
 * belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The radio is a WCN6750. Its firmware does not run on the application processor but on a
 * processor of its own inside the SoC, the "WPSS" (a Hexagon DSP), which stays in reset until
 * somebody loads it: the application processor puts the signed image -- the phone's own
 * wpss.mdt and wpss.b02 to wpss.b07, from /vendor/firmware_mnt/image of its Android -- into the
 * memory reserved for it and asks TrustZone to authenticate and start it, as for the GPU's zap
 * shader (redwood_adreno.c). Everything after that is conversation with the running firmware.
 * Its first words go through what this file begins with:
 *
 *   SMEM    two megabytes of memory that every processor of the SoC can reach, laid out by the
 *           boot loader, in which each of them allocates numbered items
 *   SMP2P   a pair of such items between two processors, with named entries of 32 bits: in
 *           the DSP's are "I am up", "I have crashed", "I have stopped"
 *   IPCC    the SoC's doorbells: a register to ring another processor's with, and one that
 *           says who rang ours
 * and through one thing that only the loading needs: the mailbox of the always-on processor
 * (the "AOP", which runs RPMh, the keeper of the supply rails), which is told that the image is
 * being loaded before it honours the DSP's own requests for power.
 *
 * Boot-arg wifi_step=N performs steps 1 to N and stops; a step says what it is about to touch before
 * it touches it and what it found after, reads come before writes, and a step that does not
 * find what it needs ends the sequence instead of going on blind.
 *
 *   1  reads   the shared memory area: how the boot loader laid it out, and whether the DSP
 *              has left anything in it (it has if it was started since the phone was)
 *   2  reads   the always-on processor's mailbox and the doorbells, as they are
 *   3  reads   RPMh: where it has the supplies involved, and what the application processor's
 *              sequencer holds in its command slots (what was last asked for from here)
 *   4  reads   the supplies themselves, from the two PMICs that make the radio's three
 *              (PM7325 S7, S1, S8) and the two the DSP runs from (CX and MX: PM8350C S2 and
 *              S10). The only thing written is the command word that asks the PMIC arbiter for
 *              a read
 *   5  reads   the radio's pins
 *   6  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   7  writes  the always-on processor's mailbox open: five words of its message memory, and
 *              its doorbell
 *   8  asks    TrustZone to take the image's header and check its signature (image 6), and
 *              tells it where the image will be
 *   9  writes  the image into the DSP's memory, and reads it back
 *  10  writes  our SMP2P item into the shared memory: allocated there, under the SoC's lock for
 *              that, unless a kernel before this one has made it; filled in afresh either way
 *  11  writes  "the image is loaded" to the always-on processor, asks TrustZone to authenticate
 *              the image and let the DSP out of reset, and watches the shared memory and our
 *              doorbell for the DSP's first words, answering in our item what SMP2P's rules
 *              have us answer
 *  12  reads   GLINK's queues in the shared memory, and what the DSP has put into its own
 *  13  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our
 *              intents; from here a thread of ours listens to the DSP
 *  14  talks   QRTR on that channel: hello both ways, the services the DSP tells of; a port
 *              of ours looks the Wi-Fi firmware's service up (sys/redwood_qrtr.h)
 *  15  talks   to the Wi-Fi firmware's service, what a WCN6750's driver says to it before it
 *              touches the radio: a stand-in for ath11k's own code, see redwood_wlfw.c
 *  16  reads   seven words of the radio's registers, where the firmware said they are: only
 *              with wifi_radio_read=1, and only with the firmware in mission mode (outside
 *              it a read there has ended the kernel)
 * Steps 1 to 5 are the survey; they write to nothing but the arbiter's command word.
 *
 * What is started is stopped: when the steps are over, wherever a run ends in order while the
 * DSP runs, and on a panic's way back to the loader, TrustZone is asked to shut the DSP down
 * and the always-on processor is told that the image is gone. A kernel that dies otherwise
 * leaves the DSP running, and the kernel after it cannot ask the hardware whether that is so. But the shared memory outlives our
 * kernels (the phone is not restarted between them), and the DSP's own item in it is there if
 * and only if the DSP has run since the phone was started: when step 8 finds it, TrustZone is
 * first asked for the shutdown. On a phone just started nothing is asked that is not needed.
 *
 * The DSP is asked to stop before TrustZone shuts it down (Linux's order; dsp_ask_to_stop()),
 * and that matters: a DSP shut down by TrustZone alone -- as the first version of this did,
 * having no SMP2P item to ask with, so that the DSP never got as far as saying it was up --
 * could not be started again before the phone was restarted. Twice that was tried, with the
 * same image written and read back again and accepted by TrustZone again, and both times the
 * phone reset without a word at the request to authenticate and start, or within two seconds
 * of it. Asked first, and having acknowledged, it starts again. So step 8, having had a DSP
 * shut down that did not say it had stopped, goes no further unless told to (wifi_again=1).
 * (Whether the asking cures it or the DSP's having come up fully before, was not told apart.)
 *
 * Nothing here switches a supply rail or changes a pin.
 *
 * wifi=1 is all of it for whoever uses the phone: the steps that are needed (not the ones that
 * only look), ath11k as step 15 for real, its station given to the rest of the kernel to ask
 * (sys/redwood_sta.h: iOS's Wi-Fi side does, through bsd/dev/redwood_apple80211.c), the network
 * of fw/network joined if the small image has that file, and everything left running while the
 * system does -- on a thread of its own, so that the system's start does not wait for it, and
 * without a word in the log but the outcome (what the steps say is kept, and shown if the radio
 * does not come up). Without the radio's firmware in the small image it says so in a line and
 * does nothing. (wifi_loud=1 with it: nothing is kept back, for looking into a start that goes
 * wrong.) The rest of this comment is about the steps one by one, wifi_step=N, which is how
 * all this was found out.
 *
 * More boot-args, for wifi_step=N:
 *   wifi_new=K         steps K and later have not been seen to work (default: the last one)
 *   wifi_pause=MS      how long such a step waits after announcing a group of accesses
 *                      (default 2200: the loader, watching from another core, notes the end of
 *                      our log every two seconds, and after a silent reset that is all there is)
 *   wifi_fine=1        such a step waits like that before every single write and call
 *   wifi_wait=S        how long step 11 watches for the DSP to say that it is up (default 10)
 *   wifi_watch=S       ... and how long it goes on watching once it has (default 3)
 *   wifi_listen=S      how long step 14 waits for the Wi-Fi firmware's service (default 3)
 *   wifi_ask=1         step 14 also asks that service a question (its capabilities) and
 *                      writes down the beginning of an answer
 *   wifi_cal=1         step 15 asks for cold-boot calibration as ath11k does when it is on, and
 *                      waits up to a minute for it; wifi_ready=S: how long it waits for
 *                      "firmware ready" otherwise (default 5)
 *   wifi_mission=1     step 15 goes on to the configuration and the mode "mission"
 *   wifi_radio_read=1  step 16 does read (2: whatever mode the firmware is in)
 *   wifi_keep=1        when the steps are over the DSP and the link stay up while the system
 *                      runs, for whoever uses sys/redwood_qrtr.h; stopped when the run ends
 *   wifi_dirty=1       the DSP is not stopped: left as a kernel that died leaves it
 *   wifi_again=1       start the DSP although the one before it was shut down without having
 *                      acknowledged a stop: THAT HAS RESET THE PHONE, twice of two
 *   wifi_return=1      when all is over, go back to the loader instead of starting the system
 *   wifi_smmu=1        before any step: read what the boot chain has set up in the SMMU for the
 *                      radio's stream (smmu_look() below; nothing is written)
 *   wifi_smmu=2        ... and give that stream a context of the SMMU with page tables of ours,
 *                      so that the radio can be shown memory (radio_space_on() below: THE
 *                      SMMU IS WRITTEN); taken back when the DSP is stopped
 *   wifi_mac=A         the radio's own address for the driver (redwood_wifi_radio_address()
 *                      below; xl takes it from a file that is not in the repository)
 *   wifi_scan=1        with wifi_lx=2: once the driver is up, an interface in station mode is
 *                      made and the radio scans, listening only; what it heard goes to the log
 *                      (other people's networks by name and address: the log is not for
 *                      passing on)
 *   wifi_join=1        with wifi_lx=2: the station joins the network named in fw/network of
 *                      the small image (redwood_wifi_network() below) and the driver stays
 *                      up as the link behind en0; with wifi_keep=1 while the system runs,
 *                      else until the steps are over (wifi_hold=S: S seconds longer)
 *   wifi_station=1     with wifi_lx=2: the driver stays up and its station is the kernel's to
 *                      ask (sys/redwood_sta.h); nothing is joined by the driver itself
 *   wifi_file=0        with wifi=1: the network of fw/network is not joined at the start
 *   wifi_irq=1         the radio's interrupts (MSI through its SMMU context to the GIC) instead
 *                      of looking at its rings every millisecond (lx_bus.c); wifi=1 stands for it
 *   wifi_ba=1          block ack: the access point's requests to send aggregates are answered
 *                      (lx_80211.c); without, the link carries about a fifth; wifi=1 stands for it
 *   wifi_wide=1        the channel as wide as the access point offers (40 or 80 MHz) instead of
 *                      20 MHz; wifi=1 stands for it
 *   wifi_lx_step=MS    the driver's start says each stub it comes to and waits MS there: for
 *                      a run that ends without a word (follow it with XL_FOLLOW=2)
 *   wifi_lx=1          step 15 is ath11k's own instead of the stand-in (redwood_lx.c), with
 *                      the radio's registers neither read nor written; 2: for real (which
 *                      wants wifi_smmu=2: the radio is given addresses of its rings)
 * The system does not go on starting until all of this is over, so that the end of the log is
 * ours.
 *
 * No interrupt is taken: what the DSP and the always-on processor say is looked for. What an
 * interrupt would take is said at the end of this file.
 *
 * Addresses are from the phone's own device tree ("dt:" below: qcom,wpss@8a00000,
 * qcom,smp2p-wpss, qcom,glink/wpss, qcom,ipcc@408000, power-controller@c300000, rsc@18200000,
 * qcom,wcn6750, reserved-memory), layouts from Linux, where SC7280 is this SoC:
 * drivers/soc/qcom/{smem,smp2p,qcom_aoss,rpmh-rsc,cmd-db,mdt_loader}.c,
 * drivers/mailbox/qcom-ipcc.c, drivers/remoteproc/qcom_q6v5{,_pas}.c,
 * drivers/firmware/qcom_scm{,-smc}.c, drivers/spmi/spmi-pmic-arb.c,
 * drivers/regulator/qcom_spmi-regulator.c, drivers/pinctrl/qcom/pinctrl-msm.c.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/redwood_qrtr.h>
#include <sys/redwood_sta.h>
#include <sys/redwood_net.h>
#include <sys/proc.h>
#include <mach/task_info.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);

static LCK_GRP_DECLARE(wifi_lock_group, "redwood-wifi");

/*
 * What the audio DSP's start (redwood_adsp.c) uses as well, while this may be at work beside it:
 * the mailbox to the always-on processor, one message at a time, and the allocation of items in
 * the shared memory (whose hardware lock both take under the same number, so it does not keep
 * the two apart).
 */
LCK_MTX_DECLARE(redwood_aop_mailbox_lock, &wifi_lock_group);
LCK_MTX_DECLARE(redwood_smem_alloc_lock, &wifi_lock_group);
/* ... and the sound's (redwood_sound.c): a free match register and context of the SMMU, found and taken */
LCK_MTX_DECLARE(redwood_smmu_lock, &wifi_lock_group);

static uint32_t last_step, pause_ms = 2200, fine, watch_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";

/* As in redwood_adreno.c: every line goes to the log the loader reads from another core as well. */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static LCK_MTX_DECLARE(say_lock, &wifi_lock_group);      /* two threads talk here: a line at a time */
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * With wifi=1 what the steps and the driver say is not for the log of an ordinary session: it is
 * kept here, the last of it, and shown only if the radio does not come up. What that log is for
 * is said with redwood_wifi_tell().
 */
static bool quiet;
static char kept[64 * 1024];
static uint32_t kept_at;
static bool kept_wrapped;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (quiet && !aloud) {
		for (const char *c = line; *c != 0; c++) {
			kept[kept_at] = *c;
			if (++kept_at == sizeof(kept)) {
				kept_at = 0;
				kept_wrapped = true;
			}
		}
	} else {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		printf("%s", line);
	}
	lck_mtx_unlock(&say_lock);
}

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

/* (for redwood_wlfw.c and the driver's side, which say their lines the same way) */
int
redwood_wifi_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return 0;
}

/* A line for whoever runs the phone, not only for whoever looks for a fault: said also when the rest is kept. */
int
redwood_wifi_tell(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return 0;
}
#define tell redwood_wifi_tell

/* The last of what was kept, into the log after all: the radio has not come up. */
static void
say_kept(void)
{
	uint32_t length = kept_wrapped ? sizeof(kept) : kept_at, show = length < 24 * 1024 ? length : 24 * 1024;
	char piece[201];

	quiet = false;
	for (uint32_t i = length - show; i < length; i += sizeof(piece) - 1) {
		uint32_t n = length - i < sizeof(piece) - 1 ? length - i : sizeof(piece) - 1;

		for (uint32_t k = 0; k < n; k++) {
			piece[k] = kept[(kept_wrapped ? kept_at + i + k : i + k) % sizeof(kept)];
		}
		piece[n] = 0;
		say_line(piece, true);
	}
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* ---- options --------------------------------------------------------------------------------------- */

static bool whole;                      /* wifi=1: all of it, by itself (see the top of this file) */
static volatile bool whole_runs;        /* ... and the thread that does it is at work */

/*
 * A number given to the kernel when it was started; with wifi=1 the ones that stands for are as if
 * given, unless given otherwise (wifi_irq=0: the rings looked at as before).
 */
uint32_t
redwood_wifi_option(const char *name)
{
	static const struct {
		const char *name;
		uint32_t value;
	} stands_for[] = { { "wifi_lx", 2 }, { "wifi_smmu", 2 }, { "wifi_station", 1 }, { "wifi_keep", 1 }, { "wifi_irq", 1 },
			   { "wifi_ba", 1 }, { "wifi_wide", 1 } };
	uint32_t value = 0;

	if (strcmp(name, "wifi_quiet") == 0) {
		return quiet;
	}
	if (PE_parse_boot_argn(name, &value, sizeof(value))) {
		return value;
	}
	for (unsigned i = 0; whole && i < sizeof(stands_for) / sizeof(stands_for[0]); i++) {
		if (strcmp(name, stands_for[i].name) == 0) {
			return stands_for[i].value;
		}
	}
	return 0;
}

/* ---- register access ------------------------------------------------------------------------------- */

/* One access is one plain load or store (see redwood_adreno.c: the hypervisor carries some of them out). */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* A stretch of registers; mapped when a step first needs it. */
struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(bool single_access)
{
	if (step >= new_from && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

/* What a step is about to do, in a line. */
static void
announce(const char *what)
{
	printf("wifi: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (step >= new_from && fine) {
		printf("wifi: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("wifi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("wifi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	hold(true);
	wr(b->base + offset, value);
}

/* ---- step 1: the shared memory area ------------------------------------------------------------------- */

/*
 * dt: reserved-memory smem@80900000 (qcom,smem). At its end, in the last 4K, the boot loader's
 * table of partitions: one per pair of processors that share items privately, and one, the
 * "global" one, for the rest. A partition begins with a header; items are allocated from both
 * of its ends, each behind (from the top: in front of) an entry that says which item it is and
 * how long. Who is who: we are processor 0; the Wi-Fi DSP is 13 (dt: qcom,remote-pid of
 * qcom,smp2p-wpss). Nothing here writes to this memory.
 */
static unsigned long smem_phys = 0x80900000UL;     /* r5q: 0x86000000 (wifi_survey) */
#define SMEM_PHYS               smem_phys
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
static uint16_t smem_remote = 13;                  /* r5q: the modem, processor 1 */
#define SMEM_WPSS               smem_remote
#define SMEM_GLOBAL             0xfffe          /* both "hosts" of the global partition */
#define SMEM_VERSION            (0x40 + 4 * 7)  /* of the old header at the start: the boot loader's word, [31:16] */
#define SMEM_VERSION_PARTITIONS 12              /* ... which is this where everything is in partitions */
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];                       /* "$TOC" */
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];                       /* "$PRT" */
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;                          /* with the padding */
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
/* An item found: where its data is in the area, and how long. */
struct smem_item {
	uint32_t item, offset, size;
};

/* The DSP's items (dt: qcom,smem of qcom,smp2p-wpss, qcom,smem-id of qcom,wpss@8a00000). */
static uint32_t item_smp2p_ours = 616, item_smp2p_theirs = 617, item_crash_reason = 626;   /* r5q (modem): 428, 435, 421 */
#define ITEM_SMP2P_OURS         item_smp2p_ours         /* SMP2P from us to the DSP: ours to allocate, when there is something to say */
#define ITEM_SMP2P_THEIRS       item_smp2p_theirs       /* ... and from the DSP to us */
#define ITEM_CRASH_REASON       item_crash_reason       /* a line of text the DSP leaves when it gives up */

static const volatile uint8_t *smem;

/* A piece of the area, copied: other processors write there while we look. */
static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		/* memory, not registers; and from the kernel's own map: where ml_io_map() puts things there are 8 MB in all */
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

/* A word of the area, written in one store: other processors read there while we write. */
static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

/* The partition of two processors: where it begins in the area, and how its entries are aligned. 0: there is none. */
static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

/* The items of a partition, up to `max` of them; how many there are. */
static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	/* from the bottom: an entry, then its item */
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	/* from the top: an entry, with its item in front of it; entries are aligned as the table says */
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	const uint16_t hosts[2][2] = { { SMEM_US, SMEM_WPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

/* An SMP2P item: who it is from and to, and up to 16 named words. */
struct smp2p {
	uint32_t magic;                         /* "$SMP" */
	uint32_t version;                       /* [7:0]; [31:8] features: bit 0, "restarts are acknowledged" */
	uint16_t local, remote, total, valid;
	uint32_t flags;                         /* bit 0 "restart done", bit 1 "restart acknowledged" */
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u              /* the feature: a side that restarts says so in its flags, the other answers in its own */
#define SMP2P_RESTART_DONE      (1u << 0)       /* flags: toggled by the side that has restarted */
#define SMP2P_RESTART_ACK       (1u << 1)       /* ... and made equal to it by the other */
/* where the words of an item are, for writing them one at a time */
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12              /* entries there is room for; << 16: entries that are valid */
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,wpss@8a00000). */
#define DSP_FATAL               (1u << 0)       /* "err-fatal": it has given up */
#define DSP_READY               (1u << 1)       /* "err-ready": it is up, and would say so if it gave up */
#define DSP_HANDOVER            (1u << 2)       /* "proxy-unvote": it has asked for its own power; ours is no longer needed */
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* The DSP's SMP2P item as it is now, if there is one. */
static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;                     /* (an item, once allocated, stays where it is) */
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

/* Which of an item's entries has that name; -1: none. */
static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* The word of the DSP's entry "slave-kernel". */
static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("wifi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

/*
 * The DSP's last words, if it has left any: its firmware makes the item when it starts, empty,
 * and writes a line into it when it gives up.
 */
static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("wifi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("wifi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("wifi: %u: THE DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* A partition, in two or more lines. */
static void
show_partition(const char *name, uint16_t one, uint16_t other)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(one, other, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("wifi: %u: there is no partition for %s\n", step, name);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("wifi: %u: the partition for %s: at %#lx, %#x bytes, of which %#x free; %u items\n", step, name, SMEM_PHYS + partition,
	    size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("wifi: %u:   items (bytes):%s\n", step, line);
	}
}

static bool
step_smem(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smp2p words;
	uint32_t version = 0, size = 0, at;

	announce("map the shared memory area (0x80900000, 2 MB: memory, not registers) and read how the boot loader laid it out");
	if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
		return false;
	}
	printf("wifi: %u: the boot loader's version word is %#x; the table of partitions begins %02x %02x %02x %02x, version %u, "
	    "%u entries\n", step, version, table.magic[0], table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
	if (version >> 16 != SMEM_VERSION_PARTITIONS || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 || table.count > 64) {
		printf("wifi: %u: that is not the layout this knows (version 12, everything in partitions)\n", step);
		return false;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			printf("wifi: %u: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", step, i,
			    entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);

	at = smem_item(ITEM_SMP2P_OURS, &size);
	printf("wifi: %u: item %u, SMP2P from us to the DSP: %s\n", step, ITEM_SMP2P_OURS, at ? "there" : "not there");
	show_crash_reason();
	if (dsp_words(&words)) {
		show_words("item 617, SMP2P from the DSP to us, IS THERE (the DSP has run since the phone was started)", &words);
	} else {
		printf("wifi: %u: item %u, SMP2P from the DSP to us: not there (the DSP has not run since the phone was started)\n", step,
		    ITEM_SMP2P_THEIRS);
	}
	return true;
}

/* ---- step 2: the always-on processor's mailbox, and the doorbells -------------------------------------- */

/*
 * dt: power-controller@c300000 (qcom,sm8350-aoss-qmp; "qmp" is the name of the protocol): 1K
 * of message memory that takes 32-bit accesses only. It begins with a description of the two
 * directions -- "ucore" is the always-on processor, "mcore" is us -- each with a link state, a
 * channel state, the other side's acknowledgement of both, and where and how big its mailbox
 * is. A message is text; it goes into our mailbox behind its length, the doorbell is rung, and
 * the always-on processor has taken it when the length reads zero.
 */
static struct block aoss = { "mailbox", 0x0c300000, 0x400 };
#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c      /* "LIAM" */
#define QMP_VERSION             0x04
#define QMP_FEATURES            0x08
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10            /* ours to write: we have seen their link state */
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18            /* ours to write */
#define QMP_THEIR_BOX_SIZE      0x1c
#define QMP_THEIR_BOX           0x20
#define QMP_OUR_LINK            0x24            /* ours to write */
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c            /* ours to write */
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64              /* what Linux sends, whatever the text's length */

/*
 * dt: qcom,ipcc@408000 (its interrupt: SPI 229). A doorbell is a processor ("client") and one
 * of its signals; clients: the always-on processor 0, the Wi-Fi DSP 24; signals: 0 the mailbox
 * or GLINK, 2 SMP2P (dt: the mboxes of power-controller@c300000, qcom,smp2p-wpss, glink wpss).
 */
static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };
#define IPCC_SEND               0x0c            /* write client << 16 | signal: ring it */
#define IPCC_RECEIVED           0x10            /* who rang us, the same way; all ones: nobody */
#define IPCC_ENABLE             0x14            /* write: hear that client's signal */
#define IPCC_DISABLE            0x18            /* write: no longer */
#define IPCC_CLEAR              0x1c            /* write: that one has been heard */
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_WPSS               24u
#define IPCC_SMP2P              2u              /* the signal */

/*
 * r5q (Galaxy S10 Lite, SM8150): the peer is the modem, started by the loader (modem=2), and there is no IPCC: every
 * doorbell is a bit of APCS IPC, 0x17c0000c (live DT: glink mpss mboxes <apcs 12>, smp2p-modem qcom,ipc <.. 0 14>,
 * qmp-aop irq-mask 1). Nothing here touches IPCC on that phone.
 */
static bool r5q_modem;
static vm_offset_t r5q_apcs;

static void
r5q_ring(unsigned bit)
{
	if (r5q_apcs == 0) {
		r5q_apcs = ml_io_map(0x17c00000, 0x1000);
	}
	wr(r5q_apcs + 0xc, 1u << bit);
}

/* the DSP's doorbell for that signal (IPCC_SMP2P, or 0: GLINK) */
static void
dsp_ring(uint32_t signal)
{
	if (r5q_modem) {
		r5q_ring(signal == IPCC_SMP2P ? 14 : 12);
	} else {
		wr(ipcc.base + IPCC_SEND, IPCC_WPSS << 16 | signal);
	}
}

static bool
step_mailbox(void)
{
	uint32_t magic, box, size;

	announce("read the description at the start of the always-on processor's message memory (0xc300000)");
	magic = get(&aoss, "magic", QMP_MAGIC);
	get(&aoss, "version", QMP_VERSION);
	get(&aoss, "features", QMP_FEATURES);
	get(&aoss, "their link", QMP_THEIR_LINK);
	get(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK);
	get(&aoss, "their channel", QMP_THEIR_CHANNEL);
	get(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK);
	get(&aoss, "their mailbox's size", QMP_THEIR_BOX_SIZE);
	get(&aoss, "their mailbox", QMP_THEIR_BOX);
	get(&aoss, "our link", QMP_OUR_LINK);
	get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK);
	get(&aoss, "our channel", QMP_OUR_CHANNEL);
	get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	if (magic != QMP_MAGIC_VALUE) {
		printf("wifi: %u: that is not the mailbox's description\n", step);
		return false;
	}
	if (size >= QMP_MESSAGE + 4 && box >= 0x3c && box + size <= aoss.size) {
		get(&aoss, "the length of the message in our mailbox", box);
	}

	announce("read who has rung our doorbell (IPCC, 0x408000: the first access to that block)");
	get(&ipcc, "received", IPCC_RECEIVED);
	return true;
}

/* ---- step 3: RPMh -------------------------------------------------------------------------------------- */

/* dt: reserved-memory cmd_db@80860000: the boot loader's list of what RPMh can switch (cmd-db.c; as in redwood_adreno.c) */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static bool
cmd_db_there(void)
{
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);       /* memory, not registers */
	}
	return cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c;
}

/* The resource of that name, or (name NULL) the one an address belongs to: the nearest at or below it. */
static const struct cmd_db_entry *
rpmh_resource(const char *name, uint32_t address)
{
	const struct cmd_db_entry *best = NULL;
	char id[8] = { 0 };

	if (!cmd_db_there()) {
		return NULL;
	}
	if (name != NULL) {
		strlcpy(id, name, sizeof(id));
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (name != NULL ? memcmp(entry[i].id, id, sizeof(id)) == 0 :
			    entry[i].address <= address && address - entry[i].address < 0x10 &&
			    (best == NULL || entry[i].address > best->address)) {
				best = &entry[i];
				if (name != NULL) {
					return best;
				}
			}
		}
	}
	return best;
}

static void
show_rpmh_resource(const char *name, const char *what)
{
	const struct cmd_db_entry *entry = rpmh_resource(name, 0);
	const uint8_t *data;
	char levels[160];
	int at = 0;

	if (entry == NULL) {
		printf("wifi: %u: RPMh has no \"%s\" (%s)\n", step, name, what);
		return;
	}
	levels[0] = 0;
	if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".lvl") == 0) {
		/* a rail asked for by level: the levels it has, which a request names by position */
		for (unsigned s = 0; s < 8; s++) {
			const struct cmd_db_slave *slave = &cmd_db->slave[s];
			const struct cmd_db_entry *first = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

			if (entry < first || entry >= first + slave->count ||
			    (unsigned)slave->data_offset + entry->offset + entry->length > CMD_DB_SIZE - 0x1000) {
				continue;
			}
			data = cmd_db->data + slave->data_offset + entry->offset;
			at = snprintf(levels, sizeof(levels), "; levels:");
			for (unsigned l = 0; l < entry->length / 2u && l < 16 && at < (int)sizeof(levels) - 8; l++) {
				unsigned level = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && level == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level);
			}
		}
	}
	printf("wifi: %u: RPMh has \"%s\" at %#x (%s)%s\n", step, name, entry->address, what, levels);
}

/*
 * dt: rsc@18200000 (qcom,rpmh-rsc), the application processor's sequencer for requests to RPMh;
 * of its three sets of registers ("DRV") the operating system's is the third (qcom,drv-id 2).
 * Requests wait in "TCS"s of up to 16 commands, a command being an address of RPMh's and a
 * value: by the device tree two TCSs for requests made at once, then three for what is to
 * hold while the processor sleeps and three for when it wakes. What the slots hold now is what
 * was last put there by whoever ran here before us: the boot loader.
 */
static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_ID                  0x000           /* [23:16] the major version: the layout below is that of versions before 3 */
#define RSC_CONFIG              0x00c           /* per DRV six bits, the number of its TCSs; [31:27] commands in a TCS */
#define RSC_TCS(n)              (0xd00 + 672 * (n))             /* dt: qcom,tcs-offset */
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18
#define TCS_ENABLED             0x1c            /* a bit per command */
#define TCS_COMMAND(c)          (0x30 + 20 * (c))               /* its flags; then its address, its value, its status */

static bool
step_rpmh(void)
{
	static const char *const kinds[] = { "at once", "at once", "asleep", "asleep", "asleep", "awake", "awake", "awake" };
	uint32_t id, config, count, commands;

	announce("look up the supplies involved in the boot loader's list of RPMh resources (memory at 0x80860000)");
	if (!cmd_db_there()) {
		printf("wifi: %u: the list is not there\n", step);
		return false;
	}
	show_rpmh_resource("cx.lvl", "the DSP's logic; dt: vdd_cx of qcom,wpss@8a00000, asked 384 while it boots");
	show_rpmh_resource("mx.lvl", "its memories; dt: vdd_mx, the same");
	show_rpmh_resource("xo.lvl", "the crystal oscillator; dt: its clock \"xo\"");
	show_rpmh_resource("smpb7", "PM7325 S7: the radio's vdd-cx-mx");
	show_rpmh_resource("smpb1", "PM7325 S1: the radio's vdd-1.8-xo");
	show_rpmh_resource("smpb8", "PM7325 S8: the radio's vdd-1.3-rfa");

	announce("read the application processor's RPMh sequencer (0x18220000: the first access to that block): its version, "
	    "and the commands its slots hold");
	id = get(&rsc, "id", RSC_ID);
	config = get(&rsc, "configuration", RSC_CONFIG);
	if (((id >> 16) & 0xff) >= 3) {
		printf("wifi: %u: version %u of the sequencer has its registers elsewhere: not read\n", step, (id >> 16) & 0xff);
		return true;
	}
	count = (config >> 12) & 0x3f;
	commands = (config >> 27) & 0x1f;
	printf("wifi: %u: the operating system's part has %u TCSs of %u commands\n", step, count, commands);
	for (uint32_t t = 0; t < count && t < 8; t++) {
		uint32_t enabled = rd(rsc.base + RSC_TCS(t) + TCS_ENABLED);

		printf("wifi: %u: TCS %u (%s): commands enabled %#x, control %#x, status %#x\n", step, t, kinds[t], enabled,
		    rd(rsc.base + RSC_TCS(t) + TCS_CONTROL), rd(rsc.base + RSC_TCS(t) + TCS_STATUS));
		for (uint32_t c = 0; c < commands && c < 16; c++) {
			uint32_t flags = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c));
			uint32_t address = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 4);
			uint32_t value = rd(rsc.base + RSC_TCS(t) + TCS_COMMAND(c) + 8);
			const struct cmd_db_entry *entry = rpmh_resource(NULL, address);

			if (address == 0 && !(enabled & 1u << c)) {
				continue;
			}
			printf("wifi: %u:   command %u%s: %#x (%.8s + %u) = %#x, flags %#x\n", step, c, enabled & 1u << c ? "" :
			    " (not enabled)", address, entry ? entry->id : "?", entry ? address - entry->address : 0, value, flags);
		}
	}
	return true;
}

/* ---- step 4: the supplies themselves ------------------------------------------------------------------- */

/*
 * The PMIC arbiter, as in redwood_buttons.c: reads go through the "observer" channels, which
 * every system has for every peripheral, whoever owns it. dt: qcom,spmi@c440000; on the bus the
 * PM7325 is slave 1 and the PM8350C slave 2.
 */
static struct block arb_core = { "arbiter", 0x0c440000, 0x1100 };
static struct block arb_cnfg = { "arbiter's owners", 0x0c40a000, 0x1000 };
static struct block arb_observer = { "arbiter's observer", 0x0e600000, 0x10000 };
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */

static uint32_t arb_channels;

/* The channel of a peripheral (slave << 8 | peripheral), as Linux chooses: the first, unless a later one is ours. -1: none. */
static int
spmi_channel(uint16_t id)
{
	int found = -1;

	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core.base + ARB_CHANNEL(n));

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		if (found < 0 || (rd(arb_cnfg.base + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
			found = (int)n;
		}
	}
	return found;
}

/* Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what went wrong. */
static const char *
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t at = arb_observer.base + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
	uint64_t start = now_us();
	uint32_t status, data[2];

	wr(at + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	do {
		status = rd(at + OBSERVER_STATUS);
	} while (!(status & STATUS_DONE) && now_us() - start < 2000);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	}
	if (status & STATUS_FAILURE) {
		return "the transfer failed (nobody at that address?)";
	}
	if (status & STATUS_DROPPED) {
		return "the transfer was dropped";
	}
	data[0] = rd(at + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(at + OBSERVER_DATA1) : 0;
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/*
 * Where the PMICs have their regulators is not in the device tree (on this phone they are
 * RPMh's to switch, and the tree knows them by RPMh's names only), so the arbiter's table is
 * asked which peripherals each PMIC has, and each peripheral what it is (registers 0x04 and
 * 0x05, its type and subtype). Of a regulator's control (qcom_spmi-regulator.c, the kinds
 * "ftsmps426" and "hfs430" that PMICs of this generation continue): 0x08 its status (bit 7: its
 * output is in regulation), 0x40/0x41 the voltage asked for in millivolts, 0x42/0x43 the voltage
 * it makes, 0x45 its mode (the low three bits: 4 retention, 5 low power, 6 automatic, 7 full
 * power), 0x46 bit 7: enabled. Whether a rail that is on was left on by the boot loader for good
 * or is held by somebody's request to RPMh, these registers do not say.
 */
#define TYPE_BUCK               0x03            /* a switching regulator's control */
#define TYPE_FTS                0x1c            /* ... of the fast kind */
#define TYPE_ULT_BUCK           0x22

static bool
step_rails(void)
{
	uint32_t version;

	announce("map the PMIC arbiter (0xc440000, 0xc40a000, 0xe600000; the buttons' driver reads through it too) and read "
	    "its version");
	version = get(&arb_core, "version", ARB_VERSION);
	arb_channels = get(&arb_core, "features", ARB_FEATURES) & 0x7ff;
	need(&arb_cnfg);
	need(&arb_observer);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("wifi: %u: this knows version 5 of the arbiter only: the supplies are not read\n", step);
		return true;
	}
	if (arb_channels == 0 || arb_channels > 512) {
		arb_channels = 512;
	}

	announce("ask every peripheral of the PM7325 (slave 1) and the PM8350C (slave 2) what it is, and the switching "
	    "regulators among them what they make (the first transfers of ours on the SPMI bus: for each a command word "
	    "written to the arbiter, nothing to a PMIC)");
	for (uint16_t slave = 1; slave <= 2; slave++) {
		static uint8_t types[256][2];
		static bool there[256];
		char line[320];
		int at = 0;

		bzero(there, sizeof(there));
		for (uint32_t n = 0; n < arb_channels; n++) {
			uint32_t map = rd(arb_core.base + ARB_CHANNEL(n)), peripheral = (map >> 8) & 0xff;

			if (map == 0 || ((map >> 16) & 0xf) != slave || there[peripheral]) {
				continue;
			}
			there[peripheral] = spmi_read((int)n, 0x04, types[peripheral], 2) == NULL;
		}
		line[0] = 0;
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			if (there[peripheral]) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x:%x/%x", peripheral, types[peripheral][0],
				    types[peripheral][1]);
			}
			if (at > 0 && (at > (int)sizeof(line) - 80 || peripheral == 255)) {
				printf("wifi: %u: slave %u (%s), peripheral:type/subtype:%s\n", step, slave, slave == 1 ? "PM7325" : "PM8350C",
				    line);
				at = 0;
			}
		}
		for (unsigned peripheral = 0; peripheral < 256; peripheral++) {
			int channel;
			uint8_t status[2], control[8];
			const char *problem;

			if (!there[peripheral] || (types[peripheral][0] != TYPE_BUCK && types[peripheral][0] != TYPE_FTS &&
			    types[peripheral][0] != TYPE_ULT_BUCK)) {
				continue;
			}
			channel = spmi_channel((uint16_t)(slave << 8 | peripheral));
			problem = channel < 0 ? "no channel" : spmi_read(channel, 0x08, status, 2);
			if (problem == NULL) {
				problem = spmi_read(channel, 0x40, control, 8);
			}
			if (problem != NULL) {
				printf("wifi: %u: slave %u, the regulator at %#x: %s\n", step, slave, peripheral << 8, problem);
				continue;
			}
			printf("wifi: %u: slave %u, the regulator at %#x (type %#x/%#x): %s, status %#x %#x (%s); asked %u mV, makes %u "
			    "mV; mode %#x; registers 0x40..0x47: %02x %02x %02x %02x %02x %02x %02x %02x\n", step, slave, peripheral << 8,
			    types[peripheral][0], types[peripheral][1], control[6] & 0x80 ? "ENABLED" : "NOT ENABLED", status[0], status[1],
			    status[0] & 0x80 ? "in regulation" : "not in regulation", (unsigned)(control[0] | control[1] << 8),
			    (unsigned)(control[2] | control[3] << 8), control[5], control[0], control[1], control[2], control[3], control[4],
			    control[5], control[6], control[7]);
		}
	}
	return true;
}

/* ---- step 5: pins -------------------------------------------------------------------------------------- */

/*
 * dt: pinctrl@f000000 (pin n's registers: 0x1000 * n above 0xf100000). Pin 84 is described there
 * as the radio's enable ("cnss_wlan_en": output, high when active), but no node uses the
 * description; Bluetooth's node calls the same pin "wl-reset" and has 85 as its own reset and 86
 * as its "sw-ctrl".
 *
 * (The DSP's clocks -- three in the global clock controller have its name: gcc-sc7280.c,
 * gcc_wpss_ahb_clk at 0x9d154 and two more -- are not looked at: a read there is answered with
 * an external abort, which ends the kernel. They are TrustZone's; the device tree gives the
 * loader's node the crystal alone.)
 */
#define FIRST_PIN               84
static struct block tlmm = { "tlmm", 0x0f100000 + 0x1000 * FIRST_PIN, 0x3000 };        /* the three pins' registers */
#define GPIO_CFG(n)             (0x1000 * ((n) - FIRST_PIN))            /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (0x1000 * ((n) - FIRST_PIN) + 4)        /* bit 0 in, bit 1 out */

static bool
step_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { 84, "the radio's enable or reset" }, { 85, "Bluetooth's reset" }, { 86, "Bluetooth's sw-ctrl" } };

	announce("read how the firmware left the radio's pins (registers the touch screen's driver uses too)");
	need(&tlmm);
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(tlmm.base + GPIO_CFG(pins[i].pin)), io = rd(tlmm.base + GPIO_IO(pins[i].pin));

		printf("wifi: %u: gpio%u (%s): configuration %#x: function %u, pull %u, %u mA, %s; reads %u\n", step, pins[i].pin,
		    pins[i].what, cfg, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & 1u << 9) ? (io & 2 ? "DRIVES HIGH" :
		    "DRIVES LOW") : "an input", io & 1);
	}
	return true;
}

/* ---- step 6: the firmware ------------------------------------------------------------------------------ */

#define PAS_WPSS                6               /* dt: qcom,pas-id of qcom,wpss@8a00000 */
#define WPSS_MEMORY             0x9ae00000u     /* dt: reserved-memory wlan@9ae00000, that node's memory-region */
#define WPSS_MEMORY_SIZE        0x01900000u

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);


/*
 * A file of fw/ in the small image that travels with the kernel -- userland/build.sh puts what
 * is in fw-wifi/ there --, found in the memory of the RAM disk itself (see redwood_adreno.c).
 */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	const uint8_t *data = redwood_image_file("fw", name, size, NULL);  /* (redwood_image.c) */

	if (data != NULL) {
		return data;
	}
	printf("wifi: %u: no fw/%s in the small image: put the phone's /vendor/firmware_mnt/image/%s into fw-wifi/ and run "
	    "userland/build.sh\n", step, name, name);
	return NULL;
}

/*
 * The image as its header describes it (mdt_loader.c): a 32-bit ELF whose header, table of
 * segments and block of hashes and certificates are the file wpss.mdt, and whose loadable
 * segments are a file each, wpss.bNN for segment NN. The segments are "relocatable": they go
 * where the memory reserved for the DSP is, which on this phone is where they say they are.
 */
#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)      /* the segment that is the block of hashes */
#define MAX_SEGMENTS            16

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;                    /* in the DSP's memory */
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;                          /* of the memory all of them take, from the start of the DSP's */
} image;

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the DSP's image in the small image that came with the kernel, and check it against its own header");
	image.segments = 0;
	header = image_file("wpss.mdt", &size);
	if (header == NULL) {
		return false;
	}
	/* a 32-bit ELF: the count of segments at 44, the table of them at 52, 32 bytes each */
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("wifi: %u: fw/wpss.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;                       /* (not one that is loaded) */
		}
		if (segment.paddr < WPSS_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)WPSS_MEMORY + WPSS_MEMORY_SIZE) {
			printf("wifi: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			return false;
		}
		image.segment[image.segments].at = segment.paddr - WPSS_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "wpss.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				return false;
			}
			if (file_size != segment.file_size) {
				printf("wifi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				return false;
			}
			files += file_size;
		}
		printf("wifi: %u: segment %u: %#x bytes at %#x, of which %#x from its file\n", step, i, segment.memory_size, segment.paddr,
		    segment.file_size);
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != WPSS_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("wifi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		image.segments = 0;
		return false;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - WPSS_MEMORY;
	printf("wifi: %u: the image is whole: a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x bytes "
	    "of the DSP's %#x from %#x\n", step, size, hashes, image.segments, files, image.size, WPSS_MEMORY_SIZE, WPSS_MEMORY);
	return true;
}

/* ---- step 7: the mailbox, opened ----------------------------------------------------------------------- */

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: the mailbox is not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring(uint32_t client, uint32_t signal, const char *whose)
{
	if (r5q_modem) {
		printf("wifi: %u: %s: APCS IPC bit %u\n", step, whose, client == IPCC_AOP ? 0 : signal == IPCC_SMP2P ? 14 : 12);
		r5q_ring(client == IPCC_AOP ? 0 : signal == IPCC_SMP2P ? 14 : 12);
		return;
	}
	put(&ipcc, whose, IPCC_SEND, client << 16 | signal);
}

static struct {
	uint32_t glink, smp2p, mailbox, other;  /* rings heard, by kind */
	uint32_t said;                          /* how many have been written to the log */
	uint64_t since;
} doorbell;

/*
 * Who has rung us (see the end of this file for what IPCC is seen to do): each pending doorbell
 * is read and told that it has been heard, which is what an interrupt handler would do before
 * anything else. The first few are written to the log.
 */
static void
doorbells(void)
{
	if (r5q_modem) {
		return;                         /* (no IPCC; the modem's rings are SPIs 449 and 451, and the queues are polled) */
	}
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		if (who == (IPCC_WPSS << 16)) {
			doorbell.glink++;
		} else if (who == (IPCC_WPSS << 16 | IPCC_SMP2P)) {
			doorbell.smp2p++;
		} else if (who == (IPCC_AOP << 16)) {
			doorbell.mailbox++;
		} else {
			doorbell.other++;
		}
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("wifi: doorbell: +%llu.%03llu ms: client %u, signal %u; heard and cleared\n", now / 1000, now % 1000, who >> 16,
			    who & 0xffff);
		}
	}
}

/*
 * Linux's qmp_open(), a step at a time: acknowledge their link, raise ours and wait for their
 * acknowledgement, raise our channel and wait for theirs, acknowledge that and wait for their
 * acknowledgement of ours. Whatever of it has been done already -- by a kernel before this one:
 * the always-on processor is restarted only with the phone -- is found done and left.
 */
static bool step_mailbox_open_locked(void);

static bool
step_mailbox_open(void)
{
	bool open;

	lck_mtx_lock(&redwood_aop_mailbox_lock);
	open = step_mailbox_open_locked();
	lck_mtx_unlock(&redwood_aop_mailbox_lock);
	return open;
}

static bool
step_mailbox_open_locked(void)
{
	uint32_t box, size;

	announce("open the mailbox to the always-on processor: words of the message memory, and its doorbell");
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("wifi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("wifi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (get(&aoss, "our link", QMP_OUR_LINK) == QMP_UP && get(&aoss, "our link, seen by them", QMP_OUR_LINK_ACK) == QMP_UP &&
	    get(&aoss, "our channel", QMP_OUR_CHANNEL) == QMP_UP && get(&aoss, "their channel", QMP_THEIR_CHANNEL) == QMP_UP &&
	    get(&aoss, "our channel, seen by them", QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("wifi: %u: the mailbox is open already (a kernel before this one opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, get(&aoss, "their link", QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second (it says %#x); our link is "
		    "taken down again\n", step, rd(aoss.base + QMP_OUR_LINK_ACK));
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: the always-on processor has seen our link\n", step);
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second (it says %#x); ours is taken down "
		    "again\n", step, rd(aoss.base + QMP_THEIR_CHANNEL));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring(IPCC_AOP, 0, "the always-on processor's doorbell");
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second (it says %#x); it is taken "
		    "down again\n", step, rd(aoss.base + QMP_OUR_CHANNEL_ACK));
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring(IPCC_AOP, 0, "the always-on processor's doorbell");
		return false;
	}
	printf("wifi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN; the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()). */
static bool mailbox_send_locked(const char *text);

static bool
mailbox_send(const char *text)
{
	bool sent;

	if (leaving) {
		return mailbox_send_locked(text);       /* (one processor left, and no lock taken: see redwood_wifi_quiesce()) */
	}
	lck_mtx_lock(&redwood_aop_mailbox_lock);
	sent = mailbox_send_locked(text);
	lck_mtx_unlock(&redwood_aop_mailbox_lock);
	return sent;
}

static bool
mailbox_send_locked(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };

	if (our_box == 0) {
		return false;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("wifi: %u: about to tell the always-on processor \"%s\": %u bytes into our mailbox at %#x, its length in "
	    "front, and the doorbell\n", step, text, QMP_MESSAGE, aoss.phys + our_box);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	hold(true);
	if (rd(aoss.base + our_box) != 0) {
		printf("wifi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		return false;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("wifi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		return false;
	}
	printf("wifi: %u: the always-on processor has taken it\n", step);
	return true;
}

/* ---- SMP2P, our half ----------------------------------------------------------------------------------- */

/*
 * Our item (616) is made by step 10, in our partition with the DSP, and like everything in the
 * shared memory it then stays until the phone is restarted: a kernel after this one finds it.
 * Our entries in it (dt: the children of qcom,smp2p-wpss that are not interrupt controllers):
 * "master-kernel", whose bit 0 asks the DSP to stop (dt: "qcom,force-stop" of qcom,wpss@8a00000),
 * and "wlan", whose bit 0 is the radio driver's to tell the firmware that we sleep. A change is
 * followed by a ring of the DSP's SMP2P doorbell.
 */
#define OUR_ENTRY_STOP          "master-kernel"
#define OUR_ENTRY_RADIO         "wlan"
#define DSP_STOP                (1u << 0)       /* of "master-kernel" */

static void glink_reset(void);
static void link_stop(void);

static uint32_t our_item;               /* where our SMP2P item is in the area; 0: not looked for, or not there */
static bool smp2p_agreed;               /* the DSP's item and ours have the same version: what follows applies */
static bool smp2p_restart_acks;         /* ... and both sides have the feature */
static bool smp2p_restart_acked;        /* the DSP's "restart done" as we have last answered it */

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/*
 * What Linux does when the DSP rings for SMP2P (smp2p.c: qcom_smp2p_intr), done instead whenever
 * the DSP's item is seen to have changed: agree on the features once, and answer a restart.
 */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			printf("wifi: %u: about to write our item's features at %#lx: %#x (what both sides have)\n", step,
			    SMEM_PHYS + our_item + SMP2P_AT_VERSION, features);
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("wifi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("wifi: %u: the DSP's flags say it has restarted (%#x): about to answer in our flags at %#lx (bit 1 = %u) and ring "
		    "its SMP2P doorbell\n", step, theirs->flags, SMEM_PHYS + our_item + SMP2P_AT_FLAGS, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		dsp_ring(IPCC_SMP2P);
	}
}

/*
 * Linux's qcom_q6v5_request_stop(): our bit "stop" set, the DSP's "stop ack" waited for (five
 * seconds there too), our bit taken back. It is asked of a DSP that says it is up and has said
 * neither that it has given up nor that it has stopped, through whatever item of ours is in the
 * shared memory -- this kernel's or, for a DSP left running by a kernel before, that kernel's.
 * True: the DSP has stopped by its own word, now or (its entry still says so) before.
 */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("wifi: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("wifi: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("wifi: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("wifi: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("wifi: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx (it holds %#x), and its SMP2P doorbell\n",
	    step, OUR_ENTRY_STOP, SMEM_PHYS + value_at, ours.entry[entry].value);
	snprintf(last_write, sizeof(last_write), "step %u asked the DSP to stop", step);
	hold(true);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	dsp_ring(IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("wifi: %u: %s after %llu.%03llu ms; its entry says %#x, its flags %#x\n", step, state & DSP_STOP_ACK ?
	    "THE DSP ACKNOWLEDGES THE STOP" : "THE DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000, (now - asked) % 1000, state,
	    theirs.flags);
	/* (taken back, as Linux does, before TrustZone is asked) */
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	dsp_ring(IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

/* ---- step 8: TrustZone takes the header ---------------------------------------------------------------- */

/* The calls, as in redwood_adreno.c (qcom_scm.c: qcom_scm_pas_*; convention: qcom_scm-smc.c). */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* two: a number, and the address of memory it may read and write */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */

static LCK_MTX_DECLARE(dsp_lock, &wifi_lock_group);     /* the DSP being started or stopped */
static bool dsp_started;                /* TrustZone has let it out of reset, and has not shut it down since */
static bool stopped_by_its_word;        /* the shutdown last asked for was of a DSP that had acknowledged a stop */
static uint32_t dirty;

/* One call, as Linux makes these: one that may be preempted. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("wifi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("wifi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("wifi: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

/*
 * The DSP is asked to stop, if it can be; then, whatever it says, TrustZone is to shut it down
 * (it takes the image's memory back, cleared), and the always-on processor to know that the
 * image is gone: it was told of one by us, or by the kernel that left the DSP running.
 */
static void radio_space_off(void);

static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;
	bool ours = dsp_started;                /* (not the DSP of a kernel before this one, shut down before ours is started) */

	printf("wifi: %u: %s: the DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		redwood_lx_stop();                      /* (the driver, if it stayed up: it leaves its network and goes down in order) */
		redwood_wlfw_stop(step);                /* (what the stand-in of step 15 has to say first, while there is a link) */
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the Wi-Fi DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_WPSS, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT SHUT THE DSP DOWN: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: THE WI-FI DSP IS SHUT DOWN\n", step);
	wr(ipcc.base + IPCC_DISABLE, IPCC_WPSS << 16 | IPCC_SMP2P);     /* (as the boot chain left it) */
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
	/*
	 * Nobody is left to use the radio's stream. Only after a DSP of ours: with wifi=1 the radio
	 * is shown its memory before the DSP is started, and the shutdown of an earlier kernel's
	 * DSP, which comes in between, took that away again -- the radio's first reach for memory,
	 * at mission mode, was then a stream the SMMU knows nothing of, and the phone went down
	 * without a word: every second start, and never a first.
	 */
	if (ours) {
		radio_space_off();
	}
	return true;
}

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	uint32_t again = 0;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		/* (see the top of this file: it has run since the phone was started, and may be running) */
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down a DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !(PE_parse_boot_argn("wifi_again", &again, sizeof(again)) && again)) {
			printf("wifi: %u: THE DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and a start "
			    "after that has reset the phone (see the top of redwood_wifi.c; wifi_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("wifi: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started, and "
		    "there is nothing to shut down\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 6), in a page of ours");
	/* in memory of ours, page-aligned and in one piece, not touched again and never freed (see redwood_adreno.c) */
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("wifi: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));       /* TrustZone reads memory, not this processor's cache */
	}
	settle();
	result = scm("to take the Wi-Fi DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_WPSS, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TRUSTZONE ACCEPTS THE WI-FI DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_WPSS, WPSS_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("wifi: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, WPSS_MEMORY);
	return true;
}

/* ---- step 9: the image, in the DSP's memory ------------------------------------------------------------ */

static bool image_written;

/* Eight bytes of a segment as they are to be in the DSP's memory, from byte `at` of it: the file's, then zeroes. */
static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the DSP's memory (0x9ae00000, 25 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	/*
	 * (The memory is looked at only now: TrustZone has just said that it is not its own. And it
	 * is mapped for as long as this takes: 25 MB do not fit where ml_io_map() puts what stays.)
	 */
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(WPSS_MEMORY, WPSS_MEMORY_SIZE, VM_WIMG_WCOMB);
	printf("wifi: %u: the memory begins %#llx, the image %#llx\n", step, dsp_memory[0], segment_word(0, 0));
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
			printf("wifi: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
			return false;
		}
		printf("wifi: %u: about to write %#x bytes at %#x (%#x of them the file's)\n", step, image.segment[s].memory_size,
		    WPSS_MEMORY + image.segment[s].at, image.segment[s].file_size);
		snprintf(last_write, sizeof(last_write), "step %u wrote a segment of the image at %#x", step,
		    WPSS_MEMORY + image.segment[s].at);
		hold(true);
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	printf("wifi: %u: %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step, written, cleared,
	    wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, WPSS_MEMORY_SIZE);
	image_written = wrong == 0;
	return image_written;
}

/* ---- step 10: our SMP2P item --------------------------------------------------------------------------- */

/*
 * The SoC's lock for allocating in the shared memory: dt: syscon@1f40000 and the hwlock
 * "qcom,tcsr-mutex" on it (0x1000 a lock), of which qcom,smem has the fourth ("hwlocks": 3). A
 * register that takes the number of whoever writes it while it holds zero, and keeps it until
 * zero is written (qcom_hwspinlock.c). The application processor's number is 1. It is held
 * around the allocation only, and released on every way out; Linux waits a second for it,
 * and so does this: a lock that somebody keeps is said, not waited for.
 */
static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one. False, and why said: not made. */
static bool smem_make_locked(uint32_t partition, uint32_t item, uint32_t data_size);

static bool
smem_make(uint32_t partition, uint32_t item, uint32_t data_size)
{
	bool made;

	lck_mtx_lock(&redwood_smem_alloc_lock);
	made = smem_make_locked(partition, item, data_size);
	lck_mtx_unlock(&redwood_smem_alloc_lock);
	return made;
}

static bool
our_item_make(uint32_t partition)
{
	return smem_make(partition, ITEM_SMP2P_OURS, (uint32_t)sizeof(struct smp2p));
}

static bool
smem_make_locked(uint32_t partition, uint32_t item, uint32_t data_size)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (data_size + 7) & ~7u, need = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	owner = get(&smem_lock, "owner", LOCK_OWNER);
	printf("wifi: %u: about to take the lock (write %u at %#x, until it reads back so, for a second at most), write an entry "
	    "of %u bytes for item %u with %u bytes behind it where the partition's items from the bottom end, move the "
	    "partition's mark by %u, and release the lock (write 0)\n", step, LOCK_US, smem_lock.phys + LOCK_OWNER,
	    (uint32_t)sizeof(entry), item, item_size, need);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	hold(true);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("wifi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		return false;
	}
	/* under the lock: the partition as it is now, every entry in it from the bottom, and the room left */
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("wifi: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("wifi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need > header.free_cached) {
		printf("wifi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need, header.free_cached - at);
		goto out;
	}
	/* the entry first, whole; only then the mark, in one store: nobody sees half of either */
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - data_size);     /* padding behind the item; none in front */
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("wifi: %u: the lock is released (it reads %u)%s\n", step, rd(smem_lock.base + LOCK_OWNER), made ? "; the entry is at "
	    "the old mark, the mark moved" : "");
	return made;
}

static bool
step_our_item(void)
{
	static const char *const donor_names[] = { OUR_ENTRY_STOP, OUR_ENTRY_RADIO };
	static const char *const modem_names[] = { OUR_ENTRY_STOP, "ipa" };
	const char *const *names = r5q_modem ? modem_names : donor_names;
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition;

	announce("look at our partition with the DSP in the shared memory, and for an SMP2P item of ours (616) in it");
	partition = smem_partition(SMEM_US, SMEM_WPSS, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("wifi: %u: there is no such partition\n", step);
		return false;
	}
	show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	printf("wifi: %u: its items from the bottom end at +%#x, those from the top begin at +%#x\n", step, header.free_uncached,
	    header.free_cached);
	our_item = our_item_find();
	if (our_item != 0) {
		printf("wifi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		announce("make the item: the SoC's lock for the shared memory (0x1f43000: the first access to that block) around it");
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("wifi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("wifi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
		show_partition("us and the Wi-Fi DSP (0 and 13)", SMEM_US, SMEM_WPSS);
	}

	announce("fill our item in (340 bytes at the address above): zeroes, then from processor 0 to 13, room for 16 entries, "
	    "the entries \"master-kernel\" and \"wlan\" at zero, feature bit 0, and last the version, which makes it valid; "
	    "then the DSP's SMP2P doorbell, as Linux rings it");
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_WPSS << 16);
	for (uint32_t i = 0; i < 2; i++) {
		uint32_t name[4] = { 0 };

		strlcpy((char *)name, names[i], sizeof(name));
		for (uint32_t word = 0; word < 4; word++) {
			smem_poke(our_item + SMP2P_AT_NAME(i) + 4 * word, name[word]);
		}
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 2u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	ring(IPCC_WPSS, IPCC_SMP2P, "the DSP's SMP2P doorbell");
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("wifi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);

	announce("see that our two marks in GLINK's descriptor are at zero before the DSP starts");
	glink_reset();
	return true;
}

/* ---- step 11: the DSP, started ------------------------------------------------------------------------- */

/*
 * Linux's order (qcom_q6v5_pas.c: adsp_start): the always-on processor is told, then TrustZone
 * asked. Linux also asks RPMh for the crystal and for CX and MX at level 384 until the DSP says
 * it has asked for its own power ("proxy" requests, dt: qcom,proxy-reg-names); that is not done
 * here -- no supply is touched from this file --, and the DSP starts on what the boot loader
 * left: level 256 for both (step 3). Then the shared memory and our doorbell are watched, every
 * 200 microseconds: for the DSP's item to appear, for every change in it, for a ring; and what
 * SMP2P's rules have us answer is answered (dsp_listen()).
 *
 * What has been seen: 6.5 ms after TrustZone returns the DSP rings our doorbell for GLINK. At
 * 32.5 ms its SMP2P item is there (on a later start in the same session of the phone: is
 * written afresh, with "restart done" in its flags, which we answer) -- version 1, feature bit
 * 0, from processor 13 to 0, the entries "smp2p", "slave-kernel" and soon "wlan". At 50 to 60 ms
 * "slave-kernel" says 6: it is up, and has asked for its own power (Android's kernel sees the
 * same word 39 ms after its start). Without an item of ours -- before step 10 existed -- the
 * DSP's item stayed without entries for as long as it was watched.
 * Asked to stop, the DSP answers within 13 ms: "slave-kernel" becomes 0x10e, bit 3 the
 * acknowledgement.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	bool rung;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written || our_box == 0) {
		printf("wifi: %u: the image is not in place, or the mailbox not open\n", step);
		return false;
	}
	PE_parse_boot_argn("wifi_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	if (have) {
		show_words("before the start, the DSP's item says (from its last run; believed again once it changes)", &last);
	}
	announce("tell the doorbells that rings from before this kernel have been heard and that the DSP's SMP2P ring is "
	    "listened for, tell the always-on processor that the image is loaded, and ask TrustZone to authenticate it and let "
	    "the DSP out of reset");
	/* (nobody answered them, and a ring after the start is to be the DSP's) */
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	/* (the boot chain listens for the DSP's GLINK ring and no other: without this an SMP2P ring leaves no trace) */
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_WPSS << 16 | IPCC_SMP2P);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!mailbox_send("{class: image, res: load_state, name: wpss, val: on}")) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the Wi-Fi DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_WPSS, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;                  /* (whatever the answer: the memory is not ours to look at now) */
	if (result != 0 || answer != 0) {
		printf("wifi: %u: TRUSTZONE DOES NOT START THE DSP: the call returns %d, its answer is %#llx\n", step, result, answer);
		mailbox_send("{class: image, res: load_state, name: wpss, val: off}");
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("wifi: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE WI-FI DSP OUT OF RESET; watching the shared memory "
	    "and our doorbell for %u s\n", step, wait_seconds);

	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = true;
			spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + watch_seconds * 1000000ull;
				printf("wifi: %u: THE WI-FI DSP SAYS IT IS UP: its entry \"slave-kernel\" says %#x (bit 1: \"error ready\"), "
				    "%llu.%03llu ms after TrustZone started it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("wifi: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start\n", step,
				    (now - started) / 1000, (now - started) % 1000);
			}
			if (state & DSP_FATAL) {
				printf("wifi: %u: THE WI-FI DSP SAYS IT HAS GIVEN UP (bit 0 of \"slave-kernel\")\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	if (have) {
		show_words("at the end of the watch the DSP's item says", &last);
	}
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("and ours", &words);
	}
	rung = doorbell.glink + doorbell.smp2p != 0;
	printf("wifi: %u: the watch is over after %llu ms; doorbells heard: GLINK %u, SMP2P %u, the mailbox %u, others %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at != 0) {
		printf("wifi: %u: the DSP said it was up%s\n", step, handed_over ? ", and that it has asked for its own power" : "");
	} else if (spoke) {
		printf("wifi: %u: THE WI-FI DSP RUNS BUT HAS NOT SAID \"UP\": it has %s%s, with %u entries\n", step, rung ?
		    "rung our doorbell" : "", have ? (rung ? " and has its item in the shared memory" :
		    "its item in the shared memory") : "", last.valid);
	} else {
		printf("wifi: %u: NOT A WORD FROM THE DSP: no ring, and %s\n", step, have ? "its item in the shared memory is as a "
		    "kernel before this one left it" : "no item of its in the shared memory");
	}
	return ready_at != 0 || spoke;
}

/* ---- GLINK ------------------------------------------------------------------------------------------- */

/*
 * GLINK between us and the DSP is two queues of bytes in the shared memory and a descriptor
 * (Linux: qcom_glink_smem.c). Items of our partition with the DSP: 478 the descriptor, four
 * words; 479 our queue, which we write and the DSP reads (both made by the boot chain: they are
 * there before anything of ours runs); 480 the DSP's queue, which it makes when it starts. Each
 * side moves the head of its own queue and the tail of the other's, and rings the other's
 * GLINK doorbell when it has moved one. Our two marks are put to zero before the DSP starts
 * (step 10): it reads our queue from the start when it comes up.
 *
 * What goes through the queues (qcom_glink_native.c) are messages of eight bytes -- a command
 * and two parameters -- some with more behind them, each padded to eight: first the two sides'
 * versions, then channels by name, each side giving a channel a number of its own. To send on
 * a channel a side needs an "intent" of the other's: the other's promise of a buffer of some
 * size, with a number; a message names the intent it uses, and the receiver says when it is
 * done with it and whether it may be used again. A side without a fitting intent asks for one.
 *
 * The DSP opens three channels, and we answer one: "IPCRTR", which carries QRTR (below).
 */
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0               /* the descriptor's words: how far the DSP has read our queue (its to move) */
#define GLINK_OUR_HEAD          4               /* how far we have written it */
#define GLINK_THEIR_TAIL        8               /* how far we have read the DSP's queue */
#define GLINK_THEIR_HEAD        12              /* how far it has written it (its to move) */
#define GLINK_RESERVE           16              /* of our queue, never filled (Linux's reserve, and room for one short message) */
#define IPCC_GLINK              0u              /* the signal */

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u              /* the feature Linux offers over this transport */
#define GLINK_CHANNEL           "IPCRTR"
#define GLINK_US                1               /* our number for that channel */

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {                           /* a whole message from the channel, waiting for the link's thread */
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

static LCK_MTX_DECLARE(glink_lock, &wifi_lock_group);   /* the queues' marks and everything below */
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;      /* where in the shared memory */
	bool up;                                /* the queues are found and ours to use */
	bool broken;                            /* something in the DSP's queue was not understood: nothing more is read */
	uint32_t features;
	bool agreed;                            /* the versions are settled */
	uint32_t their_channel;                 /* the DSP's number for IPCRTR; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
	uint32_t said, heard, trace;            /* messages each way; how many more are written to the log */
} glink;

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

/* The three items; false if one is missing or not of a shape this knows. */
static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

/* Our two marks to zero, if they are not: before the DSP starts (see above). */
static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("wifi: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("wifi: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("wifi: %u: about to write our two marks there (+4, +8): 0\n", step);
		hold(true);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

/* How much of the DSP's queue is unread. */
static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

/* How much we may write into our queue now. */
static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

/* A message into our queue, in two pieces, padded to eight bytes; then our mark, and the DSP's doorbell. */
static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	dsp_ring(IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

/* A short message of ours. */
static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("wifi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

/* An intent of ours of that size: a buffer, and the DSP told of it. False: there are no more. */
static bool
glink_offer(uint32_t size)
{
	uint32_t words[2] = { size, glink.our_intent_count + 1 }, i = glink.our_intent_count;

	if (i >= sizeof(glink.our_intents) / sizeof(glink.our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (glink.our_intents[i].data == NULL || glink.our_intents[i].size != size) {
		glink.our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);       /* (kept for good: a later link uses it again) */
	}
	if (glink.our_intents[i].data == NULL) {
		return false;
	}
	glink.our_intents[i].size = size;
	glink.our_intents[i].fill = 0;
	glink.our_intent_count++;
	glink_say(GLINK_INTENT, GLINK_US, 1, words, sizeof(words));
	return true;
}

/*
 * Everything the DSP has put into its queue, taken out and acted on: Linux's
 * qcom_glink_native_rx(). With glink_lock held; whoever waits for something from the DSP calls
 * this, so it must not wait itself. Whole messages for the channel are left in a list for the
 * link's thread.
 */
static void
glink_pump(void)
{
	static const struct {
		uint32_t size, count;
	} intents[] = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } };           /* dt: qcom,intents of qcom,wpss_qrtr */
	struct glink_message message;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);                /* the size of this piece, and what is still to come */
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("wifi: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;                                 /* (the rest of it is on its way) */
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE) {
			glink.trace--;
			printf("wifi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			if (strcmp(name, GLINK_CHANNEL) != 0) {
				printf("wifi: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				break;
			}
			printf("wifi: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, GLINK_US);
			glink.their_channel = message.param1;
			glink_say(GLINK_OPEN_ACK, glink.their_channel, 0, NULL, 0);
			if (!glink.asked) {
				bzero(name, sizeof(name));
				strlcpy(name, GLINK_CHANNEL, sizeof(name));
				glink_say(GLINK_OPEN, GLINK_US, sizeof(GLINK_CHANNEL), name, (sizeof(GLINK_CHANNEL) + 7) & ~7u);
				glink.asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_US) {
				glink.acknowledged = true;
			}
			break;
		case GLINK_INTENT:
			for (uint32_t i = 0; i < message.param2 && message.param1 == glink.their_channel; i++) {
				glink_peek(words, 8 + 8 * i, 8);        /* a size and a number */
				for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
					if (!glink.their_intents[n].there || glink.their_intents[n].id == words[1]) {
						glink.their_intents[n].there = true;
						glink.their_intents[n].in_use = false;
						glink.their_intents[n].size = words[0];
						glink.their_intents[n].id = words[1];
						break;
					}
				}
				if (glink.trace > 0) {
					printf("wifi: glink:   the DSP will take %u bytes into its intent %u\n", words[0], words[1]);
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
				if (glink.their_intents[n].there && glink.their_intents[n].id == message.param2) {
					glink.their_intents[n].in_use = false;
					glink.their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if (message.param1 == glink.their_channel) {
				bool granted = glink_offer(message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, GLINK_US, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			glink.request_answered = true;
			glink.request_granted = message.param2 != 0;
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			if (message.param1 != glink.their_channel || intent >= glink.our_intent_count ||
			    words[0] > glink.our_intents[intent].size - glink.our_intents[intent].fill) {
				printf("wifi: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(glink.our_intents[intent].data + glink.our_intents[intent].fill, 16, words[0]);
			glink.our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				/* whole: into the list, and the intent is the DSP's to use again */
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + glink.our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = glink.our_intents[intent].fill;
					memcpy(packet->data, glink.our_intents[intent].data, packet->length);
					if (glink.last != NULL) {
						glink.last->next = packet;
					} else {
						glink.first = packet;
					}
					glink.last = packet;
				}
				glink.our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, GLINK_US, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			dsp_ring(IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if (message.param1 == glink.their_channel) {
				printf("wifi: glink: THE DSP CLOSES \"%s\"\n", GLINK_CHANNEL);
				glink.open = false;
				glink.their_channel = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("wifi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (glink.their_channel != 0 && glink.acknowledged && !glink.open) {
			glink.open = true;
			printf("wifi: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
			    GLINK_CHANNEL, GLINK_US, glink.their_channel);
			for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
				for (unsigned n = 0; n < intents[i].count; n++) {
					glink_offer(intents[i].size);
				}
			}
		}
	}
}

/*
 * One whole message to the DSP on the channel (Linux's __qcom_glink_send(), for messages that
 * go in one piece). Waits, up to five seconds, for an intent of the DSP's that it fits and
 * for room in our queue, asking for the intent if there is none.
 */
static int
glink_send(const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, GLINK_US, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !glink.open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
			if (!glink.their_intents[n].there || glink.their_intents[n].size < length) {
				continue;
			}
			fits = true;                            /* (one that is in use comes back: that is waited for) */
			if (!glink.their_intents[n].in_use && (found < 0 || glink.their_intents[n].size < glink.their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			glink.request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, GLINK_US, length, NULL, 0);
			asked = true;
		}
		if (waited >= (r5q_modem ? 50000u : 5000u) || (!fits && asked && glink.request_answered && !glink.request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		if (r5q_modem) {
			sleep_us(100);          /* (r5q: the modem's Wi-Fi loader is against a grace timer: finer waits) */
		} else {
			sleep_ms(1);
		}
		lck_mtx_lock(&glink_lock);
	}
	glink.their_intents[found].in_use = true;
	header.message.param2 = glink.their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR -------------------------------------------------------------------------------------------- */

/*
 * What the channel carries (Linux: net/qrtr/af_qrtr.c, ns.c, smd.c): datagrams from a port of a
 * node to a port of a node, one in each GLINK message, behind a header of eight words. A node's
 * services announce themselves to the other nodes through a port that all have, the control
 * port: when a link comes up each side says hello, and is told the other's services one by one
 * ("new server": a service, an instance, a node, a port), and later their going. We are node 1
 * (Linux's default, and what this phone's Android is); the DSP says who it is in what it sends.
 * The interface for the rest of the kernel is sys/redwood_qrtr.h: the four calls at the end of
 * this section, handed to the QMI helpers (bsd/dev/redwood_qmi.c), which is what speaks to the
 * services.
 */
#define REDWOOD_QRTR_MAX        8192            /* the most a datagram holds, either way */
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu     /* the control port */
#define QRTR_FIRST_PORT         0x4000u         /* where Linux begins numbering ports that ask for no number */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7 };

struct qrtr_header {                            /* version 1, which Linux sends; version 2 is read as well */
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

struct redwood_qrtr_port {
	uint32_t number;                        /* 0: free */
	redwood_qrtr_receive_t receive;
	redwood_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		bool fresh;                     /* not yet told of the services that were there before it */
	} lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &wifi_lock_group);    /* the tables below */
static LCK_MTX_DECLARE(qrtr_calling, &wifi_lock_group); /* held by the link's thread around a callback */
static struct redwood_qrtr_port qrtr_ports[96];     /* (Test 381: 16 -> 96; iOS's QMI clients each take one, Test 380) */
static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer;                      /* the DSP's node; 0: it has not said */
static uint32_t qrtr_next_port = QRTR_FIRST_PORT, qrtr_in, qrtr_out, qrtr_hellos;
static thread_t link_thread_is;

/* One datagram out, of any type. */
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	if (length == 0 || length > REDWOOD_QRTR_MAX) {
		return EMSGSIZE;
	}
	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static bool
qrtr_matches(uint32_t service, uint32_t instance, unsigned server)
{
	return qrtr_servers[server].there && qrtr_servers[server].service == service &&
	       (instance == 0 || qrtr_servers[server].instance == instance);
}

/* Everybody who looked a service up is told that it has come, or gone. From the link's thread. */
static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			redwood_qrtr_server_t server = NULL;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked && !qrtr_ports[p].lookups[l].fresh &&
			    qrtr_ports[p].lookups[l].service == service &&
			    (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance)) {
				server = qrtr_ports[p].server;
			}
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
		}
	}
}

/*
 * r5q (Test 87): a lookup from the modem for a service of ours, answered as Linux's name service (net/qrtr/ns.c:
 * ctrl_cmd_new_lookup) answers one: every matching server of this node as a NEW_SERVER to the asker, then an empty
 * NEW_SERVER that ends the list. No services of ours yet, so the answer is the end alone ("not here yet"); the asker
 * waits for a NEW_SERVER later. Which services the modem asks for is what the log is for.
 */
static uint32_t r5q_lookups;

static const char *
r5q_qrtr_lookup(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control end = { .command = QRTR_NEW_SERVER };

	(void)control;
	r5q_lookups++;
	return qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, header->from_node, header->from_port, &end, sizeof(end)) == 0 ?
	       ": answered, not here (yet)" : ": THE ANSWER COULD NOT BE SENT";
}

/* The control port's part: Linux's name service, without services of our own to tell of. */
static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;                                  /* (answered once: two that answer every hello never end) */
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("wifi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		lck_mtx_unlock(&qrtr_lock);
		printf("wifi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		qrtr_tell(control->words[0], control->words[1], control->words[2], control->words[3], true);
		break;
	case QRTR_DEL_SERVER:
	case QRTR_BYE:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			bool gone;

			lck_mtx_lock(&qrtr_lock);
			gone = qrtr_servers[i].there && (control->command == QRTR_BYE ? qrtr_servers[i].node == header->from_node :
			    qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]);
			qrtr_servers[i].there = qrtr_servers[i].there && !gone;
			lck_mtx_unlock(&qrtr_lock);
			if (gone) {
				printf("wifi: qrtr: service %u at node %u port %u has gone\n", qrtr_servers[i].service, qrtr_servers[i].node,
				    qrtr_servers[i].port);
				qrtr_tell(qrtr_servers[i].service, qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port, false);
			}
		}
		break;
	case 10:                                        /* QRTR_TYPE_NEW_LOOKUP: the peer wants a service of ours */
	case 11:                                        /* QRTR_TYPE_DEL_LOOKUP */
		printf("wifi: qrtr: node %u port %u %s service %u (%#x), version %u, instance %u%s\n", header->from_node,
		    header->from_port, control->command == 10 ? "LOOKS UP" : "no longer looks up", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->command == 10 ? r5q_qrtr_lookup(header,
		    control) : "");
		break;
	default:
		break;                                  /* (a client's going, and what else there is: nothing here wants it) */
	}
}

/* A message from the channel: one datagram. From the link's thread. */
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	redwood_qrtr_receive_t receive = NULL;
	void *context = NULL;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		/* version 2: version, type, flags, a length of options; the size; then four numbers of 16 bits */
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("wifi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("wifi: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;                                 /* (we never ask for one: GLINK's intents hold us back already) */
	}
	if (header.confirm) {
		/* the sender counts what it has sent to this port, and waits for this before it sends many more */
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number == header.to_port && header.type == QRTR_DATA) {
			receive = qrtr_ports[p].receive;
			context = qrtr_ports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive != NULL) {
		receive(context, header.from_node, header.from_port, data + at, header.size);
	} else {
		static uint32_t strays;         /* (Test 103: what goes to ports nobody here has) */

		if (strays++ < 16) {
			printf("wifi: qrtr: type %u, %u bytes from %u:%u to port %u, which nobody has\n", header.type, header.size,
			    header.from_node, header.from_port, header.to_port);
		}
	}
}

/* Lookups made since the last look are told of the services that were there before them. From the link's thread. */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			redwood_qrtr_server_t server;
			uint32_t service, instance;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].fresh) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qrtr_ports[p].lookups[l].fresh = false;
			server = qrtr_ports[p].server;
			service = qrtr_ports[p].lookups[l].service;
			instance = qrtr_ports[p].lookups[l].instance;
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && server != NULL; i++) {
				if (qrtr_matches(service, instance, i)) {
					server(context, true, qrtr_servers[i].service, qrtr_servers[i].instance & 0xff,
					    qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
				}
			}
		}
	}
}

static struct redwood_qrtr_port *
qrtr_open(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
	struct redwood_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > REDWOOD_QRTR_MAX) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

static void
qrtr_close(struct redwood_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };
	bool outside = current_thread() != link_thread_is;

	if (port == NULL || port->number == 0) {
		return;
	}
	if (outside) {
		lck_mtx_lock(&qrtr_calling);            /* (a callback of the port's that is running ends first) */
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (outside) {
		lck_mtx_unlock(&qrtr_calling);
	}
	if (qrtr_peer != 0) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

static const struct redwood_qrtr_ops qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };

/* ---- the link's thread -------------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;
/*
 * Until the DSP may have said something. Nothing tells us today, so: a millisecond. This is the
 * one function to change when the doorbells' interrupt can be taken (the end of this file): it
 * then blocks until the handler has seen a ring of the DSP's.
 */
static void
link_wait(void)
{
	sleep_us(r5q_modem ? 100 : 1000);
}

/* Everything the DSP says once it runs: its SMP2P item, its queue, and from that the datagrams, handed on from here. */
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		link_wait();
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("wifi: link: THE WI-FI DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = glink.first;
			if (packet != NULL) {
				glink.first = packet->next;
				glink.last = glink.first == NULL ? NULL : glink.last;
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			lck_mtx_lock(&qrtr_calling);
			qrtr_input(packet->data, packet->length);
			lck_mtx_unlock(&qrtr_calling);
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		lck_mtx_lock(&qrtr_calling);
		qrtr_fresh();
		lck_mtx_unlock(&qrtr_calling);
	}
	link_runs = false;
	thread_terminate(current_thread());
}

/* The link given up, before the DSP is stopped: Linux too takes GLINK down first and tells the DSP nothing of it. */
static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("wifi: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; doorbells heard: "
		    "GLINK %u, SMP2P %u, the mailbox %u, others %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = glink.open = false;
	while ((packet = glink.first) != NULL) {
		glink.first = packet->next;
		kfree_data(packet, sizeof(*packet) + packet->length);
	}
	glink.last = NULL;
	lck_mtx_unlock(&glink_lock);
	lck_mtx_lock(&qrtr_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
	lck_mtx_unlock(&qrtr_lock);
}

/* ---- step 12: GLINK's queues, looked at --------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues in the shared memory, and read what the DSP has put into its own "
	    "(nothing is taken out)");
	if (!glink_find()) {
		printf("wifi: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("wifi: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("wifi: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;                                  /* (how long the others are, the reading below knows) */
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 13: GLINK, spoken ---------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!(dsp_started || r5q_modem) || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the DSP: our version into our queue and its doorbell, then whatever its messages call for "
	    "(writes: our queue, our two marks in the descriptor, the doorbell); a thread of ours listens from here on, "
	    "every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.asked = glink.acknowledged = glink.open = glink.broken = false;
	glink.their_channel = glink.our_intent_count = glink.said = glink.heard = 0;
	bzero(glink.their_intents, sizeof(glink.their_intents));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("wifi: %u: no thread\n", step);
		return false;
	}
	link_thread_is = thread;
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);                                   /* (for the intents each side tells the other of, to be in the log) */
	printf("wifi: %u: %llu ms after our version: versions %s (features %#x); the channel \"%s\" is %s\n", step,
	    (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features, GLINK_CHANNEL, glink.open ?
	    "OPEN" : "NOT OPEN");
	return glink.open;
}

/* ---- step 14: QRTR ------------------------------------------------------------------------------------- */

#define SERVICE_WLFW            0x45            /* the Wi-Fi firmware's service, the one the radio's driver talks to */

static uint32_t wlfw_node, wlfw_port;
static volatile uint32_t wlfw_answer;           /* bytes of an answer received; 0: none */
static uint8_t wlfw_answer_bytes[64];

static void
wlfw_service(__unused void *context, bool arrived, __unused uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
	printf("wifi: qrtr: THE WI-FI FIRMWARE'S SERVICE (%#x) %s node %u port %u\n", SERVICE_WLFW, arrived ? "IS AT" : "has left",
	    node, port);
	wlfw_node = arrived ? node : 0;
	wlfw_port = arrived ? port : 0;
}

static void
wlfw_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	printf("wifi: qrtr: a datagram of %lu bytes from node %u port %u\n", (unsigned long)length, node, port);
	memcpy(wlfw_answer_bytes, data, length < sizeof(wlfw_answer_bytes) ? length : sizeof(wlfw_answer_bytes));
	wlfw_answer = (uint32_t)length;
}

/* ---- step 15: the exchange with the Wi-Fi firmware's service ---------------------------------------- */

static const uint8_t *image_file(const char *name, uint64_t *size);

const uint8_t *
redwood_wifi_file(const char *name, uint64_t *size)
{
	return image_file(name, size);
}

static void radio_space_faults(void);

/*
 * With wifi_lx: ath11k itself in place of the stand-in. Its qmi.c has the exchange, core.c asks
 * for mission mode with the configuration ce.c and hal.c work out (the shadow registers), has
 * them set the copy engines' rings up in the radio's registers and waits for the firmware's
 * first message over a copy engine; how far the parts built so far go from there, and what is
 * in their place meanwhile, is wifi/lx/src/lx_bus.c's to say. wifi_lx=2 IS WHERE THE RADIO'S
 * REGISTERS ARE WRITTEN.
 *
 * What the phone said (13 October, ath11k of Linux 7.0.14: core.c, hal.c, qmi.c, ce.c, hw.c,
 * fw.c): with wifi_lx=1 the driver's own exchange runs to the end in 9 s -- capabilities, the
 * regulatory database in 4 pieces and the board's file in 10, cold-boot calibration (7 s),
 * "ready", the configuration with the 30 shadow registers the driver works out, mission mode
 * -- and the mode "off" when the driver winds down. With wifi_lx=2 and wifi_smmu=2, 100 words
 * of ring set-up go into the registers of copy engines 0, 1, 2, 3, 4, 5 and 7 without an
 * abort, 510 buffers are posted on each of the three receiving rings, and 9 ms later the
 * firmware's first message is in a buffer of copy engine 2: HTC's "ready" (2 credits of 2176
 * bytes, 5 endpoints). The SMMU records no fault. No interrupt is involved: the rings are
 * looked at every millisecond.
 *
 * With htc.c and wmi.c built as well (the same day): 29 ms after the bus is started HTC has the
 * firmware's "ready" and asks for the WMI service -- the first thing sent over a copy engine;
 * it is given endpoint 1, up on copy engine 3 and down on 2 --, the firmware announces the
 * radio over WMI ("service ready", in three events, within 60 ms), takes the driver's
 * configuration and says "unified ready" 270 ms after the bus was started. Of the radio it
 * says: one, of two chains, for 2.4 and 5 GHz (2312..2732 and 4900..7125 MHz, and capabilities
 * for 6 GHz); the rules of the regulatory domain it starts in (CN: 2402..2482, 5170..5330,
 * 5735..5835 MHz); and no address of its own (all zeros: the tree's "use-nv-mac" has Android
 * give it one). Then the driver is taken down in order, mode "off" included.
 *
 * With mac.c, reg.c and peer.c built, and with wifi_scan=1 (the same day): the driver registers
 * its radio with what stands in for mac80211 (wifi/lx/src/lx_80211.c) -- 14 channels at
 * 2.4 GHz, 28 at 5, 60 at 6, of which the firmware's regulatory domain allows 13, 13 and none
 * --, takes the address it was given, starts the radio, makes an interface in station mode
 * and has the firmware scan those 26 channels, listening only. The scan takes 4.1 s (150 ms a
 * channel), and what the radio heard is in the log: 5 to 7 access points in three runs, at
 * 2.4 and at 5 GHz, the strongest at -40 dBm, the weakest at -84, noise at -108 to -111. The
 * firmware works a country out of what it hears and the driver sets it (after the scan). Then
 * the interface is removed, the radio stopped, and the driver's own removal runs to its end.
 * The data path is still stubs: the firmware scans without it.
 *
 * With the data path built (dp.c and its neighbours: 18 of the driver's files, no stubs) and
 * wifi_join=1 (the same day): the rings of the data path are set up -- 636 words into the
 * radio's registers before the bus is started, 44 MB of ours shown to the radio -- and HTC
 * connects the data service besides WMI. The station asks for the network it was given by
 * name in a scan (1.9 s), finds its access point (channel 36, -53 dBm; WPA2 with a passphrase,
 * CCMP), authenticates, associates (20 MHz, HT and VHT, two streams) and answers the access
 * point's handshake; 130 ms after the association both keys are with the firmware and frames
 * pass. With wifi_keep=1 and the system started on top, configd's DHCP has an address in 4 to
 * 6 s, names resolve, and pages are fetched over HTTPS: iOS on the network through the
 * phone's own radio. 20 MB come down in 10.5 s (15 Mbit/s) with one processor, the rings
 * looked at by a thread and the radio's buffers not behind the processor's cache; what of
 * that holds the rate down was not taken apart. With four processors the network works as
 * well. At the run's end the network is left, the driver removes itself and the DSP stops
 * when asked.
 *
 * As wifi=1, in the session the phone is ordinarily run in (four processors, the GPU drawing,
 * touch, every stock job on demand), the same day: all of the above on a thread beside the
 * system's start, which is not held up by it (its "first boot is finished" comes 23 to 26 s
 * after the first line from user space, with the radio and without); the network is joined 12 s
 * after the kernel began on the radio; a job of ours that is in every image has configd
 * configure en0 as soon as it appears. Safari's search field then shows the search engine's
 * suggestions as one types, which come over this network. (The pages themselves Safari
 * fetches -- the lock and the site's name appear -- and does not draw: the page is black, with
 * the GPU and without. That is not the network's doing.)
 *
 * A PHONE THAT RESETS WITHOUT A WORD: twice on the way, two seconds after calibration. The
 * driver's registration failed (the hash table peer.c needs was a stub that fails), and where
 * its start fails that late core.c asks for the mode "off" before it stops the bus; our thread
 * was then still writing ring pointers into registers that had stopped answering. The read at
 * step 16 outside mission mode was a bus error and a panic; this was neither, and the loader
 * went with it. The cause was not seen in a log (there was none): it is what the code does,
 * and with a hash table that works and the copy engines let go of before any request for a
 * mode other than mission (lx_bus.c: lx_qmi_send_request()) it has not happened again.
 */
static bool
step_driver(uint32_t how)
{
	int error;

	announce(how >= 2 ? "hand the radio to ath11k itself: its exchange with the firmware up to mission mode, then THE RADIO'S "
	    "REGISTERS ARE WRITTEN, by ce.c and hal.c: the copy engines' rings (every write is said as it is made)" :
	    "hand the firmware to ath11k itself: its own exchange up to mission mode; the radio's registers are neither read "
	    "nor written (wifi_lx=1), and nothing is written but datagrams");
	error = redwood_lx_start();
	radio_space_faults();
	printf("wifi: %u: the driver's start returns %d\n", step, error);
	return error == 0;
}

static bool
step_exchange(void)
{
	uint32_t driver = 0;

	if (!glink.open) {
		return false;
	}
	if ((driver = redwood_wifi_option("wifi_lx")) != 0) {
		return step_driver(driver);
	}
	announce("have the exchange a WCN6750's driver has with the Wi-Fi firmware's service before it touches the radio, by "
	    "hand in ath11k's order (redwood_wlfw.c): indications, capabilities both ways, the regulatory database and the "
	    "board's data; nothing is written but datagrams");
	return redwood_wlfw_exchange(step);
}

/* ---- step 16: the radio's registers, looked at ------------------------------------------------------- */

/*
 * The firmware says where the radio's registers are (step 15: 0x61e00000, 2 MB), and ath11k
 * maps that address as it is and reads and writes there: no SMMU is involved, it is the
 * processor's own view (qmi.c: ath11k_qmi_request_device_info(), devm_ioremap()). In the
 * phone's device tree the address is no device's: it lies in the memory window of the first
 * PCIe controller (pcie@1c00000: "ranges" 0x60300000 + 0x3d00000), whose node the tree keeps
 * for another use. Whether a kernel under this phone's hypervisor may read there is what this
 * step asks, with four reads and no write.
 *
 * THE ANSWER: YES, ONCE THE FIRMWARE IS IN MISSION MODE, AND ONLY THEN. With the firmware
 * calibrated and "ready" but in no mode, the first read ended the kernel with an asynchronous
 * abort -- a bus error, where the DSP's clocks, which the hypervisor keeps to itself, gave a
 * synchronous one -- and half a minute later the phone restarted by itself (the panic had left
 * the DSP running; a panic's way back now stops it, and with that the phone stays up). After
 * the configuration and the mode "mission" (step 15 with wifi_mission=1), which is when ath11k
 * begins to use the registers, all seven words below read: the copy engines' rings at zero,
 * copy engine 1's control 0x80, the reorder block's enable 0x3000150, and 0x40140110 where the
 * chip's PCI siblings have their hardware version. ath11k has no wake-up to make before an
 * access on this chip and no window register to write (ahb.c: the pci_ops for wcn6750 have
 * neither, and the windows are fixed). So the step reads only in mission mode, and only when
 * told to (wifi_radio_read=1; 2: in any state, which HAS ENDED THE KERNEL).
 *
 * The 2 MB are not the chip's registers one to one (ahb.c: ath11k_ahb_get_window_start_wcn6750()):
 * the first 512K are, the second 512K show the chip's 512K at 0xa00000 (the data path), the
 * third those at 0x1b80000 (the copy engines). The words read: the two ath11k itself reads
 * first when it sets the chip up (hal.c: the control word of copy engine 1's destination ring;
 * hw.c: the reorder block's "general enable"), the reorder ring's id word, and the word where
 * this chip's PCI siblings have their hardware version (pci.c: TCSR_SOC_HW_VERSION), which this
 * one may or may not have.
 */
#define RADIO_WINDOW            0x80000u
#define RADIO_DATA_PATH         0x00a00000u     /* hal.h: HAL_SEQ_WCSS_UMAC_OFFSET */
#define RADIO_COPY_ENGINES      0x01b80000u     /* hw.c: wcn6750_regs.hal_seq_wcss_umac_ce0_src_reg */

static bool
step_registers(void)
{
	/*
	 * The first: the word ath11k touches first once the firmware is in mission mode (it writes
	 * it: ce.c: ath11k_ce_init_pipes() begins with copy engine 0's source ring, and hal.c:
	 * ath11k_hal_srng_src_hw_init() with that ring's interrupt address). The rest only if the
	 * first answers: the ring's base and id, the first word ath11k reads (copy engine 1's
	 * destination ring control), the reorder block's two, and the hardware version of this
	 * chip's PCI siblings.
	 */
	static const struct {
		uint32_t chip;                  /* the register's address in the chip */
		const char *name;
	} words[] = {
		{ 0x01b80000 + 0x48, "copy engine 0, source ring, interrupt address (low)" },
		{ 0x01b80000 + 0x00, "copy engine 0, source ring, base (low)" },
		{ 0x01b80000 + 0x08, "copy engine 0, source ring, id" },
		{ 0x01b83000 + 0xb0, "copy engine 1, destination ring control" },
		{ 0x00a38000 + 0x000, "reorder block, general enable" },
		{ 0x00a38000 + 0x1f4, "reorder ring 1, id" },
		{ 0x00000224, "hardware version (if this chip has it where its siblings do)" },
	};
	static struct block radio = { "radio", 0, 0 };
	uint64_t address = 0;
	uint32_t size = 0, asked = 0;
	bool mission = false;

	if (!redwood_wlfw_registers(&address, &size, &mission) || address > 0xffffffffull || size < 3 * RADIO_WINDOW) {
		printf("wifi: %u: the firmware has not said where the radio's registers are (step 15)\n", step);
		return false;
	}
	PE_parse_boot_argn("wifi_radio_read", &asked, sizeof(asked));
	if (asked == 0 || (!mission && asked < 2)) {
		printf("wifi: %u: THE RADIO'S REGISTERS (%#llx) ARE NOT READ: %s (see the top of this step in redwood_wifi.c)\n", step,
		    address, asked == 0 ? "not asked for (wifi_radio_read=1)" : "the firmware is not in mission mode (wifi_mission=1), "
		    "and outside it a read there has ended the kernel with a bus error (wifi_radio_read=2 to read all the same)");
		return false;
	}
	announce("read the radio's registers where the firmware says they are, the word ath11k touches first before any other; "
	    "if nothing answers there the kernel ends at that read");
	printf("wifi: %u: the registers: %#x bytes at %#llx; the firmware %s\n", step, size, address, mission ? "is in mission mode" :
	    "IS NOT IN MISSION MODE (wifi_mission=1 gets it there): the state in which the read failed before");
	radio.phys = (uint32_t)address;
	radio.size = size;
	radio.base = ml_io_map_unmappable(radio.phys, radio.size, VM_WIMG_IO);
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		uint32_t chip = words[i].chip, at;

		/* ahb.c: which of the three windows a register of the chip is seen through, and where in it */
		at = chip < RADIO_WINDOW ? chip : (chip ^ RADIO_DATA_PATH) < RADIO_WINDOW ? RADIO_WINDOW + (chip & (RADIO_WINDOW - 1)) :
		    2 * RADIO_WINDOW + (chip & (RADIO_WINDOW - 1));
		printf("wifi: %u: about to read the radio's %s (the chip's %#x) at %#x\n", step, words[i].name, chip, radio.phys + at);
		snprintf(last_write, sizeof(last_write), "step %u was reading the radio's registers at %#x", step, radio.phys + at);
		sleep_ms(i == 0 ? pause_ms : 50);
		printf("wifi: %u: THE RADIO'S %s (%#x) = %#x\n", step, words[i].name, radio.phys + at, rd(radio.base + at));
	}
	ml_io_unmap(radio.base, radio.size);
	return true;
}

static bool
step_qrtr(void)
{
	struct redwood_qrtr_port *port;
	uint32_t ask = 0, seconds = 3, services = 0;

	if (!glink.open) {
		return false;
	}
	PE_parse_boot_argn("wifi_listen", &seconds, sizeof(seconds));
	announce("listen for QRTR on the channel: the DSP's hello is answered with ours, its services are written down; and a "
	    "port of ours looks the Wi-Fi firmware's service up, as the radio's driver will");
	port = qrtr_open(wlfw_receive, wlfw_service, NULL);
	if (port == NULL || qrtr_lookup(port, SERVICE_WLFW, 1, 0) != 0) {
		printf("wifi: %u: no port\n", step);
		return false;
	}
	for (unsigned ms = 0; ms < seconds * 1000 && !(wlfw_port != 0 && ms >= 500); ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("wifi: %u: QRTR: the DSP is node %u; hellos from it %u; datagrams received %u, sent %u; services it has told of: "
	    "%u; the Wi-Fi firmware's service: %s\n", step, qrtr_peer, qrtr_hellos, qrtr_in, qrtr_out, services, wlfw_port != 0 ?
	    "THERE" : "NOT THERE");
	if (wlfw_port != 0 && PE_parse_boot_argn("wifi_ask", &ask, sizeof(ask)) && ask) {
		/*
		 * A question the service answers without having been told anything first? A QMI request is
		 * seven bytes before its content: 0 for "request", a number of ours for the exchange, the
		 * message's number, the content's length. 0x24 is "capabilities" and has no content.
		 */
		static const uint8_t question[7] = { 0, 0x01, 0x00, 0x24, 0x00, 0x00, 0x00 };
		int error;

		announce("ask the Wi-Fi firmware's service for its capabilities (QMI message 0x24, seven bytes) and wait a second "
		    "for an answer");
		wlfw_answer = 0;
		error = qrtr_send(port, wlfw_node, wlfw_port, question, sizeof(question));
		for (unsigned ms = 0; ms < 1000 && error == 0 && wlfw_answer == 0; ms++) {
			sleep_ms(1);
		}
		if (error != 0 || wlfw_answer == 0) {
			printf("wifi: %u: %s\n", step, error != 0 ? "THE QUESTION COULD NOT BE SENT" : "no answer within a second");
		} else {
			char text[3 * 48 + 1] = "";

			for (uint32_t i = 0; i < wlfw_answer && i < 48; i++) {
				snprintf(text + 3 * i, 4, " %02x", wlfw_answer_bytes[i]);
			}
			printf("wifi: %u: THE SERVICE ANSWERS: %u bytes, beginning%s\n", step, wlfw_answer, text);
		}
	}
	qrtr_close(port);
	return qrtr_hellos != 0 && qrtr_out != 0;
}

/* ---- beside the steps: the radio's stream in the SMMU (boot-arg wifi_smmu=1) ------------------------------ */

/*
 * What the radio reads and writes in memory by itself (its copy engines, the rings of the data
 * path) goes through the application processors' SMMU as stream 0x1c00, with the mask 1 (dt:
 * "iommus" of qcom,wcn6750; the SMMU is apps-smmu@15000000, an Arm MMU-500 like the GPU's).
 * Whether the boot chain leaves that stream passing untranslated, translated or faulting decides
 * what has to be made before the radio is given an address. This reads the SMMU's global
 * registers -- the ones the hypervisor answers for, see redwood_adreno.c -- and says which of
 * its stream match registers cover that stream. Nothing is written.
 */
static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
#define SMMU_SCR0               0x000           /* bit 0: the whole SMMU off; bit 10: a stream nobody matches faults; bit 3:
	                                         * streams are numbered with 16 bits */
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define RADIO_STREAM            0x1c00
#define RADIO_STREAM_MASK       0x0001

static void
smmu_look(void)
{
	static const char *const kinds[] = { "translated by context", "PASSED UNTRANSLATED (bypass; the number is", "MADE TO FAULT "
		                             "(the number is", "of a reserved kind (the number is" };
	uint32_t scr0, idr0, idr1, matches, valid = 0, covering = 0;

	printf("wifi: 0: read the global registers of the application processors' SMMU (0x15000000: the first access of ours to "
	    "that block; the hypervisor answers for them)\n");
	sleep_ms(pause_ms);
	scr0 = get(&apps_smmu, "scr0", SMMU_SCR0);
	idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	idr1 = get(&apps_smmu, "idr1", SMMU_IDR1);
	matches = idr0 & 0xff;
	printf("wifi: 0: the SMMU is %s; %u stream match registers, %u contexts; a stream nobody matches %s\n", scr0 & 1 ? "OFF" : "on",
	    matches, idr1 & 0xff, scr0 & 1u << 10 ? "faults" : "passes untranslated");
	for (uint32_t n = 0; n < matches && 0x800 + 4 * n < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		/* with 16-bit stream numbers switched on "valid" moves from bit 31 of the one to bit 10 of the other */
		uint32_t wide = scr0 & 1u << 3, stream = smr & (wide ? 0xffff : 0x7fff), mask = (smr >> 16) & (wide ? 0xffff : 0x7fff);

		if (!(wide ? s2cr & 1u << 10 : smr >> 31)) {
			continue;
		}
		valid++;
		if (((stream ^ RADIO_STREAM) & ~(mask | RADIO_STREAM_MASK)) != 0) {
			continue;
		}
		printf("wifi: 0: match register %u covers the radio's stream: stream %#x mask %#x (%#x), %s %u; s2cr %#x)\n", n, stream,
		    mask, smr, kinds[(s2cr >> 16) & 3], s2cr & 0xff, s2cr);
		covering++;
	}
	printf("wifi: 0: %u of the %u match registers are in use; %u cover stream %#x%s\n", valid, matches, covering, RADIO_STREAM,
	    covering ? "" : ": THE BOOT CHAIN HAS SET NOTHING UP FOR THE RADIO'S STREAM");
}

/* ---- beside the steps: the radio's view of memory (boot-arg wifi_smmu=2) ----------------------------- */

/*
 * The boot chain sets nothing up for the radio's stream, and a stream nobody matches faults
 * (smmu_look()): before the radio can be given the address of a ring, the stream needs a match
 * register, a translation context and page tables, as redwood_adreno.c makes them in the GPU's
 * own SMMU -- there are the formats and the order of programming, and what the hypervisor does
 * with the global registers. Here they are found rather than known: the first match register
 * that is not valid, and the lowest context that no valid match register sends a stream
 * through and that is not switched on (Linux, too, takes the lowest free ones, on top of what
 * the boot chain left: dt "qcom,skip-init").
 *
 * The radio's addresses are where Android's tree puts them (dt: qcom,iommu-dma-addr-pool of
 * qcom,wcn6750: 256 MB from 0xa0000000), handed out upwards and never taken back; its faults
 * are recorded and do not stop it (dt: qcom,iommu-faults "stall-disable", "HUPCF"), and nobody
 * takes their interrupt: radio_space_faults() looks.
 *
 * Everything written is put back when the DSP has been stopped, and on the way back to the
 * loader after a panic (radio_space_off()).
 *
 * What the phone said (13 October): the hypervisor grants it. The boot chain uses match
 * registers 0 to 7 and contexts 0 to 6; register 8 and context 7 (at 0x15087000) were taken,
 * everything read back as written (but two bits that the SMMU does not keep), and the radio
 * then read and wrote the memory it was shown without a fault.
 */
#define RADIO_ADDRESSES         0xa0000000ull
#define RADIO_ADDRESSES_SIZE    0x10000000u
#define RADIO_TABLES            32              /* of 4K: the root, one of the second level, and 2 MB of addresses each other */
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11) /* a page; for requests of either
	                                                                                  * privilege; shared, accessed, not
	                                                                                  * global: Linux's for a device */
#define RADIO_PTE_DEVICE        (RADIO_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)  /* ... a device's registers: Linux's
	                                                                                  * IOMMU_MMIO (MAIR entry 2), never
	                                                                                  * executed */
/*
 * Where the radio raises an interrupt by writing (its "MSI"): resource "msi_addr" of the device
 * tree's node for it, a register of the GIC that raises the SPI whose number is written; the
 * node's interrupts are SPI 768..799, so vector v is the GIC's number 800 + v.
 */
#define RADIO_MSI_REGISTER      0x17a10040ull
#define RADIO_FIRST_SPI         768u
#define PTE_ADDRESS             0x0000fffffffff000ull
#define SMMU_SGFSR              0x048           /* what the SMMU as a whole has refused ... */
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054           /* ... and of which stream */
#define SMMU_CBAR(n)            (4 * (n))       /* in the second page; [17:16] kind of context */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
/* on; faults recorded, no interrupt, and a fault does not hold up the requests after it */
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */

static struct {
	uint32_t wanted;                /* wifi_smmu */
	bool found, on;
	uint32_t page, pages;           /* of the SMMU's registers: a page's size, and how many before the contexts' */
	uint32_t match, context;        /* the stream match register and the context taken for the radio */
	uint32_t old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;             /* RADIO_TABLES of them, the root first */
	uint32_t table_count;
	uint64_t next;                  /* of the radio's addresses, the first not handed out (from RADIO_ADDRESSES) */
	uint32_t stream, stream_mask;   /* what the match register takes (radio_space_find()) */
} radio_space;
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the radio's context", 0, 0x1000 };

static uint64_t *
radio_table(uint32_t n)
{
	return (uint64_t *)(radio_space.tables + 4096 * n);
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
static uint64_t *
radio_next(uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < radio_space.table_count; i++) {
			if (kvtophys((vm_offset_t)radio_table(i)) == (*entry & PTE_ADDRESS)) {
				return radio_table(i);
			}
		}
		return NULL;
	}
	if (radio_space.table_count == RADIO_TABLES) {
		return NULL;
	}
	table = radio_table(radio_space.table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

/* Our memory at `physical` behind the radio's addresses from `address` on; the SMMU reads the tables from memory. */
static bool
radio_enter(uint64_t address, uint64_t physical, uint64_t size, uint64_t attributes)
{
	if (radio_space.tables == 0) {
		if (kmem_alloc(kernel_map, &radio_space.tables, round_page(4096 * RADIO_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		radio_space.table_count = 1;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t at = address + offset, *level2 = radio_next(&radio_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : radio_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			printf("wifi: %u: no page table left for the radio's %#llx\n", step, at);
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = radio_space.tables; line < radio_space.tables + 4096 * RADIO_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	return true;
}

/* Which match register and which context are free; nothing is written. */
static bool
radio_space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		printf("wifi: %u: this SMMU numbers its streams with 16 bits, which nothing here is written for\n", step);
		return false;
	}
	/*
	 * The Galaxy S20+'s hypervisor checks every match register written against streams it knows
	 * ("SMMU Virt SMR: Invalid SID" in hyp.img) and took nothing of 0x1c00 mask 1: there the radio's
	 * own stream alone, as Android has it (dt iommu-map: 0x1c01, mask 0; redwood_pcie.c makes the
	 * controller give the radio's requests that stream). wifi_sid=S, wifi_sid_mask=M: others.
	 */
	radio_space.stream = redwood_platform_is("y2q") ? 0x1c01 : RADIO_STREAM;
	radio_space.stream_mask = redwood_platform_is("y2q") ? 0 : RADIO_STREAM_MASK;
	PE_parse_boot_argn("wifi_sid", &radio_space.stream, sizeof(radio_space.stream));
	PE_parse_boot_argn("wifi_sid_mask", &radio_space.stream_mask, sizeof(radio_space.stream_mask));
	radio_space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	radio_space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * radio_space.pages * radio_space.page != 0x100000) {    /* dt: "reg" of apps-smmu */
		printf("wifi: %u: the SMMU says it has 2 x %u pages of %#x bytes, which is not the megabyte the device tree gives "
		    "it: its contexts are not looked for\n", step, radio_space.pages, radio_space.page);
		return false;
	}
	radio_space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));

		if (smr >> 31) {
			printf("wifi: %u: match register %u is in use: stream %#x mask %#x, s2cr %#x\n", step, n, smr & 0x7fff,
			    smr >> 16 & 0x7fff, s2cr);
			used[s2cr & 0xff] = true;
		} else if (radio_space.match == ~0u) {
			radio_space.match = n;
			radio_space.old_smr = smr;
			radio_space.old_s2cr = s2cr;
		}
	}
	for (radio_space.context = 0; radio_space.context < contexts && used[radio_space.context]; radio_space.context++) {
	}
	if (radio_space.match == ~0u || radio_space.context >= contexts) {
		printf("wifi: %u: the SMMU has no match register or no context left\n", step);
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + radio_space.page;
	apps_smmu_context.phys = apps_smmu.phys + (radio_space.pages + radio_space.context) * radio_space.page;
	printf("wifi: %u: free: match register %u (smr %#x, s2cr %#x) and context %u, whose registers are at %#x; the SMMU's "
	    "pages are of %#x bytes, %u before the contexts'\n", step, radio_space.match, radio_space.old_smr,
	    radio_space.old_s2cr, radio_space.context, apps_smmu_context.phys, radio_space.page, radio_space.pages);
	get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context));
	get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
	radio_space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0);
	get(&apps_smmu_context, "fsr", SMMU_CB_FSR);
	if (radio_space.old_sctlr & 1) {
		printf("wifi: %u: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left "
		    "alone\n", step, radio_space.context);
		return false;
	}
	radio_space.found = true;
	return true;
}

/* What the SMMU has recorded of requests of the radio's that it could not serve. */
static void
radio_space_faults(void)
{
	uint32_t status, global;

	if (!radio_space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		printf("wifi: %u: the SMMU has refused the radio nothing (context %u: fsr %#x; sgfsr 0)\n", step, radio_space.context,
		    status);
		return;
	}
	tell("wifi: %u: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: "
	    "status %#x, syndromes %#x %#x (the second names the stream)\n", step, radio_space.context, status,
	    (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 | rd(apps_smmu_context.base + SMMU_CB_FAR),
	    rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global, rd(apps_smmu.base + SMMU_SGFSYNR0),
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

/* The radio's stream through a context with our tables. */
static bool
radio_space_on(void)
{
	uint32_t smr = SMMU_SMR(radio_space.match), s2cr = SMMU_S2CR(radio_space.match), sees = 1u << 31 |
	    radio_space.stream_mask << 16 | radio_space.stream;
	uint64_t root = kvtophys(radio_space.tables) | (uint64_t)radio_space.context << 48;     /* tagged with the context's number */
	bool took;

	printf("wifi: %u: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) and is switched on, and "
	    "match register %u sends stream %#x mask %#x through it; before: smr %#x, s2cr %#x, the context's sctlr %#x\n", step,
	    radio_space.context, root & PTE_ADDRESS, radio_space.match, radio_space.stream, radio_space.stream_mask,
	    radio_space.old_smr, radio_space.old_s2cr, radio_space.old_sctlr);
	sleep_ms(pause_ms);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			printf("wifi: %u: the SMMU does not finish forgetting old translations\n", step);
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	radio_space.on = true;                  /* (from here on there is something to take back) */
	put(&apps_smmu, "s2cr", s2cr, radio_space.context);
	put(&apps_smmu, "smr", smr, sees);

	/* the global ones are the hypervisor's to grant: see what it made of them (each read back, to see which it took) */
	took = get(&apps_smmu, "smr", smr) == sees;
	took &= (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) == radio_space.context;
	took &= (get(&apps_smmu_second, "cbar", SMMU_CBAR(radio_space.context)) & 0x30000) == (SMMU_CBAR_STAGE_1 & 0x30000);
	took &= (get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) != 0;
	took &= get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) == (uint32_t)root;
	took &= get(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4) == (uint32_t)(root >> 32);
	if (!took) {
		printf("wifi: %u: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", step,
		    radio_space.stream, radio_space.context);
		return false;
	}
	get(&apps_smmu_context, "tcr", SMMU_CB_TCR);
	get(&apps_smmu_context, "tcr2", SMMU_CB_TCR2);
	get(&apps_smmu_second, "cba2r", SMMU_CBA2R(radio_space.context));
	get(&apps_smmu, "sgfsr", SMMU_SGFSR);
	printf("wifi: %u: THE RADIO'S STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", step, radio_space.context);
	radio_space_faults();
	return true;
}

/* The stream goes nowhere again, and the context translates nothing: as they were found. */
static void
radio_space_off(void)
{
	if (!radio_space.on) {
		return;
	}
	radio_space_faults();
	printf("wifi: %u: the radio's stream and context %u of the SMMU are put back as they were found\n", step, radio_space.context);
	put(&apps_smmu, "smr", SMMU_SMR(radio_space.match), radio_space.old_smr);
	put(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match), radio_space.old_s2cr);
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, radio_space.old_sctlr);
	get(&apps_smmu, "smr", SMMU_SMR(radio_space.match));
	get(&apps_smmu, "s2cr", SMMU_S2CR(radio_space.match));
	get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	radio_space.on = false;
}

/*
 * For the driver (redwood_lx.c): memory of ours that the radio is to see, at the address
 * returned. Without wifi_smmu=2 it has an address and the radio is shown nothing.
 */
static bool
radio_map(uint64_t physical, uint64_t size, uint64_t attributes, unsigned long long *address)
{
	size = (size + 4095) & ~4095ull;
	if (radio_space.next + size > RADIO_ADDRESSES_SIZE) {
		return false;
	}
	*address = RADIO_ADDRESSES + radio_space.next;
	radio_space.next += size;
	/*
	 * The Galaxy S20+'s Broadcom radio (redwood_pcie.c) is on the same kind of SMMU at the same
	 * address, with the same stream and mask (dt iommu-map of pcie@1c00000: 0x1c00 the root port,
	 * 0x1c01 the radio) and the same addresses (cnss_pci_iommu_group: 256 MB from 0xa0000000); none
	 * of the steps above runs there, so asking is wanting.
	 */
	if (whole || redwood_platform_is("y2q")) {
		radio_space.wanted = 2;
	}
	if (radio_space.wanted != 2) {
		printf("wifi: %u: THE RADIO IS NOT SHOWN the %llu KB of ours at %#llx that the driver will know as %#llx: that takes "
		    "a context of the SMMU (wifi_smmu=2)\n", step, size >> 10, physical, *address);
		return true;
	}
	lck_mtx_lock(&redwood_smmu_lock);       /* (the sound may be looking for a free context at the same time) */
	if ((!radio_space.found && !radio_space_find()) || !radio_enter(*address, physical, size, attributes)) {
		lck_mtx_unlock(&redwood_smmu_lock);
		return false;
	}
	printf("wifi: %u: the radio is to see %llu KB of ours (%#llx) at %#llx\n", step, size >> 10, physical, *address);
	if (!radio_space.on) {
		bool on = radio_space_on();

		lck_mtx_unlock(&redwood_smmu_lock);
		return on;
	}
	lck_mtx_unlock(&redwood_smmu_lock);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, radio_space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	return true;
}

bool
redwood_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	return radio_map(physical, size, RADIO_PTE, address);
}

/* What the SMMU has refused the radio so far (nothing is said before the stream has a context). */
void
redwood_wifi_radio_faults(void)
{
	radio_space_faults();
}

/*
 * What the radio is to write, and where, to raise its interrupt vector 0 (vector v: data + v): the
 * GIC's register that raises an SPI, shown to the radio in its SMMU context as a device's page.
 * Once; the GIC's number of the SPI of vector 0 goes to *first_spi.
 */
bool
redwood_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
	static unsigned long long page;

	if (page == 0) {
		tell("wifi: wifi_irq: THE RADIO'S SMMU CONTEXT GETS AN ENTRY for the GIC's register that raises an interrupt (%#llx), "
		    "as a device's page\n", RADIO_MSI_REGISTER);
		if (!radio_map(RADIO_MSI_REGISTER & ~4095ull, 4096, RADIO_PTE_DEVICE, &page)) {
			page = 0;
			return false;
		}
	}
	*address = page + (RADIO_MSI_REGISTER & 4095);
	*data = RADIO_FIRST_SPI + 32;
	*first_spi = RADIO_FIRST_SPI + 32;
	tell("wifi: wifi_irq: the radio raises its interrupt vector v by writing %u + v at %#llx\n", *data, *address);
	return true;
}

/*
 * wifi_smmu=2 in a run that does not get as far as the driver: the context is set up with empty
 * tables, so that whatever the stream asked for would be refused as it is now, and taken back.
 * This is the first write to this SMMU, and to see whether the hypervisor grants it.
 */
static void
radio_space_try(void)
{
	bool as_found;

	if (!radio_space_find() || !radio_enter(RADIO_ADDRESSES, 0, 0, RADIO_PTE)) {
		return;
	}
	as_found = radio_space_on();
	sleep_ms(pause_ms);
	radio_space_off();
	printf("wifi: %u: %s\n", step, as_found ? "THE SMMU TOOK A CONTEXT FOR THE RADIO'S STREAM, and it has been taken back" :
	    "THE SMMU DID NOT TAKE A CONTEXT FOR THE RADIO'S STREAM");
}

/* ---- for the driver (redwood_lx.c): the firmware's processor, and files ------------------------------- */

bool
redwood_wifi_dsp_is_up(void)
{
	printf("wifi: %u: the driver asks for the firmware's processor: it %s\n", step, dsp_started && glink.open ? "has been up "
	    "since step 11, with the link of step 13" : "IS NOT UP");
	return dsp_started && glink.open;
}

/*
 * ath11k asks for files by the names Linux's firmware tree has ("ath11k/WCN6750/hw1.0/board.bin"), and so
 * does brcmfmac on the Galaxy S20+ ("brcm/brcmfmac4375b1-pcie.bin");
 * this phone has what Android's driver loads (fw-wifi/, see userland/build.sh): its board's own
 * data file and regulatory database, which are what the two names below stand for. The others
 * it asks for first and can do without (a container of several boards' data, a table of the
 * firmware's features) are not there.
 */
const uint8_t *
redwood_wifi_driver_file(const char *name, uint64_t *size)
{
	static const char *const files[][2] = { { "board.bin", "bd_m20gl.elf" }, { "regdb.bin", "regdb_xiaomi.bin" },
		/* the Galaxy S20+'s BCM4375 (B1), for brcmfmac: Android's files (userland/build.sh) */
		{ "brcmfmac4375b1-pcie.bin", "bcm4375_sta.bin" }, { "brcmfmac4375b1-pcie.txt", "bcm4375_nvram.txt" },
		{ "brcmfmac4375b1-pcie.clm_blob", "bcm4375_clm.blob" } };
	const char *last = strrchr(name, '/');

	last = last == NULL ? name : last + 1;
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (strcmp(last, files[i][0]) == 0) {
			return image_file(files[i][1], size);
		}
	}
	return NULL;
}

/*
 * The radio's own address. Its firmware has none (it says zeros): Android keeps the phone's in
 * its storage and hands it to the radio's driver (dt: "use-nv-mac"). Here it is given to the
 * kernel when that is started -- boot-arg wifi_mac, six bytes in hexadecimal with colons
 * between -- and is nowhere in these sources. The log says that one was given and how long it
 * was, not which.
 */
bool
redwood_wifi_radio_address(uint8_t *address)
{
	char text[32] = "";
	uint8_t bytes[6];
	unsigned count = 0, digits = 0, value = 0;
	bool good;

	if (!PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) || text[0] == 0) {
		printf("wifi: %u: the driver asks for the radio's address: none was given (wifi_mac)\n", step);
		return false;
	}
	good = true;
	for (const char *c = text;; c++) {
		int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ?
		    *c - 'A' + 10 : -1;

		if (digit >= 0 && digits < 2) {
			value = value << 4 | (unsigned)digit;
			digits++;
		} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
			bytes[count++] = (uint8_t)value;
			digits = value = 0;
		} else {
			good = false;
		}
		if (*c == 0 || !good) {
			break;
		}
	}
	good = good && count == 6 && !(bytes[0] & 1) && (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) != 0;
	printf("wifi: %u: the driver asks for the radio's address: one was given, %lu characters, %s\n", step,
	    (unsigned long)strlen(text), good ? "six bytes: it is the radio's for this run" : "WHICH IS NOT A STATION'S ADDRESS: "
	    "not used");
	if (good) {
		memcpy(address, bytes, sizeof(bytes));
	}
	return good;
}

/*
 * The network the radio is to join: the file "network" among the firmware in the small image,
 * the name on its first line, the passphrase on its second (userland/build.sh takes it from
 * fw-wifi/network, which is in no repository). Not a boot argument: those are echoed by the
 * loader and printed with a panic. The log says how long the two are, and nothing else of them.
 */
bool
redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	uint64_t size = 0;
	const uint8_t *file = image_file("network", &size), *end = file + size, *line = file;
	unsigned lengths[2] = { 0, 0 }, sizes[2] = { *name_length, *passphrase_length };
	uint8_t *where[2] = { name, passphrase };

	if (file == NULL) {
		printf("wifi: %u: which network to join: none was given (fw/network of the small image)\n", step);
		return false;
	}
	for (unsigned n = 0; n < 2 && line < end; n++) {
		const uint8_t *stop = line;

		while (stop < end && *stop != '\n') {
			stop++;
		}
		lengths[n] = (unsigned)(stop - line) - (stop > line && stop[-1] == '\r' ? 1 : 0);
		if (lengths[n] > sizes[n]) {
			lengths[n] = 0;
		} else {
			memcpy(where[n], line, lengths[n]);
		}
		line = stop + 1;
	}
	printf("wifi: %u: which network to join: a name of %u bytes and a passphrase of %u bytes were given%s\n", step,
	    lengths[0], lengths[1], lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63 ? ": NOT A NAME OF 1 TO 32 BYTES AND A "
	    "PASSPHRASE OF 8 TO 63" : "");
	if (lengths[0] == 0 || lengths[1] < 8 || lengths[1] > 63) {
		return false;
	}
	*name_length = lengths[0];
	*passphrase_length = lengths[1];
	return true;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_smem, step_mailbox, step_rpmh, step_rails, step_pins, step_firmware, step_mailbox_open, step_header, step_image,
	step_our_item, step_start, step_queues, step_link, step_qrtr, step_exchange, step_registers,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/*
 * Where a run ends in order while the DSP runs (the run's time is up, the host asks for its
 * end), and when the steps are over: the DSP is stopped (dsp_shutdown()).
 */
void
redwood_wifi_stop(void)
{
	/* (a radio that is still on its way up is let get there first, or fail: half a minute at the most) */
	for (unsigned ms = 0; whole_runs && ms < 40000; ms += 10) {
		sleep_ms(10);
	}
	redwood_apple80211_summary();
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && !dirty) {
		bool down = dsp_shutdown("the run is about to end");

		if (whole) {
			tell("wifi: the radio is off: its DSP %s, and TrustZone %s\n", stopped_by_its_word ? "stopped when asked" :
			    "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
		}
	}
	if (!dsp_started) {
		redwood_pcie_radio_down();      /* (the Galaxy S20+'s radio has no DSP: powered off, then its stream put back) */
		radio_space_off();
	}
	lck_mtx_unlock(&dsp_lock);
}

/*
 * The same when the kernel goes back to the loader without having been through the above: after
 * a panic. A DSP left running then is one that nobody listens to any more, and the next kernel
 * has to find and stop it; better it is stopped now. One processor is left, with its interrupts
 * off: nothing here takes a lock or sleeps (the waits are loops), and the link's thread is
 * simply never run again. The DSP is asked first, for two seconds at most; whatever it says,
 * TrustZone is asked to shut it down.
 */
void
redwood_wifi_quiesce(void)
{
	redwood_apple80211_summary();           /* (also when the run is ended from the phone: five fingers) */
	if (!dsp_started) {
		redwood_pcie_radio_down();      /* (the Galaxy S20+'s radio: no DSP; powered off, so that the next run finds it cold) */
	}
	if (!dsp_started && radio_space.on) {
		leaving = true;                 /* (... and its stream put back) */
		new_from = ~0u;
		radio_space_off();
		return;
	}
	if (!dsp_started || dirty) {
		return;
	}
	leaving = true;
	new_from = ~0u;                         /* (no pauses) */
	dsp_shutdown("the kernel is on its way back to the loader");
}

#define REDWOOD_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin (its "redwood") */

extern vm_offset_t pe_arm_get_soc_base_phys(void);

/* The steps' end: the DSP stopped, or left as asked. */
static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && dirty) {
		printf("wifi: THE DSP IS LEFT RUNNING (wifi_dirty), as a kernel that died would leave it\n");
	} else if (dsp_started && done == last_step && redwood_wifi_option("wifi_keep")) {
		printf("wifi: THE DSP STAYS ON while the system runs (wifi_keep)%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	if (!dsp_started) {
		radio_space_off();              /* (if the DSP could not be shut down in order: all the same) */
	}
	lck_mtx_unlock(&dsp_lock);
}

/*
 * The network of the file, joined the way anybody joins one: by asking the station. iOS's Wi-Fi
 * side may ask for another later, or for none.
 */
static void
join_given(uint64_t began)
{
	static struct redwood_sta_join what;
	struct redwood_sta_state *state;
	unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
	uint64_t size = 0;
	uint32_t given = 1;
	int error;

	if (image_file("network", &size) == NULL) {
		tell("wifi: no network to join was given (fw-wifi/network, see the README): the radio waits to be asked\n");
		return;
	}
	if (PE_parse_boot_argn("wifi_file", &given, sizeof(given)) && given == 0) {
		tell("wifi: the network of the file is not joined at the start (wifi_file=0): the radio waits to be asked\n");
		return;
	}
	bzero(&what, sizeof(what));
	if (!redwood_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length)) {
		tell("wifi: the network given in fw/network is not a name and a passphrase: nothing is joined\n");
		return;
	}
	what.ssid_length = (uint8_t)name_length;
	what.passphrase_length = (uint8_t)passphrase_length;
	what.security = REDWOOD_STA_WPA2_PSK;
	error = redwood_sta_ask_join_standing(&what);
	bzero(&what, sizeof(what));
	for (unsigned waited = 0; error == 0 && redwood_sta_busy() && waited < 300; waited++) {
		sleep_ms(100);
	}
	if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
		return;
	}
	redwood_sta_state(state);
	if (state->phase == REDWOOD_STA_JOINED) {
		tell("wifi: THE NETWORK OF THE FILE IS JOINED, %llu.%llu s after the kernel began on the radio\n", (now_us() - began) /
		    1000000, (now_us() - began) / 100000 % 10);
	} else {
		tell("wifi: THE NETWORK OF THE FILE IS NOT JOINED (%d, %d): the radio waits to be asked. The last of what was said:\n",
		    error, state->last_error);
		say_kept();
	}
	kfree_data(state, sizeof(*state));
}

/*
 * wifi=1: the whole of it on a thread of its own, so that the system starts meanwhile (the
 * firmware's calibration alone takes seven seconds). The steps that only look are left out.
 */
static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const uint8_t needed[] = { 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint64_t began = now_us();
	uint32_t done = 0, failed = 0;

	/* the memory the radio is shown is one piece: taken now, while that is easy to find */
	if (redwood_lx_prepare() != 0) {
		tell("wifi: NO MEMORY FOR THE RADIO in one piece: it is not brought up\n");
		whole_runs = false;
		thread_terminate(current_thread());
	}
	for (unsigned i = 0; i < sizeof(needed) && failed == 0; i++) {
		step = needed[i];
		if (step == 15) {
			tell("wifi: the radio's DSP is up and linked, %llu ms in; now the driver\n", (now_us() - began) / 1000);
		}
		if (steps[step - 1]()) {
			done = step;
		} else {
			failed = step;
		}
	}
	last_step = needed[sizeof(needed) - 1];
	if (failed == 0) {
		tell("wifi: THE RADIO IS UP, %llu.%llu s after the kernel began on it\n", (now_us() - began) / 1000000,
		    (now_us() - began) / 100000 % 10);
		join_given(began);
	} else {
		tell("wifi: THE RADIO DID NOT COME UP: step %u failed, %llu s in. The last of what was said on the way:\n", failed,
		    (now_us() - began) / 1000000);
		say_kept();
	}
	steps_over(failed == 0 ? last_step : 0);
	step = done;
	whole_runs = false;
	thread_terminate(current_thread());
}

/* With the driver up: one scan, said in the log, then the network of the file (join_given()). */
static void
brcm_scan_and_join(uint64_t began)
{
	struct redwood_sta_scan scan;
	struct redwood_sta_bss *bss;
	unsigned count = 0;

	bzero(&scan, sizeof(scan));
	if (redwood_sta_ask_scan(&scan) != 0) {
		tell("wifi: the station takes no scan\n");
		return;
	}
	for (unsigned waited = 0; redwood_sta_busy() && waited < 200; waited++) {
		sleep_ms(100);
	}
	if ((bss = kalloc_data(sizeof(*bss), Z_WAITOK)) != NULL) {
		while (redwood_sta_heard(count, bss)) {
			count++;
		}
		kfree_data(bss, sizeof(*bss));
	}
	tell("wifi: THE SCAN HEARD %u ACCESS POINTS, %llu.%llu s after the kernel began on the radio\n", count,
	    (now_us() - began) / 1000000, (now_us() - began) / 100000 % 10);
	join_given(began);
}

/*
 * The Galaxy S20+ with wifi=1: its Broadcom radio on PCIe (redwood_pcie.c) and Linux's brcmfmac on it
 * (redwood_lx.c), on a thread of their own like whole_thread() above. None of this file's steps: the
 * radio has no DSP, and the driver does from the link up.
 */
static void
brcm_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	int error;

	if (redwood_lx_prepare() != 0) {
		tell("wifi: NO MEMORY FOR THE RADIO in one piece: it is not brought up\n");
	} else if ((error = redwood_lx_start()) != 0) {
		tell("wifi: THE RADIO DID NOT COME UP (%d), %llu s in\n", error, (now_us() - began) / 1000000);
	} else {
		tell("wifi: THE RADIO IS UP, %llu.%llu s after the kernel began on it\n", (now_us() - began) / 1000000,
		    (now_us() - began) / 100000 % 10);
		brcm_scan_and_join(began);
	}
	whole_runs = false;
	thread_terminate(current_thread());
}

static void
brcm_init(void)
{
	uint32_t on = 0, steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi", &on, sizeof(on)) || on != 1) {
		return;
	}
	if (PE_parse_boot_argn("pcie_step", &steps, sizeof(steps)) && steps != 0) {
		printf("wifi: wifi=1 with pcie_step=%u: the steps are done, not the driver\n", steps);
		return;
	}
	whole = whole_runs = true;
	redwood_lx_net_early();
	if (kernel_thread_start(brcm_thread, NULL, &thread) != KERN_SUCCESS) {
		whole_runs = false;
		tell("wifi: no thread: the radio is not brought up\n");
		return;
	}
	thread_deallocate(thread);
	tell("wifi: the radio (BCM4375, brcmfmac) is being brought up beside the system's start (wifi=1)\n");
	/* wifi_wait=1: the system's start waits for it (a minute at most), as with pcie_step=N -- for runs without an iOS root */
	if (PE_parse_boot_argn("wifi_wait", &on, sizeof(on)) && on) {
		for (unsigned ms = 0; whole_runs && ms < 60000; ms += 10) {
			sleep_ms(10);
		}
		tell("wifi: the system's start goes on (wifi_wait)\n");
	}
}

/*
 * Called while the system starts, before there is a root file system. With wifi_step=N the start
 * waits until the steps are over (see redwood_adreno_init()); with wifi=1 it does not.
 */
/*
 * r5q (Galaxy S10 Lite): the survey, step 1 of the Wi-Fi port (work/ios-r5q/wifi/WIFI-PLAN.md). The partitions of the
 * shared memory, the one between us and the modem (processors 0 and 1, live DT qcom,smp2p-mpss), and the modem's items:
 * SMP2P 435 (ours to it) and 428 (its to us: there only if the modem has run since the phone was started), its crash
 * reason 421. Nothing is written.
 */
static void
r5q_wifi_survey(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	uint32_t version = 0, size = 0, at;
	static const uint32_t items[] = { 435, 428, 421, 137, 602, 603 };

	smem_phys = 0x86000000UL;
	smem_remote = 1;
	step = 1;
	printf("wifi: r5q survey (reads only): the shared memory area at %#lx, 2 MB\n", SMEM_PHYS);
	if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
		printf("wifi: r5q survey: the area could not be read\n");
		return;
	}
	printf("wifi: r5q survey: version word %#x; table %02x %02x %02x %02x version %u, %u entries\n", version, table.magic[0],
	    table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
	if (memcmp(table.magic, "$TOC", 4) != 0 || table.count > 64) {
		return;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			printf("wifi: r5q survey: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", i,
			    entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	show_partition("us and the modem (0 and 1)", SMEM_US, 1);
	show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);
	for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
		uint8_t head[16];

		size = 0;
		at = smem_item(items[i], &size);
		bzero(head, sizeof(head));
		if (at != 0) {
			smem_peek(at, head, size < sizeof(head) ? size : sizeof(head));
		}
		printf("wifi: r5q survey: item %u: %s, %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", items[i],
		    at ? "there" : "not there", size, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7], head[8],
		    head[9], head[10], head[11]);
	}
}

/*
 * r5q (Test 88): the modem's remote file system (Linux userspace rmtfs, qmi_rmtfs.qmi), QMI service 14 version 1
 * instance 0, served from here. The modem keeps its EFS in four partitions it cannot reach itself; it opens them by
 * name, has us copy sectors between them and the 2 MB the loader gave it (rmtfs_pa=, TrustZone-assigned to the
 * modem), and asks for that buffer's address. THE PARTITIONS ARE ONLY READ: each is read whole into memory at its
 * first open, and the modem's writes go to that copy alone (they are counted, and lost at the end of the run).
 */
#define SERVICE_RMTFS           14
#define RMTFS_SECTOR            512
extern int redwood_ufs_read_named(const char *name, uint64_t offset, void *dst, uint64_t len, uint64_t *size);

static struct {
	const char *path, *part;
	uint8_t *data;
	uint64_t size;
	uint32_t reads, writes;
} rmtfs_files[] = {
	{ "/boot/modem_fs1", "modemst1", NULL, 0, 0, 0 }, { "/boot/modem_fs2", "modemst2", NULL, 0, 0, 0 },
	{ "/boot/modem_fsg", "fsg", NULL, 0, 0, 0 }, { "/boot/modem_fsc", "fsc", NULL, 0, 0, 0 },
};
static struct redwood_qrtr_port *rmtfs_port;
static uint64_t rmtfs_pa;
static volatile uint8_t *rmtfs_mem;
static uint32_t rmtfs_requests;

/* the value of TLV `type` in a QMI message's body; NULL if absent */
static const uint8_t *
qmi_tlv(const uint8_t *body, size_t length, uint8_t type, uint16_t *tlv_length)
{
	for (size_t at = 0; at + 3 <= length;) {
		uint16_t n = (uint16_t)(body[at + 1] | body[at + 2] << 8);

		if (at + 3 + n > length) {
			return NULL;
		}
		if (body[at] == type) {
			*tlv_length = n;
			return body + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}

static size_t
qmi_put(uint8_t *out, size_t at, uint8_t type, const void *value, uint16_t length)
{
	out[at] = type;
	out[at + 1] = (uint8_t)length;
	out[at + 2] = (uint8_t)(length >> 8);
	memcpy(out + at + 3, value, length);
	return at + 3 + length;
}

static void
rmtfs_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint8_t out[64];
	uint16_t txn, msg, n = 0;
	uint16_t result[2] = { 0, 0 };          /* QMI_RESULT_SUCCESS; on failure {1, error} */
	size_t at = 7;
	uint32_t caller = 0, file = ~0u;
	char path[64];

	if (length < 7 || m[0] != 0) {
		return;                         /* (requests only) */
	}
	txn = (uint16_t)(m[1] | m[2] << 8);
	msg = (uint16_t)(m[3] | m[4] << 8);
	m += 7;
	length -= 7;
	rmtfs_requests++;
	if ((v = qmi_tlv(m, length, 0x01, &n)) != NULL && n == 4 && msg != 1) {
		memcpy(&caller, v, 4);
		file = caller - 1;
	}
	switch (msg) {
	case 1:                                 /* open: TLV 1 the path; answer TLV 0x10 the caller's id */
		bzero(path, sizeof(path));
		if ((v = qmi_tlv(m, length, 0x01, &n)) != NULL) {
			memcpy(path, v, n < sizeof(path) - 1 ? n : sizeof(path) - 1);
		}
		for (uint32_t i = 0; i < sizeof(rmtfs_files) / sizeof(rmtfs_files[0]); i++) {
			if (strcmp(path, rmtfs_files[i].path) == 0) {
				file = i;
			}
		}
		if (file != ~0u && rmtfs_files[file].data == NULL) {
			uint64_t size = 0;
			int error = redwood_ufs_read_named(rmtfs_files[file].part, 0, NULL, 0, &size);

			if (error == 0 && size > 0 && size <= 16 * 1024 * 1024 &&
			    (rmtfs_files[file].data = kalloc_data(size, Z_WAITOK | Z_ZERO)) != NULL) {
				error = redwood_ufs_read_named(rmtfs_files[file].part, 0, rmtfs_files[file].data, size, NULL);
				rmtfs_files[file].size = error == 0 ? size : 0;
			}
			printf("wifi: rmtfs: %s is %s: %llu bytes read from the phone (%d); the modem's writes stay in memory\n", path,
			    rmtfs_files[file].part, rmtfs_files[file].size, error);
		}
		if (file == ~0u || rmtfs_files[file].size == 0) {
			printf("wifi: rmtfs: the modem opens \"%s\": NOT SERVED\n", path);
			result[0] = 1;
			result[1] = 0x52;               /* QMI_ERR_ENOENT-ish */
		} else {
			caller = file + 1;
			printf("wifi: rmtfs: the modem opens \"%s\" (%s): caller %u\n", path, rmtfs_files[file].part, caller);
		}
		break;
	case 2:                                 /* close */
		printf("wifi: rmtfs: the modem closes caller %u\n", caller);
		break;
	case 3: {                               /* rw_iovec: TLV 2 direction (1 = write), TLV 3 entries {sector, offset, count} */
		const uint8_t *dir = qmi_tlv(m, length, 0x02, &n), *iov = qmi_tlv(m, length, 0x03, &n);
		uint32_t entries = iov != NULL ? iov[0] : 0, bytes = 0;
		bool write = dir != NULL && dir[0] == 1, ok = file < sizeof(rmtfs_files) / sizeof(rmtfs_files[0]) &&
		    rmtfs_files[file].size != 0 && rmtfs_mem != NULL && iov != NULL && 1 + 12 * entries <= n;

		for (uint32_t e = 0; ok && e < entries; e++) {
			uint32_t w[3];
			uint64_t from, count;

			memcpy(w, iov + 1 + 12 * e, 12);
			from = (uint64_t)w[0] * RMTFS_SECTOR;
			count = (uint64_t)w[2] * RMTFS_SECTOR;
			/* "phys_offset" is the physical address in the buffer (Linux rmtfs: rmtfs_mem_ptr subtracts the base) */
			if (w[1] >= rmtfs_pa && w[1] < rmtfs_pa + 0x200000) {
				w[1] -= (uint32_t)rmtfs_pa;
			}
			if (e == 0 && rmtfs_requests <= 12) {
				printf("wifi: rmtfs:   piece: sector %u, buffer +%#x, %u sectors\n", w[0], w[1], w[2]);
			}
			if (from + count > rmtfs_files[file].size || (uint64_t)w[1] + count > 0x200000) {
				ok = false;
				break;
			}
			for (uint64_t i = 0; i < count; i++) {
				if (write) {
					rmtfs_files[file].data[from + i] = rmtfs_mem[w[1] + i];
				} else {
					rmtfs_mem[w[1] + i] = rmtfs_files[file].data[from + i];
				}
			}
			bytes += (uint32_t)count;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		if (ok) {
			write ? rmtfs_files[file].writes++ : rmtfs_files[file].reads++;
		}
		if (!ok || rmtfs_files[file].reads + rmtfs_files[file].writes <= 6) {
			printf("wifi: rmtfs: %s of %s: %u pieces, %u bytes: %s\n", write ? "write (kept in memory)" : "read",
			    file < 4 ? rmtfs_files[file].part : "?", entries, bytes, ok ? "done" : "REFUSED");
		}
		if (!ok) {
			result[0] = 1;
			result[1] = 0x03;               /* QMI_ERR_INTERNAL */
		}
		break;
	}
	case 4: {                               /* alloc_buff: TLV 2 the size; answer TLV 0x10 the address (64 bits) */
		uint32_t want = 0;

		if ((v = qmi_tlv(m, length, 0x02, &n)) != NULL && n == 4) {
			memcpy(&want, v, 4);
		}
		printf("wifi: rmtfs: the modem asks for its buffer (%u bytes): %s %#llx\n", want, rmtfs_pa ? "given" : "NONE", rmtfs_pa);
		if (rmtfs_pa == 0 || want > 0x200000) {
			result[0] = 1;
			result[1] = 0x02;               /* QMI_ERR_NO_MEMORY */
		}
		break;
	}
	case 5:                                 /* get_dev_error: answer TLV 0x10 status 0 */
		break;
	default:
		printf("wifi: rmtfs: request %u not known: refused\n", msg);
		result[0] = 1;
		result[1] = 0x1e;                       /* QMI_ERR_NOT_SUPPORTED */
		break;
	}
	/* the answer: type 2, the same transaction and message, then the result and what this request returns */
	out[0] = 2;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	at = qmi_put(out, at, 0x02, result, 4);
	if (result[0] == 0 && msg == 1) {
		at = qmi_put(out, at, 0x10, &caller, 4);
	} else if (result[0] == 0 && msg == 4) {
		at = qmi_put(out, at, 0x10, &rmtfs_pa, 8);
	} else if (result[0] == 0 && msg == 5) {
		uint8_t status = 0;

		at = qmi_put(out, at, 0x10, &status, 1);
	}
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(rmtfs_port, node, port, out, at);
}

/* the server opened and announced to the modem's name service, as Linux's ns announces a new server */
static void
rmtfs_start(void)
{
	struct qrtr_control announce_it = { .command = QRTR_NEW_SERVER };
	uint32_t pa = 0;
	int error;

	if (!PE_parse_boot_argn("rmtfs_pa", &pa, sizeof(pa)) || pa == 0) {
		printf("wifi: rmtfs: the loader gave no memory for it (rmtfs_pa=): not served\n");
		return;
	}
	rmtfs_pa = pa;
	rmtfs_mem = (volatile uint8_t *)ml_io_map(rmtfs_pa, 0x200000);
	rmtfs_port = qrtr_open(rmtfs_receive, NULL, NULL);
	if (rmtfs_port == NULL) {
		return;
	}
	announce_it.words[0] = SERVICE_RMTFS;
	announce_it.words[1] = 1;               /* version 1, instance 0 */
	announce_it.words[2] = QRTR_US;
	announce_it.words[3] = rmtfs_port->number;
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	printf("wifi: rmtfs: SERVED at node %u port %u, its memory %#llx (2 MB); announced to the modem: %s\n", QRTR_US,
	    rmtfs_port->number, rmtfs_pa, error == 0 ? "yes" : "NOT SENT");
}

/*
 * r5q (Test 89): files for the modem, as Linux's tqftpserv serves them: TFTP over QRTR, service 4096 version 1. The
 * modem's Wi-Fi domain loads its firmware this way (image/wlanmdsp.mbn, the board files bdwlan.*), from the phone's
 * "apnhlos" partition (FAT16, 4 KiB sectors), read here. Reads only: a write request is refused. Each read gets a port
 * of its own (TFTP's transfer id), an OACK for the options asked (blksize, tsize, rsize, seek, wsize, timeoutms), then
 * windows of DATA blocks, each window sent on the client's ACK of the last.
 */
#define SERVICE_TFTP            4096
struct fatfs {
	uint32_t bs, spc, root_start, root_sectors, data_start, clusters;
	uint16_t *fat;
	bool ok;
};
static struct fatfs apnhlos, modemfs;     /* (Android mounts both under /readonly/firmware/image) */
static struct redwood_qrtr_port *tftp_port;

static const char *
r5q_strstr(const char *s, const char *w)
{
	size_t n = strlen(w);

	for (; *s; s++) {
		if (strncmp(s, w, n) == 0) {
			return s;
		}
	}
	return NULL;
}

static const char *
r5q_strchr(const char *s, char c)
{
	for (; *s; s++) {
		if (*s == c) {
			return s;
		}
	}
	return NULL;
}

static bool
r5q_has_nul(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] == 0) {
			return true;
		}
	}
	return false;
}

static bool
fat_mount(struct fatfs *f, const char *part)
{
	uint8_t b[512];
	uint32_t res, nfats, root, spf, total;

	if (redwood_ufs_read_named(part, 0, b, sizeof(b), NULL) != 0 || b[510] != 0x55 || b[511] != 0xaa) {
		return false;
	}
	f->bs = b[11] | b[12] << 8;
	f->spc = b[13];
	res = b[14] | b[15] << 8;
	nfats = b[16];
	root = b[17] | b[18] << 8;
	spf = b[22] | b[23] << 8;
	total = (b[19] | b[20] << 8) ? (uint32_t)(b[19] | b[20] << 8) : (uint32_t)(b[32] | b[33] << 8 | b[34] << 16 | (uint32_t)b[35] << 24);
	if (f->bs == 0 || f->spc == 0 || spf == 0 || nfats == 0) {
		return false;
	}
	f->root_start = res + nfats * spf;
	f->root_sectors = (root * 32 + f->bs - 1) / f->bs;
	f->data_start = f->root_start + f->root_sectors;
	f->clusters = (total - f->data_start) / f->spc + 2;
	f->fat = kalloc_data((size_t)spf * f->bs, Z_WAITOK);
	if (f->fat == NULL || f->clusters >= 65525 ||
	    redwood_ufs_read_named(part, (uint64_t)res * f->bs, f->fat, (uint64_t)spf * f->bs, NULL) != 0) {
		return false;
	}
	f->ok = true;
	return true;
}

static void
fat_name83(const char *name, size_t len, char out[11])
{
	size_t dot = len;

	memset(out, ' ', 11);
	for (size_t i = 0; i < len; i++) {
		if (name[i] == '.') {
			dot = i;
		}
	}
	for (size_t i = 0; i < dot && i < 8; i++) {
		out[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
	}
	for (size_t i = dot + 1, k = 8; i < len && k < 11; i++, k++) {
		out[k] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
	}
}

/*
 * an entry in a directory (cluster 0: the root) by its name -- the long (VFAT) name if it has one, else the 8.3 one,
 * either without regard to case: apnhlos was made with mangled short names ("image" is PCAFUI~2) --; its first
 * cluster and size; false if not there
 */
static bool
fat_lookup(struct fatfs *f, const char *part, uint32_t dir, const char *want, size_t want_len, uint32_t *cluster,
    uint32_t *size, bool *is_dir)
{
	static uint8_t sector[4096];
	static const uint8_t lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	char lfn[256], n83[11];
	bool have_lfn = false;

	fat_name83(want, want_len, n83);
	bzero(lfn, sizeof(lfn));
	for (uint32_t c = dir;;) {
		uint32_t count = dir == 0 ? f->root_sectors : f->spc;

		for (uint32_t s = 0; s < count; s++) {
			uint64_t lba = dir == 0 ? f->root_start + s : f->data_start + (uint64_t)(c - 2) * f->spc + s;

			if (f->bs > sizeof(sector) || redwood_ufs_read_named(part, lba * f->bs, sector, f->bs, NULL) != 0) {
				return false;
			}
			for (uint32_t e = 0; e < f->bs; e += 32) {
				const uint8_t *x = sector + e;
				bool match;

				if (x[0] == 0) {
					return false;
				}
				if (x[0] == 0xe5) {
					have_lfn = false;
					continue;
				}
				if ((x[11] & 0x3f) == 0x0f) {   /* a piece of a long name: 13 characters, the pieces last first */
					uint32_t k = x[0] & 0x1f;

					if (x[0] & 0x40) {
						bzero(lfn, sizeof(lfn));
						have_lfn = true;
					}
					for (uint32_t i = 0; k >= 1 && k <= 19 && i < 13; i++) {
						uint16_t ch = (uint16_t)(x[lfn_at[i]] | x[lfn_at[i] + 1] << 8);

						lfn[(k - 1) * 13 + i] = (ch == 0 || ch == 0xffff) ? 0 : (ch < 0x80 ? (char)ch : '?');
					}
					continue;
				}
				match = memcmp(x, n83, 11) == 0;
				if (have_lfn && !match && strlen(lfn) == want_len) {
					match = true;
					for (size_t i = 0; i < want_len && match; i++) {
						char a = lfn[i], b = want[i];

						a = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
						b = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
						match = a == b;
					}
				}
				have_lfn = false;
				if (match && !(x[11] & 0x08)) {
					*cluster = x[26] | x[27] << 8;
					*size = x[28] | x[29] << 8 | x[30] << 16 | (uint32_t)x[31] << 24;
					*is_dir = (x[11] & 0x10) != 0;
					return true;
				}
			}
		}
		if (dir == 0 || c < 2 || c >= f->clusters || (c = f->fat[c]) < 2 || c >= 0xfff8) {
			return false;
		}
	}
}

/*
 * (Test 90b) files read for the modem stay in memory: its Wi-Fi domain's loader reads wlanmdsp.mbn whole once per
 * segment (72 times), and reading 3.8 MB from the flash each time outlasted its grace timer.
 */
static struct {
	char name[96];
	uint8_t *data;
	uint32_t size;
} tftp_cache[12];

static uint8_t *fat_load_uncached(struct fatfs *f, const char *part, const char *path, uint32_t *size_out);

static uint8_t *
fat_load(struct fatfs *f, const char *part, const char *path, uint32_t *size_out)
{
	const char *base = path;
	uint8_t *data;
	uint32_t h = 2166136261u;

	for (const char *c = path; *c; c++) {
		if (*c == '/') {
			base = c + 1;
		}
	}
	for (unsigned i = 0; i < sizeof(tftp_cache) / sizeof(tftp_cache[0]); i++) {
		if (tftp_cache[i].data != NULL && strcmp(tftp_cache[i].name, base) == 0) {
			*size_out = tftp_cache[i].size;
			return tftp_cache[i].data;
		}
	}
	data = fat_load_uncached(f, part, path, size_out);
	if (data == NULL) {
		return NULL;
	}
	for (uint32_t i = 0; i < *size_out; i++) {
		h = (h ^ data[i]) * 16777619u;
	}
	printf("wifi: tftp: %s read from %s: %u bytes, FNV-1a %08x\n", base, part, *size_out, h);
	for (unsigned i = 0; i < sizeof(tftp_cache) / sizeof(tftp_cache[0]); i++) {
		if (tftp_cache[i].data == NULL) {
			strlcpy(tftp_cache[i].name, base, sizeof(tftp_cache[i].name));
			tftp_cache[i].data = data;
			tftp_cache[i].size = *size_out;
			break;
		}
	}
	return data;
}

/*
 * r5q (Test 108): Android's view of the modem's read-only files (stock vendor partition, /vendor/rfs/msm/mpss/readonly:
 * "vendor/firmware" -> /vendor/firmware, "firmware" -> /vendor/firmware-modem, and NO "vendor/firmware_mnt"). The
 * modem asks for /readonly/vendor/firmware_mnt/image/wlanmdsp.mbn first; Android answers "not found" and it then asks
 * for /readonly/vendor/firmware/wlanmdsp.mbn -- Samsung's newer build in the vendor partition (4251884 bytes, not the
 * NON-HLOS one of 3853508 that this served until now). The vendor files the modem needs are kept in a small store in
 * USERDATA at 4 GiB + 64 MiB (written from the PC, oem writepart): a 4 KiB header "R5Q-FILES/1", a count at +16, then
 * entries of 80 bytes from +32 (the path under /vendor/firmware, 64 bytes; offset from the store, 8; size, 4; FNV-1a, 4).
 */
#define R5Q_FILES_AT            ((4ULL << 30) + (64ULL << 20))

static uint8_t *
store_load(const char *rel, uint32_t *size_out)
{
	static uint8_t header[4096];
	static bool have;
	char key[96];
	uint8_t *data;
	uint32_t count;

	snprintf(key, sizeof(key), "vendor/%s", rel);
	for (unsigned i = 0; i < sizeof(tftp_cache) / sizeof(tftp_cache[0]); i++) {
		if (tftp_cache[i].data != NULL && strcmp(tftp_cache[i].name, key) == 0) {
			*size_out = tftp_cache[i].size;
			return tftp_cache[i].data;
		}
	}
	if (!have) {
		if (redwood_ufs_read_named("darwin", R5Q_FILES_AT, header, sizeof(header), NULL) != 0 ||
		    memcmp(header, "R5Q-FILES/1", 11) != 0) {
			printf("wifi: tftp: no vendor file store in USERDATA at 4 GiB + 64 MiB\n");
			return NULL;
		}
		have = true;
	}
	memcpy(&count, header + 16, 4);
	for (uint32_t i = 0; i < count && 32 + 80 * (i + 1) <= sizeof(header); i++) {
		const uint8_t *e = header + 32 + 80 * i;
		uint64_t off;
		uint32_t size, want, h = 2166136261u;

		if (strncmp((const char *)e, rel, 64) != 0) {
			continue;
		}
		memcpy(&off, e + 64, 8);
		memcpy(&size, e + 72, 4);
		memcpy(&want, e + 76, 4);
		if (size == 0 || size > (32u << 20) || (data = kalloc_data((size + 4095) & ~4095u, Z_WAITOK)) == NULL) {
			return NULL;
		}
		if (redwood_ufs_read_named("darwin", R5Q_FILES_AT + off, data, (size + 4095) & ~4095u, NULL) != 0) {
			kfree_data(data, (size + 4095) & ~4095u);
			return NULL;
		}
		for (uint32_t k = 0; k < size; k++) {
			h = (h ^ data[k]) * 16777619u;
		}
		printf("wifi: tftp: vendor/firmware/%s from the store: %u bytes, FNV-1a %08x (%s)\n", rel, size, h,
		    h == want ? "as written" : "DIFFERENT FROM WHAT WAS WRITTEN");
		if (h != want) {
			kfree_data(data, (size + 4095) & ~4095u);
			return NULL;
		}
		for (unsigned c = 0; c < sizeof(tftp_cache) / sizeof(tftp_cache[0]); c++) {
			if (tftp_cache[c].data == NULL) {
				strlcpy(tftp_cache[c].name, key, sizeof(tftp_cache[c].name));
				tftp_cache[c].data = data;
				tftp_cache[c].size = size;
				break;
			}
		}
		*size_out = size;
		return data;
	}
	return NULL;
}

static bool
tftp_cached(const uint8_t *data)
{
	for (unsigned i = 0; i < sizeof(tftp_cache) / sizeof(tftp_cache[0]); i++) {
		if (tftp_cache[i].data == data) {
			return true;
		}
	}
	return false;
}

/* a whole file of image/ (or a directory under it) by its path's components after "image/" */
static uint8_t *
fat_load_uncached(struct fatfs *f, const char *part, const char *path, uint32_t *size_out)
{
	const char *at = r5q_strstr(path, "/image/");
	uint32_t dir = 0, cluster = 0, size = 0;
	bool is_dir = false;
	uint8_t *data;

	if (!f->ok || at == NULL) {
		return NULL;
	}
	if (!fat_lookup(f, part, 0, "image", 5, &dir, &size, &is_dir) || !is_dir) {
		return NULL;
	}
	for (at += 7; *at;) {
		const char *end = r5q_strchr(at, '/');
		size_t len = end ? (size_t)(end - at) : strlen(at);

		if (!fat_lookup(f, part, dir, at, len, &cluster, &size, &is_dir)) {
			return NULL;
		}
		if (end == NULL) {
			break;
		}
		if (!is_dir) {
			return NULL;
		}
		dir = cluster;
		at = end + 1;
	}
	if (is_dir || size == 0 || size > 64 * 1024 * 1024 || (data = kalloc_data(size, Z_WAITOK)) == NULL) {
		return NULL;
	}
	for (uint32_t done = 0, c = cluster, cs = f->bs * f->spc; done < size;) {
		uint32_t take = size - done < cs ? size - done : cs;

		if (c < 2 || c >= f->clusters ||
		    redwood_ufs_read_named(part, ((uint64_t)f->data_start + (uint64_t)(c - 2) * f->spc) * f->bs, data + done, take, NULL)) {
			kfree_data(data, size);
			return NULL;
		}
		done += take;
		c = f->fat[c];
	}
	*size_out = size;
	return data;
}

struct tftp_transfer {
	struct redwood_qrtr_port *port;
	uint32_t node, client;
	uint8_t *data;
	uint32_t size, blksize, wsize, seek, rsize, sent_to;
	char name[96];
	bool writing;
	uint32_t got;                           /* (a write: the blocks received so far) */
	uint64_t began;                         /* (Test 93: when the request came, and the ACKs it took) */
	uint32_t acks, repeats;
};

static uint64_t tftp_began;

/* what the modem writes (its /readwrite/ files: "server_check.txt" first): kept in memory, never on the phone */
#define TFTP_RAM_FILES          16
#define TFTP_RAM_MAX            (1024 * 1024)
static struct {
	char name[96];
	uint8_t *data;
	uint32_t size;
} tftp_ram[TFTP_RAM_FILES];

static int
tftp_ram_find(const char *name, bool make)
{
	for (int i = 0; i < TFTP_RAM_FILES; i++) {
		if (tftp_ram[i].data != NULL && strcmp(tftp_ram[i].name, name) == 0) {
			return i;
		}
	}
	for (int i = 0; make && i < TFTP_RAM_FILES; i++) {
		if (tftp_ram[i].data == NULL && (tftp_ram[i].data = kalloc_data(TFTP_RAM_MAX, Z_WAITOK | Z_ZERO)) != NULL) {
			strlcpy(tftp_ram[i].name, name, sizeof(tftp_ram[i].name));
			tftp_ram[i].size = 0;
			return i;
		}
	}
	return -1;
}

static void
tftp_ack(struct tftp_transfer *t, uint32_t block)
{
	uint8_t pkt[4] = { 0, 4, (uint8_t)(block >> 8), (uint8_t)block };

	qrtr_send(t->port, t->node, t->client, pkt, sizeof(pkt));
}

static void
tftp_send_block(struct tftp_transfer *t, uint32_t block)
{
	uint64_t from = (uint64_t)t->seek + (uint64_t)(block - 1) * t->blksize, limit = t->seek + t->rsize;
	uint32_t n = 0;
	uint8_t *pkt = kalloc_data(4 + t->blksize, Z_WAITOK);

	if (pkt == NULL) {
		return;
	}
	if (limit > t->size) {
		limit = t->size;
	}
	if (from < limit) {
		n = (uint32_t)(limit - from < t->blksize ? limit - from : t->blksize);
		memcpy(pkt + 4, t->data + from, n);
	}
	pkt[0] = 0;
	pkt[1] = 3;
	pkt[2] = (uint8_t)(block >> 8);
	pkt[3] = (uint8_t)block;
	{
		int error = qrtr_send(t->port, t->node, t->client, pkt, 4 + n);
		static uint32_t send_failures;

		if (error != 0 && send_failures++ < 8) {
			printf("wifi: tftp: block %u of \"%s\" NOT SENT (%d)\n", block, t->name, error);
		}
	}
	kfree_data(pkt, 4 + t->blksize);
	t->sent_to = block;
}

static void
tftp_transfer_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct tftp_transfer *t = context;
	const uint8_t *m = data;
	uint32_t ack, last;
	uint64_t total;

	if (length < 4 || node != t->node || port != t->client) {
		return;
	}
	if (m[1] == 5) {                                /* ERROR from the client: it is done with it */
		char text[48];

		bzero(text, sizeof(text));
		memcpy(text, m + 4, length - 4 < sizeof(text) - 1 ? length - 4 : sizeof(text) - 1);
		printf("wifi: tftp: the modem ENDS \"%s\" with error %u \"%s\" (after block %u was sent; +%llu ms, %u ms for it)\n",
		    t->name, (uint32_t)(m[2] << 8 | m[3]), text, t->sent_to, (now_us() - tftp_began) / 1000,
		    (uint32_t)((now_us() - t->began) / 1000));
		goto done;
	}
	if (t->writing) {
		/* DATA of a write: into the file in memory; every block acknowledged; a short one is the last */
		int f = tftp_ram_find(t->name, false);
		uint32_t block, n;

		if (m[1] != 3 || f < 0) {
			return;
		}
		block = (uint32_t)(m[2] << 8 | m[3]);
		n = (uint32_t)length - 4;
		if (block == t->got + 1) {
			uint64_t at = (uint64_t)(block - 1) * t->blksize;

			if (at + n <= TFTP_RAM_MAX) {
				memcpy(tftp_ram[f].data + at, m + 4, n);
				if (at + n > tftp_ram[f].size) {
					tftp_ram[f].size = (uint32_t)(at + n);
				}
			}
			t->got = block;
		}
		tftp_ack(t, block);
		if (n < t->blksize) {
			printf("wifi: tftp: the modem wrote \"%s\": %u bytes (kept in memory)\n", t->name, tftp_ram[f].size);
			qrtr_close(t->port);
			kfree_data(t, sizeof(*t));
		}
		return;
	}
	if (m[1] != 4) {
		return;
	}
	ack = (uint32_t)(m[2] << 8 | m[3]);
	t->acks++;
	t->repeats += ack != 0 && ack <= t->got;
	t->got = ack > t->got ? ack : t->got;
	total = (uint64_t)t->rsize < (uint64_t)t->size - t->seek ? t->rsize : (uint64_t)t->size - t->seek;
	last = (uint32_t)(total / t->blksize) + 1;      /* the last block: short (or empty) */
	if (ack >= last) {
		printf("wifi: tftp: \"%s\": all %u blocks taken by the modem (+%llu ms, %u ms for it; %u acks, %u repeated)\n", t->name,
		    last, (now_us() - tftp_began) / 1000, (uint32_t)((now_us() - t->began) / 1000), t->acks, t->repeats);
		goto done;
	}
	for (uint32_t b = ack + 1; b <= ack + t->wsize && b <= last; b++) {
		tftp_send_block(t, b);
	}
	return;
done:
	qrtr_close(t->port);
	if ((tftp_ram_find(t->name, false) < 0 || t->data != tftp_ram[tftp_ram_find(t->name, false)].data) &&
	    !tftp_cached(t->data)) {
		kfree_data(t->data, t->size);   /* (a file of the phone's, neither kept for the modem's writes nor cached) */
	}
	kfree_data(t, sizeof(*t));
}

static void
tftp_error(uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t pkt[64];
	size_t n = strlen(text) + 1;

	pkt[0] = 0;
	pkt[1] = 5;
	pkt[2] = (uint8_t)(code >> 8);
	pkt[3] = (uint8_t)code;
	memcpy(pkt + 4, text, n < 59 ? n : 59);
	qrtr_send(tftp_port, node, port, pkt, 4 + (n < 59 ? n : 59));
}

static void
tftp_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const char *m = data, *end = m + length, *name, *mode, *o;
	struct tftp_transfer *t;
	uint8_t oack[256];
	size_t at = 2;
	bool options = false;

	if (length < 4 || m[0] != 0 || (m[1] != 1 && m[1] != 2)) {
		return;
	}
	name = m + 2;
	mode = r5q_has_nul(name, (size_t)(end - name)) ? name + strlen(name) + 1 : end;
	if (mode >= end) {
		return;
	}
	t = kalloc_data(sizeof(*t), Z_WAITOK | Z_ZERO);
	if (t == NULL) {
		return;
	}
	strlcpy(t->name, name, sizeof(t->name));
	if (m[1] == 2) {
		int f = tftp_ram_find(name, true);

		if (f < 0) {
			tftp_error(node, port, 3, "no room");
			kfree_data(t, sizeof(*t));
			return;
		}
		tftp_ram[f].size = 0;
		t->writing = true;
		t->data = tftp_ram[f].data;
		t->size = TFTP_RAM_MAX;
	} else if (tftp_ram_find(name, false) >= 0) {
		t->data = tftp_ram[tftp_ram_find(name, false)].data;   /* (one the modem wrote) */
		t->size = tftp_ram[tftp_ram_find(name, false)].size;
	} else {
		uint32_t android = 1;           /* (Test 108: Android's paths; tftp_android=0 serves as before) */

		PE_parse_boot_argn("tftp_android", &android, sizeof(android));
		if (android && strncmp(name, "/readonly/vendor/firmware_mnt/", 30) == 0) {
			t->data = NULL;         /* (not there on Android: the modem then asks for /readonly/vendor/firmware/) */
		} else if (android && strncmp(name, "/readonly/vendor/firmware/", 26) == 0) {
			t->data = store_load(name + 26, &t->size);
		} else {
			t->data = fat_load(&apnhlos, "apnhlos", name, &t->size);
			if (t->data == NULL && (modemfs.ok || fat_mount(&modemfs, "modem"))) {
				t->data = fat_load(&modemfs, "modem", name, &t->size);
			}
		}
	}
	if (t->data == NULL) {
		printf("wifi: tftp: the modem reads \"%s\": NOT FOUND\n", name);
		tftp_error(node, port, 1, "file not found");
		kfree_data(t, sizeof(*t));
		return;
	}
	t->node = node;
	t->client = port;
	t->blksize = 512;
	t->wsize = 1;
	t->rsize = t->size;
	oack[0] = 0;
	oack[1] = 6;
	for (o = mode + strlen(mode) + 1; o < end && *o;) {
		const char *value = o + strlen(o) + 1;
		uint32_t v = 0;
		char text[16];

		if (value >= end) {
			break;
		}
		for (const char *d = value; *d >= '0' && *d <= '9'; d++) {
			v = v * 10 + (uint32_t)(*d - '0');
		}
		if (strcmp(o, "blksize") == 0 && v >= 8 && v <= 65464) {
			/*
			 * (Test 91) at most 2048 a block: the 7680 the modem asks for are the largest GLINK messages this link
			 * has sent, and the Wi-Fi domain reloaded its image without end at that size; the OACK says what is used
			 */
			uint32_t cap = 65464;   /* (Test 91: 2048 made no difference; tftp_blksize= to cap it) */

			PE_parse_boot_argn("tftp_blksize", &cap, sizeof(cap));
			v = v > cap ? cap : v;
			t->blksize = v;
		} else if (strcmp(o, "wsize") == 0 && v >= 1) {
			t->wsize = v;
		} else if (strcmp(o, "rsize") == 0) {
			t->rsize = v;
		} else if (strcmp(o, "seek") == 0) {
			t->seek = v;
		} else if (strcmp(o, "tsize") == 0 && !t->writing) {
			v = t->size;
		}
		snprintf(text, sizeof(text), "%u", v);
		if (at + strlen(o) + strlen(text) + 2 < sizeof(oack)) {
			memcpy(oack + at, o, strlen(o) + 1);
			at += strlen(o) + 1;
			memcpy(oack + at, text, strlen(text) + 1);
			at += strlen(text) + 1;
			options = true;
		}
		o = value + strlen(value) + 1;
	}
	if (t->seek > t->size) {
		t->seek = t->size;
	}
	if (t->rsize == 0 || t->rsize > t->size - t->seek) {
		t->rsize = t->size - t->seek;
	}
	{
		char opts[160];
		int n = 0;

		bzero(opts, sizeof(opts));
		for (const char *q = mode + strlen(mode) + 1; q < end && *q && n < (int)sizeof(opts) - 24; q += strlen(q) + 1) {
			n += snprintf(opts + n, sizeof(opts) - (size_t)n, "%s%s", n && (n % 2 == 0 || 1) ? " " : "", q);
		}
		static uint32_t said;
		if (said++ < 120) {             /* (Test 93: all of them, timed: what the Wi-Fi loader asks for each time) */
			printf("wifi: tftp: +%llu ms: %s \"%s\" options: %s\n", (now_us() - tftp_began) / 1000, m[1] == 2 ? "WRQ" : "RRQ",
			    name, opts);
		}
		t->began = now_us();
	}
	t->port = qrtr_open(tftp_transfer_receive, NULL, t);
	if (t->port == NULL) {
		if (!tftp_cached(t->data) && tftp_ram_find(t->name, false) < 0) {
			kfree_data(t->data, t->size);
		}
		kfree_data(t, sizeof(*t));
		return;
	}
	if (t->writing) {
		printf("wifi: tftp: the modem writes \"%s\" (kept in memory, blocks of %u)\n", name, t->blksize);
		if (options) {
			qrtr_send(t->port, node, port, oack, at);
		} else {
			tftp_ack(t, 0);
		}
		return;
	}
	printf("wifi: tftp: the modem reads \"%s\": %u bytes from %u of %u (blocks of %u, %u at a time)\n", name, t->rsize, t->seek,
	    t->size, t->blksize, t->wsize);
	if (options) {
		qrtr_send(t->port, node, port, oack, at);
	} else {
		tftp_send_block(t, 1);
	}
}

static void
tftp_start(void)
{
	struct qrtr_control announce_it = { .command = QRTR_NEW_SERVER };
	int error;

	if (!apnhlos.ok && !fat_mount(&apnhlos, "apnhlos")) {
		printf("wifi: tftp: the apnhlos partition is not FAT16 as expected: no files for the modem\n");
		return;
	}
	tftp_began = now_us();
	tftp_port = qrtr_open(tftp_receive, NULL, NULL);
	if (tftp_port == NULL) {
		return;
	}
	announce_it.words[0] = SERVICE_TFTP;
	announce_it.words[1] = 1;               /* version 1, instance 0 */
	announce_it.words[2] = QRTR_US;
	announce_it.words[3] = tftp_port->number;
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	printf("wifi: tftp: SERVED at node %u port %u (apnhlos: %u-byte sectors, %u per cluster); announced: %s\n", QRTR_US,
	    tftp_port->number, apnhlos.bs, apnhlos.spc, error == 0 ? "yes" : "NOT SENT");
}

/*
 * r5q (Test 90): the protection-domain mapper (Linux pd-mapper; servreg locator, QMI service 0x40 version 1 instance
 * 1). The modem's root domain asks it which domains provide "kernel/elf_loader" and starts those: the Wi-Fi domain,
 * msm/modem/wlan_pd, which then loads image/wlanmdsp.mbn over TFTP. The table is the phone's own .jsn files
 * (modem: modemr.jsn; apnhlos: modemuw, adspr, adspua, cdspr, slpir, slpius).
 */
#define SERVICE_SERVREG_LOCATOR 0x40
static const struct {
	const char *domain;
	uint32_t instance;
	const char *services;                   /* "provider/service" words, separated by spaces */
} pd_table[] = {
	{ "msm/modem/root_pd", 180, "tms/servreg gps/gps_service" },
	{ "msm/modem/wlan_pd", 180, "kernel/elf_loader tms/servreg wlan/fw" },
	{ "msm/adsp/root_pd", 74, "tms/servreg" },
	{ "msm/adsp/audio_pd", 74, "tms/servreg avs/audio" },
	{ "msm/cdsp/root_pd", 76, "tms/servreg" },
	{ "msm/slpi/root_pd", 90, "tms/servreg" },
	{ "msm/slpi/sensor_pd", 90, "tms/servreg" },
};
static struct redwood_qrtr_port *pdmap_port;

static bool
pd_provides(unsigned d, const char *service)
{
	size_t n = strlen(service);

	for (const char *w = pd_table[d].services; *w;) {
		const char *end = w;

		while (*end && *end != ' ') {
			end++;
		}
		if ((size_t)(end - w) == n && strncmp(w, service, n) == 0) {
			return true;
		}
		w = *end ? end + 1 : end;
	}
	return false;
}

static void
pdmap_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint8_t out[512];
	char name[72];
	uint16_t txn, msg, n = 0, total = 0, rev = 1, result[2] = { 0, 0 };
	uint32_t offset = 0;
	size_t at = 7, list_at;
	uint8_t count = 0;

	if (length < 7 || m[0] != 0) {
		return;
	}
	txn = (uint16_t)(m[1] | m[2] << 8);
	msg = (uint16_t)(m[3] | m[4] << 8);
	bzero(name, sizeof(name));
	if ((v = qmi_tlv(m + 7, length - 7, 0x01, &n)) != NULL) {
		memcpy(name, v, n < sizeof(name) - 1 ? n : sizeof(name) - 1);
	}
	if ((v = qmi_tlv(m + 7, length - 7, 0x10, &n)) != NULL && n == 4) {
		memcpy(&offset, v, 4);
	}
	out[0] = 2;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	if (msg != 0x21) {
		printf("wifi: pd-mapper: request %#x not known: refused\n", msg);
		result[0] = 1;
		result[1] = 0x1e;
		at = qmi_put(out, at, 0x02, result, 4);
	} else {
		for (unsigned d = 0; d < sizeof(pd_table) / sizeof(pd_table[0]); d++) {
			total += pd_provides(d, name);
		}
		at = qmi_put(out, at, 0x02, result, 4);
		at = qmi_put(out, at, 0x10, &total, 2);
		at = qmi_put(out, at, 0x11, &rev, 2);
		/* TLV 0x12: a count, then each entry: the name with a length byte, the instance, "valid", the data */
		list_at = at;
		out[at++] = 0x12;
		at += 2;
		out[at++] = 0;
		for (unsigned d = 0, seen = 0; d < sizeof(pd_table) / sizeof(pd_table[0]); d++) {
			uint32_t zero = 0;
			size_t len = strlen(pd_table[d].domain);

			if (!pd_provides(d, name) || seen++ < offset || at + 1 + len + 9 > sizeof(out)) {
				continue;
			}
			out[at++] = (uint8_t)len;
			memcpy(out + at, pd_table[d].domain, len);
			at += len;
			memcpy(out + at, &pd_table[d].instance, 4);
			at += 4;
			out[at++] = 0;
			memcpy(out + at, &zero, 4);
			at += 4;
			count++;
		}
		out[list_at + 3] = count;
		out[list_at + 1] = (uint8_t)(at - list_at - 3);
		out[list_at + 2] = (uint8_t)((at - list_at - 3) >> 8);
		printf("wifi: pd-mapper: who provides \"%s\"? %u domain%s%s\n", name, count, count == 1 ? "" : "s",
		    pd_provides(1, name) && count ? " (the Wi-Fi domain among them)" : "");
	}
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(pdmap_port, node, port, out, at);
}

static void
pdmap_start(void)
{
	struct qrtr_control announce_it = { .command = QRTR_NEW_SERVER };
	int error;

	pdmap_port = qrtr_open(pdmap_receive, NULL, NULL);
	if (pdmap_port == NULL) {
		return;
	}
	announce_it.words[0] = SERVICE_SERVREG_LOCATOR;
	announce_it.words[1] = 1 | 1 << 8;      /* version 1, instance 1 */
	announce_it.words[2] = QRTR_US;
	announce_it.words[3] = pdmap_port->number;
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	printf("wifi: pd-mapper: SERVED at node %u port %u; announced: %s\n", QRTR_US, pdmap_port->number, error == 0 ? "yes" :
	    "NOT SENT");
}

/*
 * r5q (Test 94): a listener with the modem's service registry (servreg notifier, QMI service 0x42 version 1 instance 180;
 * Linux drivers/soc/qcom/pdr_interface.c, Android's service-notifier as icnss uses it): registered for the Wi-Fi domain and
 * the root domain, told of each change of their state, and every change acknowledged as Linux acknowledges it. Messages:
 * 0x20 register listener (0x01 enable, 0x02 path; answer 0x10 the state now), 0x22 state updated (0x01 state, 0x02 path,
 * 0x03 transaction), 0x23 set ack (0x01 path, 0x02 transaction). Boot-arg wifi_servreg=0 leaves it out.
 */
#define SERVICE_SERVREG_NOTIFIER        0x42
/*
 * (r5q, Test 172) The Wi-Fi domain went down (Test 168: after ~18 MB uploaded, "msm/modem/wlan_pd" DOWN then UP by
 * itself) and with it every ring and message the bring-up made: r5q_modem_link() does the bring-up again when it is
 * back (r5q_wifi_again()).
 */
static volatile bool r5q_pd_down, r5q_pd_back;
static volatile bool wlfw10_server;     /* (defined with its kin below; cleared here the moment the domain goes down) */
static struct redwood_qrtr_port *servreg_port;
static uint32_t servreg_node, servreg_where;
static uint16_t servreg_txn = 1;

static const char *
servreg_state(uint32_t s)
{
	return s == 0x1fffffff ? "UP" : s == 0x0fffffff ? "DOWN" : s == 0x2fffffff ? "EARLY DOWN" : s == 0x7fffffff ?
	       "UNINIT" : "?";
}

static void
servreg_send(uint16_t msg, const char *path, bool enable, uint16_t transaction, bool ack)
{
	uint8_t out[160];
	size_t at = 7, n = strlen(path);
	uint8_t on = enable;
	uint16_t txn = servreg_txn++;

	out[0] = 0;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	if (ack) {
		at = qmi_put(out, at, 0x01, path, (uint16_t)n);
		at = qmi_put(out, at, 0x02, &transaction, 2);
	} else {
		at = qmi_put(out, at, 0x01, &on, 1);
		at = qmi_put(out, at, 0x02, path, (uint16_t)n);
	}
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(servreg_port, servreg_node, servreg_where, out, at);
}

static void
servreg_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint16_t msg, n = 0, result[2] = { 0, 0 }, transaction = 0;
	uint32_t state = 0;
	char path[72];

	if (length < 7 || node != servreg_node || port != servreg_where) {
		return;
	}
	msg = (uint16_t)(m[3] | m[4] << 8);
	bzero(path, sizeof(path));
	if (m[0] == 2 && msg == 0x20) {
		if ((v = qmi_tlv(m + 7, length - 7, 0x02, &n)) != NULL && n == 4) {
			memcpy(result, v, 4);
		}
		if ((v = qmi_tlv(m + 7, length - 7, 0x10, &n)) != NULL && n == 4) {
			memcpy(&state, v, 4);
		}
		printf("wifi: servreg: registered (result %u, error %#x): the state now %#x (%s)\n", result[0], result[1], state,
		    servreg_state(state));
	} else if (m[0] == 4 && msg == 0x22) {
		if ((v = qmi_tlv(m + 7, length - 7, 0x01, &n)) != NULL && n == 4) {
			memcpy(&state, v, 4);
		}
		if ((v = qmi_tlv(m + 7, length - 7, 0x02, &n)) != NULL) {
			memcpy(path, v, n < sizeof(path) - 1 ? n : sizeof(path) - 1);
		}
		if ((v = qmi_tlv(m + 7, length - 7, 0x03, &n)) != NULL && n == 2) {
			memcpy(&transaction, v, 2);
		}
		printf("wifi: servreg: +%llu ms: \"%s\" IS NOW %#x (%s), transaction %u: acknowledged\n", (now_us() - tftp_began) / 1000,
		    path, state, servreg_state(state), transaction);
		if (strcmp(path, "msm/modem/wlan_pd") == 0) {
			if (state == 0x0fffffff || state == 0x2fffffff) {
				r5q_pd_down = true;
				wlfw10_server = false;  /* (Test 181: here, before its service comes back -- Test 180 lost that race) */
			} else if (state == 0x1fffffff && r5q_pd_down) {
				r5q_pd_back = true;
			}
		}
		servreg_send(0x23, path, true, transaction, true);
	} else if (m[0] == 2 && msg == 0x23) {
		if ((v = qmi_tlv(m + 7, length - 7, 0x02, &n)) != NULL && n == 4) {
			memcpy(result, v, 4);
		}
		printf("wifi: servreg: our acknowledgement taken (result %u, error %#x)\n", result[0], result[1]);
	} else if (m[0] == 2 && msg == 0x21) {
		if ((v = qmi_tlv(m + 7, length - 7, 0x02, &n)) != NULL && n == 4) {
			memcpy(result, v, 4);
		}
		printf("wifi: servreg: the restart asked for: result %u, error %#x%s\n", result[0], result[1],
		    result[0] == 0 ? " (taken)" : " (REFUSED)");
	} else {
		printf("wifi: servreg: message %#x of type %u (%zu bytes)\n", msg, m[0], length);
	}
}

/* (Test 172) Linux's pdr_restart_pd: ask the registry to restart a domain (message 0x21, its path), for a test of the above */
static void
servreg_restart(const char *path)
{
	uint8_t out[96];
	size_t at = 7;
	uint16_t txn = servreg_txn++;

	if (servreg_port == NULL) {
		return;
	}
	out[0] = 0;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = 0x21;
	out[4] = 0;
	at = qmi_put(out, at, 0x01, path, (uint16_t)strlen(path));
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(servreg_port, servreg_node, servreg_where, out, at);
	printf("wifi: servreg: asked to restart \"%s\" (wifi_pd_restart)\n", path);
}

static void
servreg_start(void)
{
	uint32_t on = 1;

	PE_parse_boot_argn("wifi_servreg", &on, sizeof(on));
	if (on == 0) {
		return;
	}
	for (unsigned ms = 0; ms < 3000 && servreg_where == 0; ms += 10) {
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].service == SERVICE_SERVREG_NOTIFIER) {
				servreg_node = qrtr_servers[i].node;
				servreg_where = qrtr_servers[i].port;
			}
		}
		if (servreg_where == 0) {
			sleep_ms(10);
		}
	}
	if (servreg_where == 0 || (servreg_port = qrtr_open(servreg_receive, NULL, NULL)) == NULL) {
		printf("wifi: servreg: the modem's service registry (0x42) is not there: no listener\n");
		return;
	}
	printf("wifi: servreg: a listener at port %u, with the registry at node %u port %u: the Wi-Fi and root domains\n",
	    servreg_port->number, servreg_node, servreg_where);
	servreg_send(0x20, "msm/modem/wlan_pd", true, 0, false);
	servreg_send(0x20, "msm/modem/root_pd", true, 0, false);
}

/*
 * r5q (Test 103): DIAG with the modem over QRTR, as linux-msm/diag's router does it (peripheral-qrtr.c, diag_cntl.c):
 * service 4097, the AP publishing the control (version 0) and data (version 2) ends for the modem (instance base 0);
 * the modem sends its feature mask on the control end, which is answered (feature mask, message masks, real-time
 * mode, streaming), and then its F3 messages arrive on the data end as non-HDLC frames (7e 01 len payload 7e). Masks:
 * every SSID at ERROR and FATAL, SSID 100 (TMS: pd_mon, the virtual dog, servreg) also at MED and HIGH. Each payload
 * goes to the log as hex ("diag:") for the PC to decode against the modem's qdsp6m.qdb. Boot-arg wifi_diag=1.
 */
#define SERVICE_DIAG            4097
static struct redwood_qrtr_port *diag_cntl_port, *diag_data_port;
static uint32_t diag_lines, diag_lines_max = 6000, diag_next_id = 2;
static volatile bool diag_masks_due;
static bool diag_all_levels, diag_all_enabled;
static uint32_t diag_mask_node, diag_mask_port;

static bool
strstr_r5q_root(const char *name)
{
	size_t n = strlen(name);

	return n >= 7 && strcmp(name + n - 7, "root_pd") == 0;
}

static void
diag_cntl_send(uint32_t node, uint32_t port, const void *pkt, size_t len)
{
	int error = qrtr_send(diag_cntl_port, node, port, pkt, len);

	if (error != 0) {
		printf("wifi: diag: control packet %u NOT SENT (%d)\n", *(const uint32_t *)pkt, error);
	}
}

static void
diag_send_masks(uint32_t node, uint32_t port)
{
	static const uint16_t first[25] = { 0, 500, 1000, 2000, 3000, 4000, 4500, 4600, 5000, 5500, 6000, 6500, 7000, 7100,
	    7200, 8000, 8500, 9000, 9500, 10200, 10251, 10300, 10350, 10400, 0xc000 };
	static const uint16_t last[25] = { 120, 506, 1007, 2008, 3014, 4010, 4573, 4615, 5033, 5516, 6081, 6521, 7003, 7111,
	    7201, 8000, 8529, 9008, 9510, 10210, 10255, 10300, 10377, 10416, 0xc063 };
	uint8_t *pkt = kalloc_data(24 + 4 * 128, Z_WAITOK | Z_ZERO);
	struct __attribute__((packed)) {
		uint32_t cmd, len, version, sleep_vote, real_time, use_nrt, commit, sleep_threshold, sleep_time, drain, stale;
	} mode = { 3, 36, 1, 1, 1, 0, 0, 0, 0, 0, 0 };
	struct __attribute__((packed)) {
		uint32_t cmd, len, version;
		uint8_t stream_id, tx_mode;
	} buffering = { 17, 6, 1, 1, 0 };      /* (stream 1, streaming: msm's values) */

	if (pkt == NULL) {
		return;
	}
	for (unsigned r = 0; r < 25; r++) {
		uint32_t n = (uint32_t)(last[r] - first[r] + 1), len = 19 + 4 * n, word;

		if (n > 128) {
			continue;
		}
		bzero(pkt, 24 + 4 * 128);
		word = 11;
		memcpy(pkt, &word, 4);
		word = len - 8;
		memcpy(pkt + 4, &word, 4);
		pkt[8] = 1;                     /* stream 1 */
		pkt[9] = 3;                     /* DIAG_CTRL_MASK_VALID */
		pkt[10] = 0;
		if (diag_all_enabled) {         /* (wifi_diag=3: DIAG_CTRL_MASK_ALL_ENABLED, one word a range, as msm sends it) */
			n = 1;
			len = 19 + 4;
			word = len - 8;
			memcpy(pkt + 4, &word, 4);
			pkt[9] = 2;
		}
		memcpy(pkt + 11, &first[r], 2);
		memcpy(pkt + 13, &last[r], 2);
		memcpy(pkt + 15, &n, 4);
		for (uint32_t i = 0; i < n; i++) {
			word = diag_all_levels ? 0x1f : first[r] + i == 100 ? 0x1e : 0x18;     /* (wifi_diag=2: everything) */
			memcpy(pkt + 19 + 4 * i, &word, 4);
		}
		diag_cntl_send(node, port, pkt, len);
	}
	kfree_data(pkt, 24 + 4 * 128);
	diag_cntl_send(node, port, &mode, sizeof(mode));
	diag_cntl_send(node, port, &buffering, sizeof(buffering));
	printf("wifi: diag: masks sent (all SSIDs ERROR+FATAL, TMS 100 from MED), real-time, streaming\n");
}

static void
diag_cntl_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	size_t at = 0;

	while (at + 8 <= length) {
		uint32_t cmd, len;

		memcpy(&cmd, m + at, 4);
		memcpy(&len, m + at + 4, 4);
		if (at + 8 + len > length) {
			break;
		}
		if (cmd == 8 && len >= 5) {             /* feature mask: a length, then that many bytes of mask */
			uint32_t theirs = 0, mask_len;
			uint8_t reply[18];
			uint32_t words[4] = { 8, 10, 4, 0 };

			memcpy(&mask_len, m + at + 8, 4);
			memcpy(&theirs, m + at + 12, mask_len < 4 ? (mask_len < len - 4 ? mask_len : len - 4) : 4);
			words[3] = theirs & ((1u << 0) | (1u << 1) | (1u << 4) | (1u << 6) | (1u << 11) | (1u << 13) | (1u << 15));
			bzero(reply, sizeof(reply));
			memcpy(reply, words, 16);
			printf("wifi: diag: THE MODEM SPEAKS DIAG (features %#x); ours %#x\n", theirs, words[3]);
			diag_cntl_send(node, port, reply, sizeof(reply));
			/* (the masks follow the root PD's DIAG id, as msm sends them when DIAG-ID is agreed) */
		} else if (cmd == 33 && len >= 8) {     /* diag id: a name, given a number */
			char name[40];
			uint8_t reply[64];
			uint32_t words[4] = { 33, 0, 1, diag_next_id++ };
			size_t n;

			bzero(name, sizeof(name));
			{
				uint32_t version, flen = 0;
				size_t off = 16;        /* v1: version, id, the name */

				memcpy(&version, m + at + 8, 4);
				if (version >= 2 && len >= 12) {        /* v2+: version, id, feature length, the features, the name */
					memcpy(&flen, m + at + 16, 4);
					off = 20 + flen;
				}
				if (off < 8 + len) {
					memcpy(name, m + at + off, 8 + len - off < sizeof(name) - 1 ? 8 + len - off : sizeof(name) - 1);
				}
				printf("wifi: diag: diag-id request version %u, feature length %u\n", version, flen);
			}
			n = strlen(name) + 1;
			words[1] = (uint32_t)(8 + n);
			memcpy(reply, words, 16);
			memcpy(reply + 16, name, n);
			printf("wifi: diag: id %u for \"%s\"\n", words[3], name);
			diag_cntl_send(node, port, reply, 16 + n);
			if (strstr_r5q_root(name)) {    /* (the masks, from the link thread, once the root PD has its id) */
				diag_mask_node = node;
				diag_mask_port = port;
				diag_masks_due = true;
			}
		} else {
			printf("wifi: diag: control %u (%u bytes)\n", cmd, len);
		}
		at += 8 + len;
	}
}

static void
diag_data_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	size_t at = 0;
	static uint32_t raw_said;

	if (raw_said < 12) {
		char raw[3 * 24 + 1];
		size_t k = 0;

		raw_said++;
		for (size_t i = 0; i < length && i < 24; i++) {
			k += (size_t)snprintf(raw + k, sizeof(raw) - k, "%02x ", m[i]);
		}
		raw[k] = 0;
		printf("wifi: diag: data datagram %u, %zu bytes: %s\n", raw_said, length, raw);
	}
	while (at + 5 <= length && m[at] == 0x7e && m[at + 1] == 1) {
		uint16_t n = (uint16_t)(m[at + 2] | m[at + 3] << 8);
		char line[3 * 80 + 1];
		size_t k = 0;

		if (at + 5 + n > length) {
			break;
		}
		if (diag_lines < diag_lines_max) {
			for (uint16_t i = 0; i < n && i < 80; i++) {
				k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x", m[at + 4 + i]);
			}
			line[k] = 0;
			diag_lines++;
			printf("diag: +%llu %s\n", (now_us() - tftp_began) / 1000, line);
		}
		at += 5 + n;
	}
}

static struct redwood_qrtr_port *diag_cmd_port;

static void
diag_cmd_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	char raw[3 * 32 + 1];
	size_t k = 0;

	for (size_t i = 0; i < length && i < 32; i++) {
		k += (size_t)snprintf(raw + k, sizeof(raw) - k, "%02x ", m[i]);
	}
	raw[k] = 0;
	printf("wifi: diag: command answer, %zu bytes: %s\n", length, raw);
}

/* one DIAG command to the modem's command end (4097 version 1), as a non-HDLC frame */
static void
diag_command(const uint8_t *payload, uint16_t n)
{
	uint8_t frame[64];
	uint32_t node = 0, where = 0;

	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == SERVICE_DIAG && (qrtr_servers[i].instance & 0xff) == 1) {
			node = qrtr_servers[i].node;
			where = qrtr_servers[i].port;
		}
	}
	if (where == 0 || n + 5 > sizeof(frame)) {
		printf("wifi: diag: the modem's command end is not there\n");
		return;
	}
	if (diag_cmd_port == NULL && (diag_cmd_port = qrtr_open(diag_cmd_receive, NULL, NULL)) == NULL) {
		return;
	}
	/* (APPS_HDLC_ENCODE agreed: commands go to the peripheral raw, as linux-msm/diag's qrtr_perif_send sends them) */
	memcpy(frame, payload, n);
	printf("wifi: diag: command %#x to %u:%u: %s\n", payload[0], node, where,
	    qrtr_send(diag_cmd_port, node, where, frame, n) == 0 ? "sent" : "NOT SENT");
}

static void
diag_commands(void)
{
	static const uint8_t version[1] = { 0x00 };                                     /* DIAG_VERNO_F */
	static const uint8_t all[8] = { 0x7d, 0x05, 0, 0, 0x18, 0, 0, 0 };              /* every SSID: ERROR, FATAL */
	static const uint8_t tms[12] = { 0x7d, 0x04, 100, 0, 100, 0, 0, 0, 0x1e, 0, 0, 0 };    /* SSID 100: MED up */

	diag_command(version, sizeof(version));
	diag_command(all, sizeof(all));
	diag_command(tms, sizeof(tms));
}

static void
diag_start(void)
{
	struct qrtr_control announce_it = { .command = QRTR_NEW_SERVER };
	uint32_t on = 0;

	if (!PE_parse_boot_argn("wifi_diag", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("wifi_diag_lines", &diag_lines_max, sizeof(diag_lines_max));
	diag_all_levels = on >= 2;
	diag_all_enabled = on == 3;
	diag_cntl_port = qrtr_open(diag_cntl_receive, NULL, NULL);
	diag_data_port = qrtr_open(diag_data_receive, NULL, NULL);
	if (diag_cntl_port == NULL || diag_data_port == NULL) {
		return;
	}
	announce_it.words[0] = SERVICE_DIAG;
	announce_it.words[1] = 0;               /* "version" 0 = the modem's control end, instance 0 */
	announce_it.words[2] = QRTR_US;
	announce_it.words[3] = diag_cntl_port->number;
	qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	announce_it.words[1] = 2;               /* its data end */
	announce_it.words[3] = diag_data_port->number;
	qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	printf("wifi: diag: control at port %u, data at port %u, announced (service 4097)\n", diag_cntl_port->number,
	    diag_data_port->number);
}

/*
 * r5q (Test 107): the modem's own word on what failed, asked of its subsystem control (SSCTL, QMI 0x2b version 2
 * instance 18; msm sysmon-qmi.c): 0x22 get failure reason, an empty request; the answer's TLV 0x10 is the message (a
 * length byte, then up to 90 characters). Asked a few times while the Wi-Fi domain loads and dies.
 */
#define SERVICE_SSCTL           0x2b
static struct redwood_qrtr_port *ssctl_port;
static uint16_t ssctl_txn = 1;

static void
ssctl_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint16_t n = 0, result[2] = { 0, 0 };
	char text[96];

	if (length < 7) {
		return;
	}
	bzero(text, sizeof(text));
	if ((v = qmi_tlv(m + 7, length - 7, 0x02, &n)) != NULL && n == 4) {
		memcpy(result, v, 4);
	}
	if ((v = qmi_tlv(m + 7, length - 7, 0x10, &n)) != NULL && n >= 1) {
		memcpy(text, v + 1, (size_t)(n - 1) < sizeof(text) - 1 ? (size_t)(n - 1) : sizeof(text) - 1);
	}
	printf("wifi: ssctl: +%llu ms: failure reason (result %u, error %#x): \"%s\"\n", (now_us() - tftp_began) / 1000, result[0],
	    result[1], text);
}

static void
ssctl_ask(void)
{
	uint8_t out[7];
	uint32_t node = 0, where = 0;

	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == SERVICE_SSCTL) {
			node = qrtr_servers[i].node;
			where = qrtr_servers[i].port;
		}
	}
	if (where == 0 || (ssctl_port == NULL && (ssctl_port = qrtr_open(ssctl_receive, NULL, NULL)) == NULL)) {
		return;
	}
	out[0] = 0;
	out[1] = (uint8_t)ssctl_txn;
	out[2] = (uint8_t)(ssctl_txn++ >> 8);
	out[3] = 0x22;
	out[4] = 0;
	out[5] = 0;
	out[6] = 0;
	qrtr_send(ssctl_port, node, where, out, sizeof(out));
}

/*
 * r5q (Test 95): memshare (QMI service 0x34 version 1 instance 1; Android's msm_memshare.c, heap_mem_ext_v01): the modem
 * asks the AP for heap memory (live DT qcom,memshare: client ids 0 and 2 of no size, id 1 5 MB at boot, id 3 32 MB at boot
 * from modem_shared_mem_region 0xb3900000). This test only listens: every request is written down and refused, nothing
 * is handed out. Messages: 0x20 alloc, 0x21 free, 0x22 alloc generic (0x01 bytes, 0x02 client, 0x03 proc, 0x04 sequence),
 * 0x23 free generic, 0x24 query size (0x01 client, 0x10 proc). Boot-arg wifi_memshare=0 leaves it out.
 */
#define SERVICE_MEMSHARE        0x34
static struct redwood_qrtr_port *memshare_port;

static void
memshare_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *v;
	uint8_t out[64];
	uint16_t txn, msg, n = 0, result[2] = { 1, 0x1e };
	uint32_t words[5] = { 0, 0, 0, 0, 0 };
	size_t at = 7;

	if (length < 7 || m[0] != 0) {
		return;
	}
	txn = (uint16_t)(m[1] | m[2] << 8);
	msg = (uint16_t)(m[3] | m[4] << 8);
	for (unsigned i = 0; i < 4; i++) {
		if ((v = qmi_tlv(m + 7, length - 7, (uint8_t)(i + 1), &n)) != NULL && n == 4) {
			memcpy(&words[i], v, 4);
		}
	}
	if ((v = qmi_tlv(m + 7, length - 7, 0x10, &n)) != NULL && n == 4) {
		memcpy(&words[4], v, 4);
	}
	printf("wifi: memshare: +%llu ms: THE MODEM ASKS: message %#x (%s): TLVs 1-4 %#x %#x %#x %#x, 0x10 %#x (%zu bytes): refused\n",
	    (now_us() - tftp_began) / 1000, msg, msg == 0x20 ? "alloc" : msg == 0x21 ? "free" : msg == 0x22 ? "alloc generic" :
	    msg == 0x23 ? "free generic" : msg == 0x24 ? "query size" : "?", words[0], words[1], words[2], words[3], words[4],
	    length);
	out[0] = 2;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	at = qmi_put(out, at, 0x02, result, 4);
	out[5] = (uint8_t)(at - 7);
	out[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(memshare_port, node, port, out, at);
}

static void
memshare_start(void)
{
	struct qrtr_control announce_it = { .command = QRTR_NEW_SERVER };
	uint32_t on = 1;
	int error;

	PE_parse_boot_argn("wifi_memshare", &on, sizeof(on));
	if (on == 0 || (memshare_port = qrtr_open(memshare_receive, NULL, NULL)) == NULL) {
		return;
	}
	announce_it.words[0] = SERVICE_MEMSHARE;
	announce_it.words[1] = 1 | 1 << 8;      /* version 1, instance 1 */
	announce_it.words[2] = QRTR_US;
	announce_it.words[3] = memshare_port->number;
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &announce_it, sizeof(announce_it));
	printf("wifi: memshare: SERVED (listening only) at node %u port %u; announced: %s\n", QRTR_US, memshare_port->number,
	    error == 0 ? "yes" : "NOT SENT");
}

/*
 * r5q (Test 92): the WCN3990's supplies and clock, asked of RPMh as Android's icnss / Linux's ath10k_snoc ask for them
 * before the Wi-Fi firmware starts (the Wi-Fi domain loaded its image and then missed its watchdog without them).
 * Live DT qcom,icnss@18800000: vdd-cx-mx pm8150 L1 (ldoa1, 752 mV), vdd-1.8-xo pm8150 L7 (ldoa7, 1800 mV), vdd-1.3-rfa
 * pm8150l L2 (ldoc2, 1304 mV), vdd-3.3-ch0 pm8150l L11 (ldoc11, 3104 mV), clock rf_clk3 (rfclka3). The resources'
 * addresses from cmd-db (0x85f20000; Linux cmd-db.c); the requests through the apps RSC (DRV 2, TCS at 0x18220d00;
 * live DT tcs-config: TCS 0-1 active, used here: 1; Linux rpmh-rsc.c), as votes the AOP aggregates with every other
 * master's and drops at a reset. No PMIC register is written by this kernel. Boot-arg wifi_rails=1.
 */
#define R5Q_CMD_DB_PHYS         0x85f20000UL
#define R5Q_CMD_DB_SIZE         0x20000u
#define R5Q_RSC_TCS_PHYS        0x18220d00UL
#define R5Q_RSC_TCS_SIZE        0x3000u
#define R5Q_RSC_TCS_ID          1
#define RSC_TCS_STRIDE          672
#define RSC_CMD_STRIDE          20

static uint32_t
cmd_db_addr(const volatile uint8_t *db, const char *id)
{
	char want[8];

	bzero(want, sizeof(want));
	for (int i = 0; i < 8 && id[i]; i++) {
		want[i] = id[i];
	}
	if (db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c) {
		return 0;
	}
	for (int s = 0; s < 8; s++) {
		const volatile uint8_t *h = db + 8 + 16 * s;
		uint32_t hoff = (uint32_t)(h[2] | h[3] << 8), cnt = (uint32_t)(h[6] | h[7] << 8);

		for (uint32_t i = 0; i < cnt && 144 + hoff + 24 * (i + 1) <= R5Q_CMD_DB_SIZE; i++) {
			const volatile uint8_t *e = db + 144 + hoff + 24 * i;
			bool same = true;

			for (int k = 0; k < 8 && same; k++) {
				same = e[k] == (uint8_t)want[k];
			}
			if (same) {
				return (uint32_t)(e[16] | e[17] << 8 | e[18] << 16 | (uint32_t)e[19] << 24);
			}
		}
	}
	return 0;
}

/* one batch of RPMh writes through an active TCS, each waited for; false if the TCS is busy or a command does not complete */
static bool rsc_write_one(vm_offset_t tcs_base, const uint32_t *addr, const uint32_t *data, unsigned n);
static volatile uint32_t rsc_busy;      /* (Test 191: the microphones' clock vote may come from another thread) */

static bool
rsc_write(vm_offset_t tcs_base, const uint32_t *addr, const uint32_t *data, unsigned n)
{
	bool ok;

	while (!__sync_bool_compare_and_swap(&rsc_busy, 0, 1)) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	ok = rsc_write_one(tcs_base, addr, data, n);
	__sync_lock_release(&rsc_busy);
	return ok;
}

/*
 * (r5q, Test 191) For others in the kernel (the microphones' codec clock, redwood_adsp.c): an RPMh resource of
 * cmd-db, by its name, enabled (+4 = 1: a VRM regulator's or clock's enable) through the same TCS. True if RPMh says done.
 */
bool
redwood_r5q_rpmh_enable(const char *id)
{
	static const volatile uint8_t *db;
	static vm_offset_t tcs;
	uint32_t addr, one = 1;

	if (db == NULL) {
		db = (const volatile uint8_t *)ml_io_map_unmappable(R5Q_CMD_DB_PHYS, R5Q_CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (tcs == 0) {
		tcs = ml_io_map(R5Q_RSC_TCS_PHYS, R5Q_RSC_TCS_SIZE);
	}
	addr = db != NULL ? cmd_db_addr(db, id) : 0;
	if (addr == 0 || addr > 0xfffff || tcs == 0) {
		printf("rpmh: %s is not in cmd-db: not enabled\n", id);
		return false;
	}
	addr += 4;
	printf("rpmh: %s at %#x: enable\n", id, addr - 4);
	return rsc_write(tcs, &addr, &one, 1);
}

/*
 * (r5q Test 276, the camera) A rail of cmd-db voted to a level: `vlvl` (RPMh's level number, e.g. 128 SVS, 192
 * SVS_L1) turned into its position in the rail's level list (the entry's aux data, u16 each), and that position written
 * to the rail's address. vlvl 0 gives the vote back (position 0). Returns the position written, or -1.
 */
int
redwood_r5q_rpmh_level(const char *id, unsigned vlvl)
{
	static const volatile uint8_t *db;
	static vm_offset_t tcs;
	char want[8];

	if (db == NULL) {
		db = (const volatile uint8_t *)ml_io_map_unmappable(R5Q_CMD_DB_PHYS, R5Q_CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (tcs == 0) {
		tcs = ml_io_map(R5Q_RSC_TCS_PHYS, R5Q_RSC_TCS_SIZE);
	}
	if (db == NULL || tcs == 0 || db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c) {
		return -1;
	}
	bzero(want, sizeof(want));
	for (int i = 0; i < 8 && id[i]; i++) {
		want[i] = id[i];
	}
	for (int s = 0; s < 8; s++) {
		const volatile uint8_t *h = db + 8 + 16 * s;
		uint32_t hoff = (uint32_t)(h[2] | h[3] << 8), doff = (uint32_t)(h[4] | h[5] << 8), cnt = (uint32_t)(h[6] | h[7] << 8);

		for (uint32_t i = 0; i < cnt && 144 + hoff + 24 * (i + 1) <= R5Q_CMD_DB_SIZE; i++) {
			const volatile uint8_t *e = db + 144 + hoff + 24 * i;
			bool same = true;
			uint32_t addr, len, off, position = 0, n;

			for (int k = 0; k < 8 && same; k++) {
				same = e[k] == (uint8_t)want[k];
			}
			if (!same) {
				continue;
			}
			addr = (uint32_t)(e[16] | e[17] << 8 | e[18] << 16 | (uint32_t)e[19] << 24);
			len = (uint32_t)(e[20] | e[21] << 8);
			off = (uint32_t)(e[22] | e[23] << 8);
			n = len / 2;
			if (vlvl != 0) {
				position = n;
				for (uint32_t l = 0; l < n && 144 + doff + off + 2 * l + 1 < R5Q_CMD_DB_SIZE; l++) {
					const volatile uint8_t *v = db + 144 + doff + off + 2 * l;
					uint32_t level = (uint32_t)(v[0] | v[1] << 8);

					if (l > 0 && level == 0) {
						break;
					}
					if (level >= vlvl) {
						position = l;
						break;
					}
				}
				if (position >= n) {
					printf("rpmh: %s has no level >= %u\n", id, vlvl);
					return -1;
				}
			}
			printf("rpmh: %s at %#x: level %u = position %u\n", id, addr, vlvl, position);
			return rsc_write(tcs, &addr, &position, 1) ? (int)position : -1;
		}
	}
	printf("rpmh: %s is not in cmd-db\n", id);
	return -1;
}

/*
 * (r5q Test 305, the camera) A bus clock manager (BCM) of cmd-db voted for bandwidth, as Linux's qcom bcm-voter does:
 * the entry's aux data is { le32 unit; le16 width; u8 vcd; u8 reserved }; the vote is AB (x, bits 27:14) and IB
 * (y, bits 13:0) in that unit, 14 bits each, with "valid" (bit 29) and "commit" (bit 30). `mask` != 0 makes it an
 * enable-mask BCM's vote (ACV): y = mask, x = 0. avg/peak in kB/s; 0, 0 gives the vote back. Returns the command
 * written, or 0.
 */
uint32_t
redwood_r5q_rpmh_bcm(const char *id, uint64_t avg_kbps, uint64_t peak_kbps, uint32_t mask)
{
	static const volatile uint8_t *db;
	static vm_offset_t tcs;
	char want[8];

	if (db == NULL) {
		db = (const volatile uint8_t *)ml_io_map_unmappable(R5Q_CMD_DB_PHYS, R5Q_CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (tcs == 0) {
		tcs = ml_io_map(R5Q_RSC_TCS_PHYS, R5Q_RSC_TCS_SIZE);
	}
	if (db == NULL || tcs == 0 || db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c) {
		return 0;
	}
	bzero(want, sizeof(want));
	for (int i = 0; i < 8 && id[i]; i++) {
		want[i] = id[i];
	}
	for (int s = 0; s < 8; s++) {
		const volatile uint8_t *h = db + 8 + 16 * s;
		uint32_t hoff = (uint32_t)(h[2] | h[3] << 8), doff = (uint32_t)(h[4] | h[5] << 8), cnt = (uint32_t)(h[6] | h[7] << 8);

		for (uint32_t i = 0; i < cnt && 144 + hoff + 24 * (i + 1) <= R5Q_CMD_DB_SIZE; i++) {
			const volatile uint8_t *e = db + 144 + hoff + 24 * i;
			bool same = true;
			uint32_t addr, len, off, unit = 0, width = 0, cmd;
			uint64_t x = 0, y = 0;

			for (int k = 0; k < 8 && same; k++) {
				same = e[k] == (uint8_t)want[k];
			}
			if (!same) {
				continue;
			}
			addr = (uint32_t)(e[16] | e[17] << 8 | e[18] << 16 | (uint32_t)e[19] << 24);
			len = (uint32_t)(e[20] | e[21] << 8);
			off = (uint32_t)(e[22] | e[23] << 8);
			if (len >= 6 && 144 + doff + off + 6 <= R5Q_CMD_DB_SIZE) {
				const volatile uint8_t *a = db + 144 + doff + off;

				unit = (uint32_t)(a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24);
				width = (uint32_t)(a[4] | a[5] << 8);
			}
			if (mask != 0) {
				y = mask;
			} else if (unit != 0 && (avg_kbps != 0 || peak_kbps != 0)) {
				/* Linux: y = peak * 1000 / unit; x = avg * 1000 * width / (unit * buswidth) -- buswidth taken as width */
				y = (peak_kbps * 1000 + unit - 1) / unit;
				x = (avg_kbps * 1000 + unit - 1) / unit;
				y = y == 0 ? 1 : y > 0x3fff ? 0x3fff : y;
				x = x > 0x3fff ? 0x3fff : x;
			}
			cmd = (1u << 30) | (1u << 29) | ((uint32_t)x << 14) | (uint32_t)y;
			printf("rpmh: BCM %s at %#x (unit %u, width %u): AB %llu, IB %llu -> %#x\n", id, addr, unit, width, x, y, cmd);
			return rsc_write(tcs, &addr, &cmd, 1) ? cmd : 0;
		}
	}
	printf("rpmh: BCM %s is not in cmd-db\n", id);
	return 0;
}

static bool
rsc_write_one(vm_offset_t tcs_base, const uint32_t *addr, const uint32_t *data, unsigned n)
{
	vm_offset_t t = tcs_base + RSC_TCS_STRIDE * R5Q_RSC_TCS_ID;
	uint32_t ctrl, mask = (1u << n) - 1, done = 0;

	if (n == 0 || n > 16 || rd(t + 0x18) == 0) {           /* DRV_STATUS: zero while the TCS is busy */
		printf("wifi: rails: TCS %u is busy (status %#x): nothing sent\n", R5Q_RSC_TCS_ID, rd(t + 0x18));
		return false;
	}
	for (unsigned i = 0; i < n; i++) {
		wr(t + 0x30 + RSC_CMD_STRIDE * i, 0x10108);     /* MSGID: write, response requested, 8 bytes */
		wr(t + 0x34 + RSC_CMD_STRIDE * i, addr[i]);
		wr(t + 0x38 + RSC_CMD_STRIDE * i, data[i]);
	}
	wr(t + 0x10, mask);                                     /* CMD_WAIT_FOR_CMPL */
	wr(t + 0x1c, mask);                                     /* CMD_ENABLE */
	ctrl = rd(t + 0x14);
	wr(t + 0x14, ctrl & ~(1u << 24));
	wr(t + 0x14, ctrl & ~((1u << 24) | (1u << 16)));
	wr(t + 0x14, 1u << 16);                                 /* AMC mode */
	wr(t + 0x14, (1u << 16) | (1u << 24));                  /* ... and trigger */
	for (unsigned us = 0; us < 200000; us += 50) {
		done = 0;
		for (unsigned i = 0; i < n; i++) {
			done |= (rd(t + 0x3c + RSC_CMD_STRIDE * i) & (1u << 16)) ? (1u << i) : 0;     /* CMD_STATUS: completed */
		}
		if (done == mask) {
			break;
		}
		sleep_us(50);
	}
	wr(t + 0x1c, 0);
	wr(tcs_base + 0x08, 1u << R5Q_RSC_TCS_ID);              /* IRQ_CLEAR */
	ctrl = rd(t + 0x14);
	wr(t + 0x14, ctrl & ~(1u << 24));
	wr(t + 0x14, ctrl & ~((1u << 24) | (1u << 16)));
	if (done != mask) {
		printf("wifi: rails: RPMh did not complete every request (%#x of %#x)\n", done, mask);
	}
	return done == mask;
}

static void
r5q_wifi_rails(void)
{
	static const struct {
		const char *id, *what;
		uint32_t mv;                    /* 0: a clock */
	} rails[] = {
		{ "ldoa1", "vdd-cx-mx (pm8150 L1)", 752 }, { "ldoa7", "vdd-1.8-xo (pm8150 L7)", 1800 },
		{ "ldoc2", "vdd-1.3-rfa (pm8150l L2)", 1304 }, { "ldoc11", "vdd-3.3-ch0 (pm8150l L11)", 3104 },
		{ "rfclka3", "rf_clk3", 0 },
		{ "rfclka2", "rf_clk2 (Linux sm8150 wifi: cxo_ref_clk_pin)", 0 },      /* (Test 96) */
	};
	const volatile uint8_t *db;
	vm_offset_t tcs;
	uint32_t base[6], addr[16], data[16], on = 0;
	unsigned n = 0;

	if (!PE_parse_boot_argn("wifi_rails", &on, sizeof(on)) || on == 0) {
		printf("wifi: rails: not asked for (wifi_rails=1): the WCN3990's supplies are left as they are\n");
		return;
	}
	db = (const volatile uint8_t *)ml_io_map_unmappable(R5Q_CMD_DB_PHYS, R5Q_CMD_DB_SIZE, VM_WIMG_WCOMB);
	for (unsigned i = 0; i < 6; i++) {
		base[i] = cmd_db_addr(db, rails[i].id);
		printf("wifi: rails: cmd-db: %s (%s) at %#x\n", rails[i].id, rails[i].what, base[i]);
		if (base[i] == 0 || base[i] > 0xfffff) {
			printf("wifi: rails: %s is not in cmd-db: NOTHING IS ASKED\n", rails[i].id);
			return;
		}
	}
	if (on == 1) {
		printf("wifi: rails: wifi_rails=1: looked up only; wifi_rails=2 sends the requests\n");
		return;
	}
	tcs = ml_io_map(R5Q_RSC_TCS_PHYS, R5Q_RSC_TCS_SIZE);
	printf("wifi: rails: TCS %u status %#x, control %#x before the requests\n", R5Q_RSC_TCS_ID,
	    rd(tcs + RSC_TCS_STRIDE * R5Q_RSC_TCS_ID + 0x18), rd(tcs + RSC_TCS_STRIDE * R5Q_RSC_TCS_ID + 0x14));
	/* voltages and high-power mode first, then the enables (and the clock), as qcom-rpmh-regulator orders them */
	for (unsigned i = 0; i < 4; i++) {
		addr[n] = base[i] + 0x0;
		data[n++] = rails[i].mv;
		addr[n] = base[i] + 0x8;
		data[n++] = 7;                                  /* PMIC4 LDO HPM */
	}
	if (!rsc_write(tcs, addr, data, n)) {
		return;
	}
	n = 0;
	for (unsigned i = 0; i < 6; i++) {
		addr[n] = base[i] + 0x4;                        /* enable (a VRM clock: its enable at +4 too) */
		data[n++] = 1;
	}
	printf("wifi: rails: %s\n", rsc_write(tcs, addr, data, n) ? "THE WCN3990'S FOUR SUPPLIES AND RF_CLK2 AND 3 ARE ASKED FOR "
	    "(752, 1800, 1304, 3104 mV, HPM; RPMh says done)" : "the enables were not all taken");
}

/*
 * r5q (Test 109): the Wi-Fi firmware's service (WLFW, QMI 0x45 v1) taken up as Linux's ath10k qmi.c does for a WCN3990
 * (ath10k_qmi_event_server_arrive and what it calls): indications, host capabilities, the MSA memory's information, the
 * MSA regions given to the modem and the WLAN (TrustZone assign_mem, as ath10k_qmi_map_msa_permission), MSA ready,
 * capabilities; then, on "MSA ready", the board data (bdwlan.bin from the vendor file store) and the calibration
 * report; then "firmware ready" is waited for. The messages are ath10k's (redwood_wlfw_ath10k.h, generated). MSA:
 * the live DT's qcom,wlan-msa-fixed-region, 0x8c200000 + 0x180000 (inside the firmware's "PIL Reserved II": never iOS's).
 * Boot-arg wifi_wlfw=1.
 */
#define REDWOOD_QMI_NO_KERNEL_CONNECT
#include "redwood_wlfw_ath10k.h"
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define R5Q_MSA_PA              0x8c200000ULL
#define R5Q_MSA_SIZE            0x180000u
#define R5Q_WLFW_CLIENT_ID      0x4b4e454c      /* ath10k's ATH10K_QMI_CLIENT_ID */
static struct qmi_handle wlfw10;
static volatile bool wlfw10_server, wlfw10_msa_ready, wlfw10_fw_ready;
static uint64_t wlfw10_began;

static int
wlfw10_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = redwood_qmi_connect(qmi->sock, service->node, service->port);

	printf("wifi: wlfw: the service at node %u port %u%s\n", service->node, service->port, error ? ": NOT CONNECTED" : "");
	wlfw10_server = error == 0;
	return error;
}

static void
wlfw10_fw_ready_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	printf("wifi: wlfw: +%llu ms: THE FIRMWARE SAYS \"READY\"\n", (now_us() - wlfw10_began) / 1000);
	wlfw10_fw_ready = true;
}

static void
wlfw10_msa_ready_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	printf("wifi: wlfw: +%llu ms: the firmware says \"MSA ready\"\n", (now_us() - wlfw10_began) / 1000);
	wlfw10_msa_ready = true;
}

static void
wlfw10_pin_ind(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    const void *decoded)
{
	const struct wlfw_pin_connect_result_ind_msg_v01 *p = decoded;

	printf("wifi: wlfw: pin connect result: power %u/%#x, phy io %u/%#x, rf %u/%#x\n", p->pwr_pin_result_valid,
	    p->pwr_pin_result, p->phy_io_pin_result_valid, p->phy_io_pin_result, p->rf_pin_result_valid, p->rf_pin_result);
}

static const struct qmi_ops wlfw10_ops = { .new_server = wlfw10_new_server };
static const struct qmi_msg_handler wlfw10_handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, wlfw_fw_ready_ind_msg_v01_ei, sizeof(struct wlfw_fw_ready_ind_msg_v01),
	  wlfw10_fw_ready_ind },
	{ QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, wlfw_msa_ready_ind_msg_v01_ei, sizeof(struct wlfw_msa_ready_ind_msg_v01),
	  wlfw10_msa_ready_ind },
	{ QMI_INDICATION, QMI_WLFW_PIN_CONNECT_RESULT_IND_V01, wlfw_pin_connect_result_ind_msg_v01_ei,
	  sizeof(struct wlfw_pin_connect_result_ind_msg_v01), wlfw10_pin_ind },
	{ 0 },
};

/* one request and its answer; the answer's first field is the QMI result. True: success. */
static bool
wlfw10_ask(const char *what, int message, size_t largest, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, bool quiet)
{
	const struct qmi_response_type_v01 *result = resp;
	struct qmi_txn txn;
	uint64_t began = now_us();
	int ret = qmi_txn_init(&wlfw10, &txn, resp_ei, resp);

	if (ret >= 0) {
		ret = qmi_send_request(&wlfw10, NULL, &txn, message, largest, req_ei, req);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = redwood_qmi_txn_wait_ms(&txn, 5000);
		}
	}
	if (ret >= 0 && result->result == QMI_RESULT_SUCCESS_V01) {
		if (!quiet) {
			printf("wifi: wlfw: %s (%#x): success, %llu ms\n", what, message, (now_us() - began) / 1000);
		}
		return true;
	}
	if (ret < 0) {
		printf("wifi: wlfw: %s (%#x): NO ANSWER (%d)\n", what, message, ret);
	} else {
		printf("wifi: wlfw: %s (%#x): REFUSED: result %u, error %u\n", what, message, result->result, result->error);
	}
	return false;
}

/* TrustZone assign_mem of one MSA region, as ath10k_qmi_map_msa_permission: HLOS -> MSS_MSA, WLAN (and WLAN_CE if not secure) */
static int32_t
wlfw10_assign(uint64_t addr, uint32_t size, bool secure)
{
	uint8_t *page = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_ZERO);
	uint64_t *map, *ext, session = 0;
	uint32_t *src, count = secure ? 2 : 3;
	struct r5q_vmperm {
		uint32_t vmid, perm;
		uint64_t ctx;
		uint32_t ctx_size, unused;
	} *dest;
	static const uint32_t vmids[3] = { 15, 0x18, 0x19 };    /* MSS_MSA, WLAN, WLAN_CE */
	uint64_t function = SCM_64 | SCM_SIP | SCM_CALL(0x0c, 0x16);

	if (page == NULL || ((vm_offset_t)page & PAGE_MASK) != 0) {
		printf("wifi: wlfw: no page-aligned memory for TrustZone's arguments\n");
		return -1;
	}
	map = (uint64_t *)page;
	src = (uint32_t *)(page + 64);
	dest = (struct r5q_vmperm *)(page + 128);
	ext = (uint64_t *)(page + 256);
	map[0] = addr;
	map[1] = size;
	src[0] = 3;                             /* HLOS */
	for (uint32_t i = 0; i < count; i++) {
		dest[i].vmid = vmids[i];
		dest[i].perm = 6;               /* read, write */
	}
	ext[0] = 4;                             /* the source list's size */
	ext[1] = kvtophys((vm_offset_t)dest);
	ext[2] = sizeof(*dest) * count;
	ext[3] = 0;
	FlushPoC_DcacheRegion((vm_offset_t)page, PAGE_SIZE);
	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = 0x1117;
		register uint64_t x2 __asm__("x2") = kvtophys((vm_offset_t)map);
		register uint64_t x3 __asm__("x3") = 16;
		register uint64_t x4 __asm__("x4") = kvtophys((vm_offset_t)src);
		register uint64_t x5 __asm__("x5") = kvtophys((vm_offset_t)ext);
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("wifi: wlfw: TrustZone assign_mem %#llx + %#x to MSS%s: %d (answer %#llx)\n", addr, size,
			    secure ? " + WLAN" : " + WLAN + WLAN_CE", (int32_t)x0, x1);
			kfree_data(page, PAGE_SIZE);
			return (int32_t)x0 != 0 ? (int32_t)x0 : (int32_t)x1;
		}
	}
	kfree_data(page, PAGE_SIZE);
	return -1;
}

/*
 * (Test 113) One region of the kernel's memory shared by TrustZone with the WLAN and its copy engines (HLOS keeps it):
 * assign_mem HLOS -> HLOS + WLAN 0x18 + WLAN_CE 0x19, read-write; as rmtfs's memory is shared with the modem.
 */
static int32_t
r5q_share_wlan(uint64_t addr, uint32_t size)
{
	uint8_t *page = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_ZERO);
	uint64_t *map, *ext, session = 0;
	uint32_t *src;
	struct r5q_vmperm2 {
		uint32_t vmid, perm;
		uint64_t ctx;
		uint32_t ctx_size, unused;
	} *dest;
	static const uint32_t vmids[3] = { 3, 0x18, 0x19 };     /* HLOS, WLAN, WLAN_CE */
	uint64_t function = SCM_64 | SCM_SIP | SCM_CALL(0x0c, 0x16);

	if (page == NULL || ((vm_offset_t)page & PAGE_MASK) != 0) {
		return -1;
	}
	map = (uint64_t *)page;
	src = (uint32_t *)(page + 64);
	dest = (struct r5q_vmperm2 *)(page + 128);
	ext = (uint64_t *)(page + 256);
	map[0] = addr;
	map[1] = size;
	src[0] = 3;
	for (uint32_t i = 0; i < 3; i++) {
		dest[i].vmid = vmids[i];
		dest[i].perm = 6;
	}
	ext[0] = 4;
	ext[1] = kvtophys((vm_offset_t)dest);
	ext[2] = sizeof(*dest) * 3;
	ext[3] = 0;
	FlushPoC_DcacheRegion((vm_offset_t)page, PAGE_SIZE);
	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = 0x1117;
		register uint64_t x2 __asm__("x2") = kvtophys((vm_offset_t)map);
		register uint64_t x3 __asm__("x3") = 16;
		register uint64_t x4 __asm__("x4") = kvtophys((vm_offset_t)src);
		register uint64_t x5 __asm__("x5") = kvtophys((vm_offset_t)ext);
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			kfree_data(page, PAGE_SIZE);
			return (int32_t)x0 != 0 ? (int32_t)x0 : (int32_t)x1;
		}
	}
	kfree_data(page, PAGE_SIZE);
	return -1;
}

/* every 16 KB page of a stretch of the kernel's memory, shared so; the number refused */
static uint32_t
r5q_share_pages(const void *va, size_t bytes)
{
	uint32_t refused = 0;

	for (size_t at = 0; at < bytes; at += PAGE_SIZE) {
		int32_t r = r5q_share_wlan(kvtophys((vm_offset_t)va + at), PAGE_SIZE);

		if (r != 0) {
			if (refused++ == 0) {
				printf("wifi: ce: TrustZone refuses to share %#llx with the WLAN: %d\n", kvtophys((vm_offset_t)va + at), r);
			}
		}
	}
	return refused;
}

static void
wlfw10_board(void)
{
	static struct wlfw_bdf_download_req_msg_v01 req;
	struct wlfw_bdf_download_resp_msg_v01 resp;
	struct wlfw_cal_report_req_msg_v01 cal;
	struct wlfw_cal_report_resp_msg_v01 cal_resp;
	uint32_t size = 0, left, pieces = 0;
	const uint8_t *data = store_load("wlan/qca_cld/bdwlan.bin", &size);

	if (data == NULL || size == 0) {
		printf("wifi: wlfw: no bdwlan.bin in the vendor store: board data not sent\n");
		return;
	}
	for (left = size; left > 0; pieces++) {
		bzero(&req, sizeof(req));
		req.valid = 1;
		req.file_id_valid = 1;
		req.file_id = 0;
		req.total_size_valid = 1;
		req.total_size = size;
		req.seg_id_valid = 1;
		req.seg_id = pieces;
		req.data_valid = 1;
		req.end_valid = 1;
		req.data_len = left > QMI_WLFW_MAX_DATA_SIZE_V01 ? QMI_WLFW_MAX_DATA_SIZE_V01 : left;
		req.end = left <= QMI_WLFW_MAX_DATA_SIZE_V01;
		memcpy(req.data, data + (size - left), req.data_len);
		bzero(&resp, sizeof(resp));
		if (!wlfw10_ask("board data", QMI_WLFW_BDF_DOWNLOAD_REQ_V01, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_bdf_download_req_msg_v01_ei, &req, wlfw_bdf_download_resp_msg_v01_ei, &resp, true) &&
		    !(req.end && resp.resp.result == QMI_ERR_MALFORMED_MSG_V01)) {
			printf("wifi: wlfw: board data refused at piece %u\n", pieces);
			return;
		}
		left -= req.data_len;
	}
	printf("wifi: wlfw: board data bdwlan.bin (%u bytes) sent in %u pieces\n", size, pieces);
	bzero(&cal, sizeof(cal));
	bzero(&cal_resp, sizeof(cal_resp));
	wlfw10_ask("calibration report", QMI_WLFW_CAL_REPORT_REQ_V01, WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cal_report_req_msg_v01_ei, &cal, wlfw_cal_report_resp_msg_v01_ei, &cal_resp, false);
}

/*
 * r5q (Test 110): ath10k_snoc_wlan_enable -> ath10k_qmi_wlan_enable: the configuration (the copy engines' pipes as the
 * target sees them, the map of HTC services to pipes with its zero row, the shadow registers: snoc.c's tables, as
 * numbers in redwood_wlfw_ath10k_ce.h), then the mode "mission" (hw_debug 0), after which the firmware has the radio
 * on. The host's own copy engines (ath10k_snoc_init_pipes) are NOT set up yet: that is the next step.
 */
#include "redwood_wlfw_ath10k_ce.h"

static void
r5q_wlfw_enable(void)
{
	static struct wlfw_wlan_cfg_req_msg_v01 cfg;
	struct wlfw_wlan_cfg_resp_msg_v01 cfg_resp;
	struct wlfw_wlan_mode_req_msg_v01 mode;
	struct wlfw_wlan_mode_resp_msg_v01 mode_resp;
	uint32_t n;

	bzero(&cfg, sizeof(cfg));
	cfg.tgt_cfg_valid = 1;
	n = sizeof(r5q_ce_tgt) / sizeof(r5q_ce_tgt[0]);
	cfg.tgt_cfg_len = n < QMI_WLFW_MAX_NUM_CE_V01 ? n : QMI_WLFW_MAX_NUM_CE_V01;
	for (uint32_t i = 0; i < cfg.tgt_cfg_len; i++) {
		cfg.tgt_cfg[i].pipe_num = r5q_ce_tgt[i][0];
		cfg.tgt_cfg[i].pipe_dir = r5q_ce_tgt[i][1];
		cfg.tgt_cfg[i].nentries = r5q_ce_tgt[i][2];
		cfg.tgt_cfg[i].nbytes_max = r5q_ce_tgt[i][3];
		cfg.tgt_cfg[i].flags = r5q_ce_tgt[i][4];
	}
	cfg.svc_cfg_valid = 1;
	n = sizeof(r5q_ce_svc) / sizeof(r5q_ce_svc[0]);
	cfg.svc_cfg_len = n < QMI_WLFW_MAX_NUM_SVC_V01 ? n : QMI_WLFW_MAX_NUM_SVC_V01;
	for (uint32_t i = 0; i < cfg.svc_cfg_len; i++) {
		cfg.svc_cfg[i].service_id = r5q_ce_svc[i][0];
		cfg.svc_cfg[i].pipe_dir = r5q_ce_svc[i][1];
		cfg.svc_cfg[i].pipe_num = r5q_ce_svc[i][2];
	}
	cfg.shadow_reg_valid = 1;
	n = sizeof(r5q_ce_shadow) / sizeof(r5q_ce_shadow[0]);
	cfg.shadow_reg_len = n < QMI_WLFW_MAX_NUM_SHADOW_REG_V01 ? n : QMI_WLFW_MAX_NUM_SHADOW_REG_V01;
	for (uint32_t i = 0; i < cfg.shadow_reg_len; i++) {
		cfg.shadow_reg[i].id = r5q_ce_shadow[i][0];
		cfg.shadow_reg[i].offset = r5q_ce_shadow[i][1];
	}
	bzero(&cfg_resp, sizeof(cfg_resp));
	if (!wlfw10_ask("WLAN configuration", QMI_WLFW_WLAN_CFG_REQ_V01, WLFW_WLAN_CFG_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_cfg_req_msg_v01_ei, &cfg, wlfw_wlan_cfg_resp_msg_v01_ei, &cfg_resp, false)) {
		return;
	}
	bzero(&mode, sizeof(mode));
	mode.mode = QMI_WLFW_MISSION_V01;
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	bzero(&mode_resp, sizeof(mode_resp));
	if (wlfw10_ask("WLAN mode \"mission\"", QMI_WLFW_WLAN_MODE_REQ_V01, WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_mode_req_msg_v01_ei, &mode, wlfw_wlan_mode_resp_msg_v01_ei, &mode_resp, false)) {
		printf("wifi: wlfw: +%llu ms: THE RADIO IS ON (mission mode); next: the host's copy engines at 0x18800000\n",
		    (now_us() - wlfw10_began) / 1000);
	}
}

/*
 * r5q (Test 111): the host's side of the copy engines and HTC, a stand-in for ath10k's snoc.c/ce.c/htc.c (WCN3990,
 * 64-bit descriptors, read indices in host memory "RRI", source write indices through shadow registers). The WLAN
 * block's registers (live DT membase 0x18800000) are touched only after mission mode. Rings and buffers are 16 KB pages
 * of the kernel (physically contiguous within one), cleaned/invalidated by hand around the engines' DMA; the engines see
 * physical addresses (the apps SMMU is the boot chain's: qcom,skip-init; SID 0x640 is stage-1 bypass in the DT).
 * Polled, no interrupts. CE0 HTC control out, CE3 WMI out, CE2 HTC/WMI in, CE1 HTT in (pipes as snoc.c's maps).
 * Boot-arg wifi_wlfw=3. Wanted: HTC READY, the services connected, and WMI's first events (service ready).
 */
#define R5Q_WLAN_PHYS           0x18800000UL
#define R5Q_WLAN_CE_OFF         0x240000UL      /* the copy engines: CE n at + n * 0x1000, the wrapper at + 0xc000 */
#define R5Q_WLAN_SHADOW_OFF     0x32000UL       /* the shadow registers */
#define CE_REG(n, r)            (0x1000u * (n) + (r))
#define CE_SR_BASE_LO           0x00
#define CE_SR_BASE_HI           0x04
#define CE_SR_SIZE              0x08
#define CE_DR_BASE_LO           0x0c
#define CE_DR_BASE_HI           0x10
#define CE_DR_SIZE              0x14
#define CE_CTRL1                0x18
#define CE_MISC_IS              0x38
#define CE_SR_WR                0x3c
#define CE_DST_WR               0x40
#define CE_CUR_SRRI             0x44
#define CE_CUR_DRRI             0x48
#define CE_SRC_WM               0x4c
#define CE_DST_WM               0x50
#define CE_RRI_LOW              0xc004u
#define CE_RRI_HIGH             0xc008u
#define CE_BUF                  2048u

struct r5q_ce {
	uint32_t id, n, sw, wr;
	bool dest;
	uint8_t *ring;                  /* descriptors: 16 bytes each (addr 8, nbytes 2, flags 2, hash 4) */
	uint64_t ring_pa;
	uint8_t *bufs;                  /* n buffers of CE_BUF, slot k at k * CE_BUF */
	uint64_t bufs_pa[8];            /* the physical address of each 16 KB page of them */
};
static uint8_t *wlan_rri;
static vm_offset_t smmu_regs;           /* (Test 113: the apps SMMU, for its fault registers) */
static uint32_t smmu_cb = ~0u;          /* the context bank given to SID 0x640 */
static vm_offset_t wlan_regs, wlan_shadow;   /* (Test 111b: two small windows; 4 MB of I/O map panicked) */
static struct r5q_ce ce0, ce1, ce2, ce3, ce4;
static uint8_t htc_seq;

static uint8_t *
r5q_dma_page(uint64_t *pa, size_t bytes)
{
	uint8_t *p = kalloc_data(bytes, Z_WAITOK | Z_ZERO);

	if (p == NULL || ((vm_offset_t)p & PAGE_MASK) != 0) {
		return NULL;
	}
	for (size_t at = PAGE_SIZE; at < bytes; at += PAGE_SIZE) {
		if (kvtophys((vm_offset_t)p + at) != kvtophys((vm_offset_t)p) + at) {
			printf("wifi: ce: %zu bytes not contiguous at +%zu\n", bytes, at);
		}
	}
	*pa = kvtophys((vm_offset_t)p);
	FlushPoC_DcacheRegion((vm_offset_t)p, bytes);
	return p;
}

static uint64_t
ce_buf_pa(struct r5q_ce *c, uint32_t slot)
{
	uint64_t off = (uint64_t)slot * CE_BUF;

	return kvtophys((vm_offset_t)c->bufs + off);
}

static bool
ce_setup(struct r5q_ce *c, uint32_t id, uint32_t n, bool dest)
{
	uint32_t mask = n - 1, v;
	uint64_t pa;

	bool again = c->ring != NULL && c->bufs != NULL && c->n == n;  /* (Test 172) after the domain restarted */

	c->id = id;
	c->n = n;
	c->dest = dest;
	if (again) {
		bzero(c->ring, PAGE_SIZE);
	} else if ((c->ring = r5q_dma_page(&c->ring_pa, PAGE_SIZE)) == NULL ||
	    (c->bufs = kalloc_data((size_t)n * CE_BUF, Z_WAITOK | Z_ZERO)) == NULL) {
		return false;
	}
	if (((vm_offset_t)c->bufs & (CE_BUF - 1)) != 0) {
		printf("wifi: ce: CE%u's buffers are not %u-aligned: some would cross a page\n", id, CE_BUF);
		return false;
	}
	FlushPoC_DcacheRegion((vm_offset_t)c->bufs, (size_t)n * CE_BUF);
	pa = c->ring_pa;
	if (dest) {
		/*
		 * (Test 123) A receive ring is written by both sides: the engine fills in each descriptor's length, we give the
		 * slot back. Four 16-byte descriptors share a 64-byte cache line, and cleaning our copy of a line put stale
		 * lengths back over ones the engine had just written: a slot was missed and the ring stalled (Test 122, under
		 * real traffic: CE1 stopped, then beacon misses). As ath10k's coherent rings: through an uncached
		 * (write-combining) mapping of the same page; the cached one is not touched again.
		 */
		vm_offset_t nc = again ? 0 : ml_io_map_wcomb(pa, PAGE_SIZE);   /* (again: c->ring is that mapping) */

		if (nc != 0) {
			c->ring = (uint8_t *)nc;
		}
	}
	if (dest) {
		c->sw = rd(wlan_regs + CE_REG(id, CE_CUR_DRRI)) & mask;
		c->wr = rd(wlan_regs + CE_REG(id, CE_DST_WR)) & mask;
		wr(wlan_regs + CE_REG(id, CE_DR_BASE_LO), (uint32_t)pa);
		v = rd(wlan_regs + CE_REG(id, CE_DR_BASE_HI));
		wr(wlan_regs + CE_REG(id, CE_DR_BASE_HI), (v & ~0x1fu) | ((uint32_t)(pa >> 32) & 0x1f));
		wr(wlan_regs + CE_REG(id, CE_DR_SIZE), n);
		v = rd(wlan_regs + CE_REG(id, CE_CTRL1));
		wr(wlan_regs + CE_REG(id, CE_CTRL1), v & ~(1u << 18));                 /* no byte swap */
		v = rd(wlan_regs + CE_REG(id, CE_DST_WM));
		wr(wlan_regs + CE_REG(id, CE_DST_WM), (v & ~0xffffffffu) | (0u << 16) | (n & 0xffff));
	} else {
		c->sw = rd(wlan_regs + CE_REG(id, CE_CUR_SRRI)) & mask;
		c->wr = rd(wlan_regs + CE_REG(id, CE_SR_WR)) & mask;
		wr(wlan_regs + CE_REG(id, CE_SR_BASE_LO), (uint32_t)pa);
		wr(wlan_regs + CE_REG(id, CE_SR_BASE_HI), (uint32_t)(pa >> 32) & 0x1f);
		wr(wlan_regs + CE_REG(id, CE_SR_SIZE), n);
		v = rd(wlan_regs + CE_REG(id, CE_CTRL1));
		wr(wlan_regs + CE_REG(id, CE_CTRL1), (v & ~0xffffu & ~(1u << 17)) | CE_BUF);   /* dmax, no byte swap */
		v = rd(wlan_regs + CE_REG(id, CE_SRC_WM));
		wr(wlan_regs + CE_REG(id, CE_SRC_WM), (0u << 16) | (n & 0xffff));
	}
	printf("wifi: ce: CE%u %s ring of %u at %#llx (indices sw %u wr %u), ctrl1 %#x\n", id, dest ? "receive" : "send", n, pa,
	    c->sw, c->wr, rd(wlan_regs + CE_REG(id, CE_CTRL1)));
	return true;
}

/* every slot but one gets its buffer, as ath10k_snoc_rx_post leaves one free */
static void
ce_post_all(struct r5q_ce *c)
{
	uint32_t mask = c->n - 1;

	for (uint32_t k = 0; k + 1 < c->n; k++) {
		uint8_t *d = c->ring + 16 * c->wr;
		uint64_t pa = ce_buf_pa(c, c->wr) & 0x7ffffffffULL;

		memcpy(d, &pa, 8);
		bzero(d + 8, 8);
		c->wr = (c->wr + 1) & mask;
	}
	FlushPoC_DcacheRegion((vm_offset_t)c->ring, 16 * c->n);
	wr(wlan_regs + CE_REG(c->id, CE_DST_WR), c->wr);
}

/* the next received buffer, or NULL; *len its length. The slot is given back to the engine at once. */
static const uint8_t *
ce_recv(struct r5q_ce *c, uint32_t *len)
{
	uint8_t *d = c->ring + 16 * c->sw;
	uint16_t nbytes;
	uint32_t slot = c->sw, mask = c->n - 1;
	uint64_t pa;

	FlushPoC_DcacheRegion((vm_offset_t)d, 16);
	memcpy(&nbytes, d + 8, 2);
	if (nbytes == 0) {
		return NULL;
	}
	FlushPoC_DcacheRegion((vm_offset_t)c->bufs + (size_t)slot * CE_BUF, CE_BUF);
	*len = nbytes;
	bzero(d + 8, 2);
	c->sw = (c->sw + 1) & mask;
	/* the same buffer goes back at the write index (which trails the read index by one) */
	d = c->ring + 16 * c->wr;
	pa = ce_buf_pa(c, c->wr) & 0x7ffffffffULL;
	memcpy(d, &pa, 8);
	bzero(d + 8, 8);
	FlushPoC_DcacheRegion((vm_offset_t)c->ring, 16 * c->n);
	c->wr = (c->wr + 1) & mask;
	wr(wlan_regs + CE_REG(c->id, CE_DST_WR), c->wr);
	return c->bufs + (size_t)slot * CE_BUF;
}

/* one HTC frame out on a send ring: the header (endpoint, length, sequence) and the payload */
static void
ce_send_htc(struct r5q_ce *c, uint32_t shadow, uint8_t eid, const void *payload, uint16_t len)
{
	uint32_t slot = c->wr, mask = c->n - 1;
	uint8_t *buf = c->bufs + (size_t)slot * CE_BUF, *d = c->ring + 16 * slot;
	uint64_t pa = ce_buf_pa(c, slot);
	uint32_t lo = (uint32_t)pa, hi = (uint32_t)(pa >> 32) & 0x1f;
	uint16_t total = (uint16_t)(8 + len), meta = (uint16_t)((eid << 4) & 0xfff0);

	bzero(buf, 8);
	buf[0] = eid;
	buf[2] = (uint8_t)len;
	buf[3] = (uint8_t)(len >> 8);
	buf[5] = htc_seq++;
	memcpy(buf + 8, payload, len);
	FlushPoC_DcacheRegion((vm_offset_t)buf, total);
	memcpy(d, &lo, 4);
	memcpy(d + 4, &hi, 4);
	memcpy(d + 8, &total, 2);
	memcpy(d + 10, &meta, 2);
	bzero(d + 12, 4);
	FlushPoC_DcacheRegion((vm_offset_t)d, 16);
	c->wr = (c->wr + 1) & mask;
	wr(wlan_shadow + shadow, c->wr);
}

/* a line for the log, and time for the PC's live follow to fetch it before a step that might stop the machine */
static void
ce_mark(const char *what)
{
	printf("wifi: ce: %s\n", what);
	{
		static int slow = -1;

		if (slow < 0) {
			slow = 0;
			PE_parse_boot_argn("wifi_slow", &slow, sizeof(slow));
		}
		if (slow) {                     /* (Test 125: only on request; the bring-up is known by now) */
			delay_for_interval(4000, NSEC_PER_MSEC);
		}
	}
}

/*
 * (Test 112) The apps SMMU (0x15000000, the boot chain's: qcom,skip-init) faults streams it has no match for (sCR0
 * USFCFG), and has none for the WLAN's SID 0x640: its copy engines' first DMA reset the phone. As Linux's
 * arm-smmu-qcom does where the hypervisor refuses S2CR "bypass" (SDM845 and kin): a context bank with translation off
 * (SCTLR.M 0, CBAR "stage 1, stage 2 bypass") and a stream match for 0x640 (mask 1) whose S2CR translates through it --
 * the WLAN then sees physical addresses, as Android's icnss has it (qcom,smmu-s1-bypass). wifi_smmu=1 looks only;
 * wifi_smmu=2 also writes, choosing a context bank that no valid stream match's S2CR names, and a free SMR.
 */
static bool
r5q_smmu_look(bool configure)
{
	vm_offset_t s = smmu_regs = ml_io_map(0x15000000UL, 0x100000);
	uint32_t scr0 = rd(s), idr0 = rd(s + 0x20), idr1 = rd(s + 0x24), n = idr0 & 0xff, found = 0;
	uint32_t numcb = idr1 & 0xff, page = (idr1 >> 31) ? 0x10000u : 0x1000u, numpage = 1u << (((idr1 >> 28) & 7) + 1);
	uint32_t free_smr = ~0u, cb = ~0u;
	uint8_t used[256];

	bzero(used, sizeof(used));
	printf("wifi: smmu: sCR0 %#x (client bypass %u, unmatched streams %s), %u stream matches, %u context banks, pages of %#x, "
	    "%u global pages\n", scr0, scr0 & 1, (scr0 >> 10) & 1 ? "FAULT" : "bypass", n, numcb, page, numpage);
	for (uint32_t i = 0; i < n && i < 128; i++) {
		uint32_t smr = rd(s + 0x800 + 4 * i), id = smr & 0x7fff, m = (smr >> 16) & 0x7fff, s2cr = rd(s + 0xc00 + 4 * i);

		if (!(smr & 0x80000000u)) {
			if (free_smr == ~0u) {
				free_smr = i;
			}
			continue;
		}
		if (((s2cr >> 16) & 3) == 0) {
			used[s2cr & 0xff] = 1;
		}
		if ((0x640u & ~m) == (id & ~m)) {
			printf("wifi: smmu: SMR%u %#x matches SID 0x640: S2CR %#x\n", i, smr, s2cr);
			found++;
		}
	}
	if ((uint64_t)numpage * page + (uint64_t)numcb * page > 0x100000) {
		printf("wifi: smmu: the context banks lie past the 1 MB mapped: not looked at\n");
		return false;
	}
	for (uint32_t k = numcb; k-- > 0;) {
		vm_offset_t c = s + (vm_offset_t)numpage * page + (vm_offset_t)k * page;

		(void)c;
		if (!used[k]) {         /* no stream names it (the unused banks hold reset junk in SCTLR): the last such, as Linux */
			cb = k;
			break;
		}
	}
	if (found == 0) {
		printf("wifi: smmu: no stream match for SID 0x640; a free SMR: %u, an unused context bank: %u (CBAR %#x)\n",
		    free_smr, cb, cb != ~0u ? rd(s + page + 4 * cb) : 0);
	}
	for (uint32_t k = 0; k < numcb; k += 8) {
		char line[200];
		int at = 0;

		for (uint32_t j = k; j < k + 8 && j < numcb; j++) {
			vm_offset_t c = s + (vm_offset_t)numpage * page + (vm_offset_t)j * page;

			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u:%x/%x%s", j, rd(c), rd(s + page + 4 * j),
			    used[j] ? "*" : "");
		}
		printf("wifi: smmu: CB sctlr/cbar (* named by a stream):%s\n", line);
	}
	if (found != 0) {
		return true;
	}
	if (!configure || free_smr == ~0u) {
		return false;
	}
	/*
	 * (Test 114) A bank of our own faulted at stage 2 (CB55 FSR 0x2 at a receive buffer): the hypervisor set nothing up
	 * behind it. The boot chain's banks with translation off (SCTLR.M 0, CBAR "S1, S2 bypass"), which other streams'
	 * DMA goes through every moment, are shared instead: SID 0x640's S2CR is made a copy of one that names such a bank.
	 */
	for (uint32_t i = 0; i < n && i < 128; i++) {
		uint32_t smr = rd(s + 0x800 + 4 * i), s2cr = rd(s + 0xc00 + 4 * i), k = s2cr & 0xff;
		vm_offset_t c = s + (vm_offset_t)numpage * page + (vm_offset_t)k * page;

		if ((smr & 0x80000000u) && ((s2cr >> 16) & 3) == 0 && k < numcb && !(rd(c) & 1) && rd(s + page + 4 * k) == 0x1f000u) {
			smmu_cb = k;
			ce_mark("about to give SID 0x640 a stream match through a boot bank with translation off");
			wr(s + 0xc00 + 4 * free_smr, s2cr);                             /* S2CR: as SMR i's, through bank k */
			wr(s + 0x800 + 4 * free_smr, 0x80000000u | (1u << 16) | 0x640u);
			__asm__ volatile ("dsb sy" ::: "memory");
			printf("wifi: smmu: SMR%u now %#x, S2CR %#x (a copy of SMR%u %#x's); bank %u SCTLR %#x CBAR %#x\n", free_smr,
			    rd(s + 0x800 + 4 * free_smr), rd(s + 0xc00 + 4 * free_smr), i, smr, k, rd(c), rd(s + page + 4 * k));
			return rd(s + 0x800 + 4 * free_smr) == (0x80000000u | (1u << 16) | 0x640u);
		}
	}
	printf("wifi: smmu: no boot bank with translation off to share\n");
	if (cb == ~0u) {
		return false;
	}
	smmu_cb = cb;
	ce_mark("about to give SID 0x640 a stream match through a context bank with translation off");
	wr(s + (vm_offset_t)numpage * page + (vm_offset_t)cb * page, 0);         /* CB SCTLR: M 0 */
	wr(s + page + 4 * cb, 0x1f000u);        /* GR1 CBAR: S1 translation, S2 bypass -- as the boot chain's CB0-3 have it */
	wr(s + 0xc00 + 4 * free_smr, cb);                                       /* S2CR: translate, through cb */
	wr(s + 0x800 + 4 * free_smr, 0x80000000u | (1u << 16) | 0x640u);       /* SMR: valid, mask 1, id 0x640 */
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("wifi: smmu: SMR%u now %#x, S2CR %#x; CB%u CBAR %#x SCTLR %#x\n", free_smr, rd(s + 0x800 + 4 * free_smr),
	    rd(s + 0xc00 + 4 * free_smr), cb, rd(s + page + 4 * cb), rd(s + (vm_offset_t)numpage * page + (vm_offset_t)cb * page));
	return rd(s + 0x800 + 4 * free_smr) == (0x80000000u | (1u << 16) | 0x640u);
}

static void
htc_show(const char *what, const uint8_t *p, uint32_t len)
{
	char line[3 * 40 + 1];
	size_t k = 0;

	for (uint32_t i = 0; i < len && i < 40; i++) {
		k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[i]);
	}
	line[k] = 0;
	printf("wifi: htc: %s (%u bytes): %s\n", what, len, line);
}

/*
 * (Test 115) WMI, the TLV kind (ath10k wmi-tlv.c): a command is a 4-byte header (its id) and TLVs (le16 length, le16
 * tag, the value), sent as an HTC frame on WMI's endpoint through CE3 (shadow register 0x3200c). On SERVICE_READY:
 * WMI_INIT (0x1) with the resource configuration ath10k_wmi_tlv_op_gen_init gives a WCN3990 (TARGET_TLV / _HL_TLV
 * values, native-wifi decap) and no host memory chunks; READY (0x2) then carries the MAC address.
 */
static bool wmi_init_sent;

/*
 * (Test 116) After READY, a scan as ath10k's hw_scan makes one: SCAN_CHAN_LIST (0x3003), a station vdev (VDEV_CREATE
 * 0x5001, vdev 0, type STA 2, a locally administered address of our own -- READY's is all zeros, the host gives one),
 * then a passive START_SCAN (0x3001) over every channel. WMI has HTC transmit credits (READY's count, given to it at
 * connect): one per command, returned in received frames' trailers (flags 0x2: records {id 1, len, {eid, credits}..}).
 * Scan events (0x3001) and the beacons / probe responses the firmware passes up as WMI management rx (0x7001) are logged,
 * each network once.
 */
static uint8_t wmi_step;                 /* 1 HTT version, 2 HTT rx ring, 3 channel list, 4 vdev, 5 scan, 6 sent */
static uint8_t htt_ep = 0xff;
static bool htt_ver_ok;
static uint8_t join_step;        /* 10 vdev start, 11 wait, 12 peer, 13 wait, 14 auth, 15 wait, 16 assoc, 17 wait, 18 peer assoc, 19 up, 20 joined,
                                  * 21 PTK, 22 GTK, 23 authorize, 24 connected, 30 failed */
static bool join_ev;             /* the event the step waits for came */
static void r5q_join_next(uint8_t eid);
static void r5q_eapol_rx(const uint8_t *e, uint32_t len);
static uint8_t *dtx_page;        /* (Test 119) the fragment descriptor bank (32 x 72 bytes) */
static uint64_t dtx_pa;
static uint32_t dtx_slot, rx_data;
/* (Test 120) 32 frames in flight at most, each slot free again at its TX_COMPL (or after 2 s) */
#define R5Q_TXN 128u                     /* (Test 125: 32 dropped ~600 frames a minute in bursts) */
#define TX_BUSY(k)      ((dtx_busy[(k) / 32] >> ((k) % 32)) & 1u)
#define TX_SET(k)       (dtx_busy[(k) / 32] |= 1u << ((k) % 32))
#define TX_CLEAR(k)     (dtx_busy[(k) / 32] &= ~(1u << ((k) % 32)))
static uint8_t *dtx_frames;
static uint64_t dtx_fpa[R5Q_TXN], dtx_sent_us[R5Q_TXN];
static uint32_t dtx_busy[R5Q_TXN / 32], tx_drops, tx_compl, tx_compl_bad, tx_sent;
static bool r5q_link_ready, r5q_keyed;     /* the net thread services the rings; the pairwise key is in */
static uint8_t svc_wmi_eid = 0xff, svc_htt_eid = 0xff, svc_credits;
static bool svc_ready;
static uint64_t svc_t0, wpa_rekey_us;
static uint32_t rx_to_net;
static void r5q_rx_to_net(const uint8_t *f, uint32_t ml);
static void r5q_en0_address(char *out, size_t size);
static void r5q_flows_say(void);
/* (Test 122) the station side for iOS's Wi-Fi (sys/redwood_sta.h) */
static bool scan_running, r5q_sta_attached;
static int join_error;
#define R5Q_HEARD 64u
static struct redwood_sta_bss *heard_tab;
static void r5q_heard_add(const uint8_t *f, uint32_t flen, int rssi, uint32_t rx_chan);
static void r5q_install_key(uint8_t eid, uint32_t idx, uint32_t flags, const uint8_t *key, uint32_t klen);

/*
 * (Test 117) Test 116's scan visited every channel and received nothing: on this "LL" design the MAC DMAs every frame
 * it receives (beacons too) into host buffers named by HTT's rx ring, and the firmware only then turns management
 * frames into WMI events -- no ring, no frames. ath10k_htt_setup after READY: VERSION_REQ (H2T 0) on HTT's endpoint
 * (pipe CE4, shadow 0x10), VERSION_CONF (T2H 0) back on CE1, then RX_RING_CFG (H2T 2) with one 64-bit ring: the alloc
 * index (in host memory, which the firmware reads), the ring of buffer addresses, 2048-byte buffers, every content flag
 * and the WCN3990 descriptor offsets (struct htt_rx_desc_v2, 360 bytes, in words: header 74, payload 90, ppdu start 27,
 * ppdu end 37, mpdu start 4, mpdu end 26, msdu start 7, msdu end 12, attention 1, frag info 2). Buffers the firmware
 * hands back (RX_IN_ORD_PADDR_IND, T2H 0x12) go straight back on the ring. (FRAG_DESC_BANK_CFG is for transmit: later.)
 */
#define R5Q_RXR 256u
static uint64_t *rxr_ring;
static uint32_t *rxr_idx;
static uint8_t *rxr_bufs;
static uint64_t rxr_pa[R5Q_RXR], rxr_ring_pa, rxr_idx_pa;
static uint32_t rxr_alloc, htt_inord, htt_msdus, htt_other, htt_lost;

static bool
r5q_rx_ring_alloc(void)
{
	static uint64_t pa;
	static uint8_t *page;

	if (page == NULL || rxr_bufs == NULL) {         /* (Test 172: again after a restart, the same memory) */
		page = r5q_dma_page(&pa, PAGE_SIZE);
		if (page == NULL || (rxr_bufs = kalloc_data((size_t)R5Q_RXR * 2048, Z_WAITOK | Z_ZERO)) == NULL ||
		    ((vm_offset_t)rxr_bufs & 2047) != 0) {
			printf("wifi: htt: no memory for the rx ring\n");
			return false;
		}
	}
	bzero(page, PAGE_SIZE);
	if (heard_tab == NULL) {
		heard_tab = kalloc_data(sizeof(struct redwood_sta_bss) * R5Q_HEARD, Z_WAITOK | Z_ZERO);
	}
	rxr_ring = (uint64_t *)(void *)page;
	rxr_idx = (uint32_t *)(void *)(page + R5Q_RXR * 8);
	rxr_ring_pa = pa;
	rxr_idx_pa = pa + R5Q_RXR * 8;
	for (uint32_t i = 0; i < R5Q_RXR; i++) {
		rxr_pa[i] = kvtophys((vm_offset_t)rxr_bufs + (vm_offset_t)i * 2048);
	}
	FlushPoC_DcacheRegion((vm_offset_t)rxr_bufs, (size_t)R5Q_RXR * 2048);
	for (rxr_alloc = 0; rxr_alloc < R5Q_RXR - 1; rxr_alloc++) {    /* fill level: all but one */
		rxr_ring[rxr_alloc] = rxr_pa[rxr_alloc];
	}
	*rxr_idx = rxr_alloc;
	FlushPoC_DcacheRegion((vm_offset_t)page, R5Q_RXR * 8 + 64);
	return true;
}

/* a buffer the firmware is done with goes back on the ring, its attention word cleared */
static void
r5q_rx_recycle(uint64_t pa)
{
	uint32_t i;

	for (i = 0; i < R5Q_RXR && rxr_pa[i] != pa; i++) {
	}
	if (i == R5Q_RXR) {
		htt_lost++;
		return;
	}
	bzero(rxr_bufs + (size_t)i * 2048, 64);
	FlushPoC_DcacheRegion((vm_offset_t)rxr_bufs + (vm_offset_t)i * 2048, 64);
	rxr_ring[rxr_alloc] = pa;
	FlushPoC_DcacheRegion((vm_offset_t)&rxr_ring[rxr_alloc], 8);
	rxr_alloc = (rxr_alloc + 1) & (R5Q_RXR - 1);
	*rxr_idx = rxr_alloc;
	FlushPoC_DcacheRegion((vm_offset_t)rxr_idx, 4);
}

static void
r5q_htt_msg(const uint8_t *p, uint16_t pl)
{
	const uint8_t *m = p + 8;
	uint8_t type = m[0];

	if (type == 0x0) {
		htt_ver_ok = true;
		printf("wifi: htt: VERSION_CONF: %u.%u\n", m[2], m[1]);
	} else if (type == 0x12 && pl >= 8) {
		uint16_t n = (uint16_t)(m[6] | m[7] << 8);

		htt_inord++;
		if (htt_inord <= 3) {
			printf("wifi: htt: RX_IN_ORD_PADDR_IND: %u buffers, peer %u, vdev %u\n", n, m[2] | m[3] << 8, m[4]);
		}
		for (uint32_t k = 0; k < n && 8u + 12u * (k + 1) <= pl; k++) {
			uint64_t pa;
			uint16_t ml;
			uint32_t i;

			memcpy(&pa, m + 8 + 12 * k, 8);
			memcpy(&ml, m + 8 + 12 * k + 8, 2);
			htt_msdus++;
			for (i = 0; i < R5Q_RXR && rxr_pa[i] != pa; i++) {
			}
			if (i < R5Q_RXR && ml <= 2048 - 360) {
				/* (Test 119) the frame: after the 360-byte WCN3990 rx descriptor, native-wifi decapsulated */
				const uint8_t *b = rxr_bufs + (size_t)i * 2048, *f = b + 360;
				static const uint8_t snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
				bool eapol = false;

				FlushPoC_DcacheRegion((vm_offset_t)b, 2048);
				for (uint32_t at = 0; at + 8 <= ml && at < 64; at++) {
					if (memcmp(f + at, snap, 8) == 0) {
						r5q_eapol_rx(f + at + 8, ml - at - 8);
						eapol = true;
						break;
					}
				}
				if (!eapol && join_step >= 24) {
					r5q_rx_to_net(f, ml);
				} else if (!eapol && ++rx_data <= 6) {
					htc_show("rx frame (start of payload)", f, ml < 40 ? ml : 40);
				}
			}
			r5q_rx_recycle(pa);
		}
	} else {
		htt_other++;
		if (type == 0x3 && join_step == 13) {   /* PEER_MAP: the firmware made the peer */
			join_ev = true;
		}
		if (type == 0x7 && pl >= 6) {           /* TX_COMPL_IND: status (0 ok, 1 discard, 2 no ack), count, ids */
			for (uint32_t k = 0; k < m[2] && 4u + 2u * (k + 1) <= pl; k++) {
				uint16_t id = (uint16_t)(m[4 + 2 * k] | m[5 + 2 * k] << 8);

				if (id < R5Q_TXN) {
					TX_CLEAR(id);
				}
			}
			tx_compl += m[2];
			if (tx_compl <= 4 || ((m[1] & 7) != 0 && ++tx_compl_bad <= 10)) {
				printf("wifi: htt: TX_COMPL: status %u, %u frame(s), id %u\n", m[1] & 7, m[2], m[4] | m[5] << 8);
			}
			return;
		}
		if (htt_other <= 40) {
			htc_show(type == 0x3 ? "htt PEER_MAP" : type == 0x1 ? "htt RX_IND" : "htt message", m, pl);
		}
	}
}

static void
r5q_htt_send(const void *msg, uint16_t len)
{
	if (htt_ep != 0xff) {
		ce_send_htc(&ce4, 0x10, htt_ep, msg, len);
	}
}
static int wmi_credits = -1;
static uint64_t wmi_last_send_us, wmi_hold_us;
static uint32_t scan_frames, scan_nets;
static uint8_t scan_bss[64][6];
static const uint8_t r5q_mac[6] = { 0x02, 0x52, 0x35, 0x51, 0x00, 0x01 };     /* locally administered: "R5Q" */

static void
r5q_wmi_send(uint8_t eid, uint32_t cmd_id, const void *tlvs, uint16_t len)
{
	static uint8_t buf[1600];

	if (len + 4 > sizeof(buf)) {
		printf("wifi: wmi: command %#x too long (%u bytes)\n", cmd_id, len);
		return;
	}
	memcpy(buf, &cmd_id, 4);
	memcpy(buf + 4, tlvs, len);
	ce_send_htc(&ce3, 0xc, eid, buf, (uint16_t)(len + 4));
	if (wmi_credits > 0) {
		wmi_credits--;
	}
	wmi_last_send_us = now_us();
}

static uint32_t
tlv_put(uint8_t *at, uint16_t tag, const void *v, uint16_t len)
{
	uint16_t h[2] = { len, tag };

	memcpy(at, h, 4);
	if (v != NULL && len != 0) {
		memcpy(at + 4, v, len);
	}
	return 4u + len;
}

static const uint16_t r5q_scan_mhz[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
	5745, 5765, 5785, 5805, 5825,
};
#define R5Q_SCAN_N (sizeof(r5q_scan_mhz) / sizeof(r5q_scan_mhz[0]))

/* the next command of the scan, when WMI has a credit (or none came back in 700 ms: the engine then just holds it) */
static void
r5q_wmi_next(uint8_t eid)
{
	static uint8_t m[1200];
	uint32_t n = 0;

	if (wmi_step == 0 || (wmi_step >= 6 && (join_step < 10 || join_step == 20 || join_step >= 24))) {
		return;
	}
	if (wmi_step >= 6) {
		bool sends = join_step == 10 || join_step == 12 || join_step == 14 || join_step == 16 || join_step == 18 ||
		    (join_step >= 19 && join_step <= 23);

		if (sends && wmi_credits == 0 && now_us() - wmi_last_send_us < 700000) {
			return;
		}
		if (sends && wmi_credits == 0) {
			printf("wifi: wmi: no credit back after 700 ms: sending anyway\n");
		}
		r5q_join_next(eid);
		return;
	}
	if (wmi_step == 1) {
		uint8_t req[4] = { 0, 0, 0, 0 };                                 /* VERSION_REQ */

		if (htt_ep == 0xff || !r5q_rx_ring_alloc()) {
			wmi_step = 3;
			return;
		}
		r5q_htt_send(req, sizeof(req));
		wmi_last_send_us = now_us();
		printf("wifi: htt: VERSION_REQ sent (endpoint %u, CE4)\n", htt_ep);
		wmi_step = 2;
		return;
	}
	if (wmi_step == 2) {
		uint8_t c[48];
		uint16_t w16[14] = {
			R5Q_RXR, 2048, 0xffff, 0,        /* ring entries, buffer bytes, every content flag, fw index (set below) */
			74, 90, 27, 37, 4, 26, 7, 12, 1, 2,
		};

		if (!htt_ver_ok && now_us() - wmi_last_send_us < 2000000) {
			return;
		}
		if (!htt_ver_ok) {
			printf("wifi: htt: no VERSION_CONF in 2 s: configuring the rx ring anyway\n");
		}
		/*
		 * (Test 119) FRAG_DESC_BANK_CFG (H2T 6) first, as ath10k_htt_setup: WCN3990 transmits from "continuous" fragment
		 * descriptors (struct htt_msdu_ext_desc_64, 72 bytes each) in a bank the host gives: one bank of 4 here, in the
		 * same DMA page as the frames sent (r5q_htt_tx). info 0 (no peer flow control); q_state record size 1.
		 */
		if (dtx_frames == NULL && (dtx_frames = kalloc_data(R5Q_TXN * 2048, Z_WAITOK | Z_ZERO)) != NULL) {
			for (uint32_t k = 0; k < R5Q_TXN; k++) {
				dtx_fpa[k] = kvtophys((vm_offset_t)dtx_frames + 2048 * k);
			}
		}
		if (dtx_page == NULL && dtx_frames != NULL && ((vm_offset_t)dtx_frames & 2047) == 0 &&
		    (dtx_page = r5q_dma_page(&dtx_pa, (R5Q_TXN * 72 + PAGE_SIZE - 1) & ~(size_t)PAGE_MASK)) != NULL) {
			uint8_t b[64];

			bzero(b, sizeof(b));
			b[0] = 6;
			b[2] = 1;                       /* banks */
			b[3] = 72;                      /* descriptor size */
			memcpy(b + 4, &dtx_pa, 8);      /* bank 0 base */
			b[38] = (uint8_t)(R5Q_TXN - 1); /* bank 0 ids 0..127 */
			b[39] = (uint8_t)((R5Q_TXN - 1) >> 8);
			b[60] = 1;                      /* q_state record size */
			r5q_htt_send(b, sizeof(b));
			printf("wifi: htt: FRAG_DESC_BANK_CFG sent: %u descriptors of 72 bytes at %#llx\n", R5Q_TXN, dtx_pa);
		}
		w16[3] = (uint16_t)rxr_alloc;
		bzero(c, sizeof(c));
		c[0] = 2;                       /* RX_RING_CFG */
		c[1] = 1;                       /* one ring */
		memcpy(c + 4, &rxr_idx_pa, 8);
		memcpy(c + 12, &rxr_ring_pa, 8);
		memcpy(c + 20, w16, sizeof(w16));
		r5q_htt_send(c, sizeof(c));
		wmi_last_send_us = now_us();
		printf("wifi: htt: RX_RING_CFG sent: %u buffers of 2048 at ring %#llx, index %#llx (%u)\n", R5Q_RXR - 1,
		    rxr_ring_pa, rxr_idx_pa, rxr_alloc);
		wmi_step = 3;
		return;
	}
	if (wmi_credits == 0 && now_us() - wmi_last_send_us < 700000) {
		return;
	}
	if (wmi_credits == 0) {
		printf("wifi: wmi: no credit back after 700 ms: sending anyway\n");
	}
	if (wmi_step == 3) {
		uint32_t num = R5Q_SCAN_N, at;

		n += tlv_put(m + n, 0x4f, &num, 4);                              /* SCAN_CHAN_LIST_CMD */
		at = n;
		n += 4;                                                          /* ARRAY_STRUCT, length below */
		for (uint32_t i = 0; i < R5Q_SCAN_N; i++) {
			uint32_t c[6], mhz = r5q_scan_mhz[i];

			c[0] = mhz;
			c[1] = mhz;                                              /* 20 MHz: centre = primary */
			c[2] = 0;
			c[3] = (mhz < 3000 ? 5u : 4u) | (1u << 7) | (1u << 11);   /* 11ng/11na HT20, PASSIVE, ALLOW_HT */
			c[4] = 0 | (40u << 8) | (40u << 16);                     /* min 0, max 20 dBm, reg 20 dBm (0.5 dBm units) */
			c[5] = 0 | (40u << 8);                                   /* antenna gain 0, max tx 20 dBm */
			n += tlv_put(m + n, 0x50, c, sizeof(c));                 /* CHANNEL */
		}
		tlv_put(m + at, 0x12, NULL, (uint16_t)(n - at - 4));
		r5q_wmi_send(eid, 0x3003, m, (uint16_t)n);
		printf("wifi: wmi: SCAN_CHAN_LIST sent: %u channels (2.4 GHz 1-13, 5 GHz 36-64/100-140/149-165), all passive\n",
		    (unsigned)R5Q_SCAN_N);
	} else if (wmi_step == 4) {
		uint32_t v[5] = { 0, 2, 0, 0, 0 };                               /* vdev 0, STA, no subtype */

		memcpy(&v[3], r5q_mac, 6);
		n += tlv_put(m + n, 0x56, v, sizeof(v));
		r5q_wmi_send(eid, 0x5001, m, (uint16_t)n);
		printf("wifi: wmi: VDEV_CREATE sent: vdev 0, station, %02x:%02x:%02x:%02x:%02x:%02x\n", r5q_mac[0], r5q_mac[1],
		    r5q_mac[2], r5q_mac[3], r5q_mac[4], r5q_mac[5]);
	} else {
		uint32_t s[25], ch[R5Q_SCAN_N], bssid[2] = { 0xffffffffu, 0xffffu };

		bzero(s, sizeof(s));
		s[0] = 0xA000 | 1;              /* scan_id (WMI_HOST_SCAN_REQ_ID_PREFIX) */
		s[1] = 0xA000 | 1;              /* scan_req_id */
		s[2] = 0;                       /* vdev */
		s[3] = 1;                       /* priority LOW */
		s[4] = 0x1ff;                   /* every scan event */
		s[5] = 50;                      /* dwell active */
		s[6] = 150;                     /* dwell passive */
		s[7] = 50;                      /* min rest */
		s[8] = 500;                     /* max rest */
		s[9] = 0;                       /* repeat probe */
		s[10] = 0;                      /* probe spacing */
		s[11] = 0;                      /* idle */
		s[12] = 20000;                  /* max scan time */
		s[13] = 5;                      /* probe delay */
		s[14] = 0x1 ^ 0x20;             /* PASSIVE; FILTER_PROBE_REQ inverted on WMI-TLV, as ath10k does */
		s[15] = 0;                      /* burst duration */
		s[16] = R5Q_SCAN_N;             /* channels */
		s[17] = 1;                      /* bssids */
		s[18] = 0;                      /* ssids */
		s[19] = 0;                      /* ie length */
		s[20] = 3;                      /* probes */
		/* s[21..24]: mac_addr / mac_mask (no randomisation) */
		for (uint32_t i = 0; i < R5Q_SCAN_N; i++) {
			ch[i] = r5q_scan_mhz[i];
		}
		n += tlv_put(m + n, 0x4d, s, sizeof(s));                         /* START_SCAN_CMD */
		n += tlv_put(m + n, 0x10, ch, sizeof(ch));                       /* ARRAY_UINT32: the channels */
		n += tlv_put(m + n, 0x13, NULL, 0);                              /* ARRAY_FIXED_STRUCT: no SSIDs */
		n += tlv_put(m + n, 0x13, bssid, sizeof(bssid));                 /* ARRAY_FIXED_STRUCT: ff:ff:ff:ff:ff:ff */
		n += tlv_put(m + n, 0x11, NULL, 0);                              /* ARRAY_BYTE: no IEs */
		r5q_wmi_send(eid, 0x3001, m, (uint16_t)n);
		wmi_hold_us = now_us() + 30000000;
		printf("wifi: wmi: START_SCAN sent (passive, %u channels, 150 ms each); listening up to 30 s\n", (unsigned)R5Q_SCAN_N);
	}
	wmi_step++;
}

/* HTC trailer credit records for WMI's endpoint */
static void
htc_trailer(const uint8_t *p, uint16_t hl, uint8_t wmi_eid)
{
	uint8_t tl = p[4];
	const uint8_t *t = p + 8 + hl - tl;

	if (!(p[1] & 0x2) || tl == 0 || tl > hl) {
		return;
	}
	for (uint32_t at = 0; at + 4 <= tl;) {          /* record: id, len, 2 pad, then len bytes (Test 116: 01 04 00 00 02 04 00 00) */
		uint8_t id = t[at], rl = t[at + 1];

		if (at + 4 + rl > tl) {
			break;
		}
		if (id == 1) {
			for (uint32_t k = 0; k + 4 <= rl; k += 4) {
				const uint8_t *r = t + at + 4 + k;

				if (r[0] == wmi_eid && wmi_credits >= 0) {
					wmi_credits += r[1];
				}
				static uint32_t said;

				if (++said <= 30) {
					printf("wifi: htc: credit report: endpoint %u +%u (WMI now %d)\n", r[0], r[1], wmi_credits);
				}
			}
		}
		at += 4u + rl;
	}
}

/*
 * (Test 118) Joining: the network named by boot-arg wifi_ssid (hex, after an 'h': "wifi_ssid=h<ssid-hex>"), its
 * strongest BSS from the scan. As ath10k + mac80211 for a station: VDEV_START_REQUEST (0x5003, tag 0x58 + CHANNEL
 * 0x50 + empty ARRAY_STRUCT) and its response event (0x5001, tag 0x28); PEER_CREATE (0x6001, tag 0x61) for the BSSID,
 * which HTT confirms with PEER_MAP (T2H 0x3); the 802.11 open-system authentication and the association request sent
 * as management frames by reference (MGMT_TX_SEND 0x7008, tag 0x1a6: vdev, desc id, 0, frame address, length; then
 * up to 64 bytes inline), their answers seen in WMI management rx; then PEER_ASSOC (0x6005, tag 0x65 + legacy rates +
 * HT rates + VHT_RATE_SET 0x66) and VDEV_UP (0x5005, tag 0x5c). WPA2-PSK / CCMP is asked for in the RSN element; the
 * 4-way handshake (needing the key) comes next: here the access point's first EAPOL message is only watched for.
 */
struct r5q_bss {
	uint8_t bssid[6];
	uint8_t chan;
	int rssi;
	uint16_t cap, bint, ielen;
	uint8_t ies[700];
};
static struct r5q_bss join_bss;
static bool join_have;
static uint8_t join_ssid[32], join_ssid_len;
static uint64_t join_t;
static uint32_t join_tries, join_aid;
static uint16_t join_seq;
static uint8_t *mtx_page;
static uint64_t mtx_pa;
static uint32_t mtx_slot;

static int
hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void
r5q_join_args(void)
{
	char h[80];

	bzero(h, sizeof(h));
	join_ssid_len = 0;
	if (!PE_parse_boot_argn("wifi_ssid", h, sizeof(h)) || h[0] != 'h') {
		return;
	}
	for (uint32_t i = 1; h[i] && h[i + 1] && join_ssid_len < 32; i += 2) {
		int a = hexval(h[i]), b = hexval(h[i + 1]);

		if (a < 0 || b < 0) {
			break;
		}
		join_ssid[join_ssid_len++] = (uint8_t)(a << 4 | b);
	}
}

static const uint8_t *
ie_find(const uint8_t *ies, uint32_t len, uint8_t id)
{
	for (uint32_t at = 0; at + 2 <= len;) {
		if (at + 2 + ies[at + 1] > len) {
			break;
		}
		if (ies[at] == id) {
			return ies + at;
		}
		at += 2u + ies[at + 1];
	}
	return NULL;
}

/* every beacon / probe response: the strongest BSS of the wanted network */
static void
r5q_join_consider(const uint8_t *f, uint32_t flen, int rssi, uint32_t rx_chan)
{
	const uint8_t *ies = f + 36, *e;
	uint32_t il = flen - 36;

	if (join_ssid_len == 0 || (e = ie_find(ies, il, 0)) == NULL || e[1] != join_ssid_len ||
	    memcmp(e + 2, join_ssid, join_ssid_len) != 0 || il > sizeof(join_bss.ies)) {
		return;
	}
	if (join_have && rssi <= join_bss.rssi && memcmp(join_bss.bssid, f + 16, 6) != 0) {
		return;
	}
	memcpy(join_bss.bssid, f + 16, 6);
	join_bss.rssi = rssi;
	join_bss.bint = (uint16_t)(f[32] | f[33] << 8);
	join_bss.cap = (uint16_t)(f[34] | f[35] << 8);
	join_bss.ielen = (uint16_t)il;
	memcpy(join_bss.ies, ies, il);
	join_bss.chan = (uint8_t)rx_chan;
	if ((e = ie_find(ies, il, 3)) != NULL && e[1] == 1) {
		join_bss.chan = e[2];
	} else if ((e = ie_find(ies, il, 61)) != NULL && e[1] >= 1) {
		join_bss.chan = e[2];
	}
	join_have = true;
}

static uint32_t
chan_mhz(uint32_t ch)
{
	return ch == 14 ? 2484 : ch < 14 ? 2407 + 5 * ch : 5000 + 5 * ch;
}

/* a management frame out by reference: the frame in a DMA page slot, its first 64 bytes inline as well */
static void
r5q_mgmt_tx(uint8_t eid, uint8_t subtype, const uint8_t *body, uint32_t blen)
{
	uint8_t *f, m[4 + 32 + 4 + 64];
	uint32_t flen = 24 + blen, inl, c[8];
	uint64_t pa;

	if (mtx_page == NULL && (mtx_page = r5q_dma_page(&mtx_pa, PAGE_SIZE)) == NULL) {
		return;
	}
	if (flen > 1024) {
		return;
	}
	mtx_slot = (mtx_slot + 1) & 3;
	f = mtx_page + 1024 * mtx_slot;
	pa = mtx_pa + 1024 * mtx_slot;
	bzero(f, 24);
	f[0] = subtype;
	memcpy(f + 4, join_bss.bssid, 6);
	memcpy(f + 10, r5q_mac, 6);
	memcpy(f + 16, join_bss.bssid, 6);
	f[22] = (uint8_t)(join_seq << 4);
	f[23] = (uint8_t)(join_seq >> 4);
	join_seq++;
	memcpy(f + 24, body, blen);
	FlushPoC_DcacheRegion((vm_offset_t)f, flen);
	inl = flen < 64 ? (flen + 3) & ~3u : 64;
	c[0] = 0;                       /* vdev */
	c[1] = mtx_slot;                /* desc id */
	c[2] = 0;                       /* chanfreq */
	memcpy(&c[3], &pa, 8);          /* paddr (packed: words 3-4) */
	c[5] = flen;                    /* frame_len */
	c[6] = inl;                     /* buf_len */
	bzero(m, sizeof(m));
	tlv_put(m, 0x1a6, c, 28);
	tlv_put(m + 32, 0x11, f, (uint16_t)inl);
	r5q_wmi_send(eid, 0x7008, m, (uint16_t)(36 + inl));
}

/*
 * (Test 119) WPA2-PSK, the station's half of the 4-way handshake (IEEE 802.11-2016 12.7.6), in the kernel:
 * - the PMK comes from USERDATA 4 GiB + 520 KiB ("R5Q-WIFI-KEY/1", written by `python phone.py wifi-join`, which derives
 *   it on the PC from the name and password; the password is never stored). Without one, a dry run: message 2 is made
 *   with an all-zero PMK, so the access point refuses its MIC, but the transmit path is exercised once.
 * - EAPOL-Key frames arrive in HTT rx buffers (RX_IN_ORD_PADDR_IND) after the rx descriptor, native-wifi decapsulated
 *   (802.11 header, LLC/SNAP 88-8e); answers leave as HTT TX_FRM (H2T 1) on CE4: the HTT tx descriptor, a fragment
 *   descriptor in the bank pointing at the frame, and its first 52 bytes inline ("prefetch"), TID 7 as mac80211 does.
 * - message 1: SNonce, PTK = PRF-384(PMK, "Pairwise key expansion", min/max(AA, SPA) || min/max(ANonce, SNonce)),
 *   message 2 with our RSN element and MIC = HMAC-SHA1(KCK)[0:16]; message 3: MIC checked, the key data unwrapped with
 *   the KEK (AES key wrap), the GTK KDE (00-0f-ac:1) taken, message 4 sent; then VDEV_INSTALL_KEY (0x5009, tag 0x60:
 *   AES-CCM = 4) for the PTK's TK (pairwise, index 0) and the GTK (group, its index), and PEER_SET_PARAM (0x6004, tag
 *   0x64) AUTHORIZE (3) = 1.
 */
#include "redwood_wpa.h"
extern void read_random(void *buffer, u_int numBytes);

#define R5Q_WIFI_KEY_AT ((4ULL << 30) + 520ULL * 1024)
static uint8_t wpa_pmk[32], wpa_anonce[32], wpa_snonce[32], wpa_ptk[48], wpa_gtk[32];
static uint8_t wpa_rsn[22];
static bool wpa_have_pmk, wpa_have_snonce, wpa_dry_sent;
static uint32_t wpa_gtk_len, wpa_gtk_idx, wpa_msg2s;
static uint64_t wpa_msg4_us;

static void
r5q_wifi_key_load(void)
{
	static uint8_t blk[4096];
	uint32_t sl, want, h = 2166136261u;

	if (redwood_ufs_read_named("darwin", R5Q_WIFI_KEY_AT, blk, sizeof(blk), NULL) != 0 ||
	    memcmp(blk, "R5Q-WIFI-KEY/1", 14) != 0) {
		printf("wifi: wpa: no network key stored on the phone (python phone.py wifi-join): a dry run only\n");
		return;
	}
	for (uint32_t k = 0; k < 84; k++) {
		h = (h ^ blk[k]) * 16777619u;
	}
	memcpy(&sl, blk + 16, 4);
	memcpy(&want, blk + 84, 4);
	if (h != want || sl < 1 || sl > 32) {
		printf("wifi: wpa: the stored network key record is damaged: not used\n");
		bzero(blk, sizeof(blk));
		return;
	}
	if (join_ssid_len == 0) {
		memcpy(join_ssid, blk + 20, sl);
		join_ssid_len = (uint8_t)sl;
	} else if (sl != join_ssid_len || memcmp(join_ssid, blk + 20, sl) != 0) {
		printf("wifi: wpa: the stored key is for another network name than wifi_ssid: not used\n");
		bzero(blk, sizeof(blk));
		return;
	}
	memcpy(wpa_pmk, blk + 52, 32);
	wpa_have_pmk = true;
	bzero(blk, sizeof(blk));
	printf("wifi: wpa: a network key is stored (for a %u-byte network name): it will be used\n", sl);
}

/* a data frame (native wifi: 802.11 header + LLC/SNAP + payload) out through HTT; false: no room, dropped */
static bool
r5q_htt_tx(const uint8_t *frame, uint32_t len, uint8_t tid)
{
	uint8_t m[20 + 52], *f, *ext;
	uint64_t fpa, epa, now = now_us();
	uint32_t pf, lo, slot = 0, k, sr;
	uint16_t hi, l16 = (uint16_t)len, fl1 = (uint16_t)((tid << 6) | (1u << 11));    /* vdev 0, TID, POSTPONED */

	if (dtx_page == NULL || dtx_frames == NULL || htt_ep == 0xff || len > 2048) {
		return false;
	}
	for (k = 0; k < R5Q_TXN; k++) {
		if (TX_BUSY(k) && now - dtx_sent_us[k] > 2000000) {
			TX_CLEAR(k);                    /* no completion in 2 s: taken back */
		}
	}
	sr = rd(wlan_regs + CE_REG(4, CE_CUR_SRRI)) & (ce4.n - 1);
	if (((ce4.wr + 1) & (ce4.n - 1)) == sr) {
		tx_drops++;
		return false;
	}
	for (k = 0; k < R5Q_TXN; k++) {
		slot = (dtx_slot + 1 + k) % R5Q_TXN;
		if (!TX_BUSY(slot)) {
			break;
		}
	}
	if (k == R5Q_TXN) {
		tx_drops++;
		return false;
	}
	dtx_slot = slot;
	TX_SET(slot);
	dtx_sent_us[slot] = now;
	f = dtx_frames + 2048 * slot;
	fpa = dtx_fpa[slot];
	ext = dtx_page + 72 * slot;
	epa = dtx_pa + 72 * slot;
	memcpy(f, frame, len);
	bzero(ext, 72);
	lo = (uint32_t)fpa;
	hi = (uint16_t)(fpa >> 32);
	memcpy(ext + 24, &lo, 4);               /* frags[0]: paddr_lo, paddr_hi, len (frags[1] zero ends the list) */
	memcpy(ext + 28, &hi, 2);
	memcpy(ext + 30, &l16, 2);
	FlushPoC_DcacheRegion((vm_offset_t)f, len);
	FlushPoC_DcacheRegion((vm_offset_t)ext, 72);
	bzero(m, sizeof(m));
	m[0] = 1;                               /* TX_FRM */
	m[1] = 0x21;                            /* MAC header present, packet type native wifi */
	memcpy(m + 2, &fl1, 2);
	memcpy(m + 4, &l16, 2);
	m[6] = (uint8_t)slot;                   /* msdu id */
	m[7] = (uint8_t)(slot >> 8);
	memcpy(m + 8, &epa, 8);                 /* the fragment descriptor */
	m[16] = 0xff;                           /* peer id: invalid (the firmware finds it) */
	m[17] = 0xff;
	pf = len < 50 ? len : 50;
	memcpy(m + 20, frame, pf);
	pf = (pf + 3) & ~3u;
	ce_send_htc(&ce4, 0x10, htt_ep, m, (uint16_t)(20 + pf));
	tx_sent++;
	return true;
}

/* an EAPOL-Key frame to the access point: header, SNAP, EAPOL (MIC filled in with the KCK) */
static void
r5q_eapol_send(uint8_t ver, uint16_t info, const uint8_t replay[8], const uint8_t *nonce, const uint8_t *kd, uint16_t kdl)
{
	uint8_t f[24 + 8 + 99 + 64], *e = f + 32, mic[20];
	uint16_t body = (uint16_t)(95 + kdl);
	uint32_t el = 99u + kdl;
	const uint8_t *d[1];
	size_t dl[1];

	if (kdl > 64) {
		return;
	}
	bzero(f, sizeof(f));
	f[0] = 0x08;                            /* data */
	f[1] = (uint8_t)(0x01 | (r5q_keyed ? 0x40 : 0));        /* to DS; protected once the pairwise key is in */
	memcpy(f + 4, join_bss.bssid, 6);
	memcpy(f + 10, r5q_mac, 6);
	memcpy(f + 16, join_bss.bssid, 6);
	f[24] = 0xaa;
	f[25] = 0xaa;
	f[26] = 0x03;
	f[30] = 0x88;
	f[31] = 0x8e;
	e[0] = ver;
	e[1] = 3;                               /* EAPOL-Key */
	e[2] = (uint8_t)(body >> 8);
	e[3] = (uint8_t)body;
	e[4] = 2;                               /* RSN key descriptor */
	e[5] = (uint8_t)(info >> 8);
	e[6] = (uint8_t)info;
	memcpy(e + 9, replay, 8);
	if (nonce != NULL) {
		memcpy(e + 17, nonce, 32);
	}
	e[97] = (uint8_t)(kdl >> 8);
	e[98] = (uint8_t)kdl;
	if (kdl) {
		memcpy(e + 99, kd, kdl);
	}
	d[0] = e;
	dl[0] = el;
	wpa_hmac_sha1(wpa_ptk, 16, d, dl, 1, mic);
	memcpy(e + 81, mic, 16);
	r5q_htt_tx(f, 32 + el, 7);
}

static void
r5q_eapol_rx(const uint8_t *e, uint32_t len)
{
	uint16_t info, dl;

	if (len < 99 || e[1] != 3) {
		printf("wifi: wpa: an EAPOL frame of type %u (%u bytes)\n", len >= 2 ? e[1] : 0, len);
		return;
	}
	info = (uint16_t)(e[5] << 8 | e[6]);
	dl = (uint16_t)(e[97] << 8 | e[98]);
	printf("wifi: wpa: EAPOL-Key from the access point: info %#x, replay %02x%02x, %u bytes of key data\n", info, e[15],
	    e[16], dl);
	if (join_step < 20 || 99u + dl > len) {
		return;
	}
	if ((info & 0x0008) && (info & 0x0080) && !(info & 0x0100)) {          /* message 1 */
		uint8_t data[76], *pmk = wpa_pmk, zero[32];
		int c;

		if (!wpa_have_pmk) {
			if (wpa_dry_sent) {
				return;
			}
			bzero(zero, sizeof(zero));
			pmk = zero;
		}
		if (memcmp(wpa_anonce, e + 17, 32) != 0) {
			wpa_have_snonce = false;        /* a new handshake (the access point renews the keys): a new SNonce */
		}
		memcpy(wpa_anonce, e + 17, 32);
		if (!wpa_have_snonce) {
			uint64_t t = mach_absolute_time();
			struct wpa_sha1 s;
			uint8_t r[32], hsh[20];

			read_random(r, sizeof(r));
			wpa_sha1_init(&s);
			wpa_sha1_add(&s, r, sizeof(r));
			wpa_sha1_add(&s, (const uint8_t *)&t, sizeof(t));
			wpa_sha1_end(&s, hsh);
			memcpy(wpa_snonce, r, 32);
			for (int i = 0; i < 20; i++) {
				wpa_snonce[i] ^= hsh[i];
			}
			wpa_have_snonce = true;
		}
		c = memcmp(join_bss.bssid, r5q_mac, 6);
		memcpy(data, c < 0 ? join_bss.bssid : r5q_mac, 6);
		memcpy(data + 6, c < 0 ? r5q_mac : join_bss.bssid, 6);
		c = memcmp(wpa_anonce, wpa_snonce, 32);
		memcpy(data + 12, c < 0 ? wpa_anonce : wpa_snonce, 32);
		memcpy(data + 44, c < 0 ? wpa_snonce : wpa_anonce, 32);
		wpa_prf(pmk, 32, "Pairwise key expansion", data, sizeof(data), wpa_ptk, 48);
		r5q_eapol_send(e[0], 0x010a, e + 9, wpa_snonce, wpa_rsn, sizeof(wpa_rsn));
		wpa_msg2s++;
		printf("wifi: wpa: message 1 -> message 2 sent%s\n", wpa_have_pmk ? "" :
		    " (DRY RUN: no key stored, made with a zero PMK -- the access point will refuse its MIC)");
		if (!wpa_have_pmk) {
			wpa_dry_sent = true;
			bzero(wpa_ptk, sizeof(wpa_ptk));
		}
	} else if ((info & 0x0008) && (info & 0x01c0) == 0x01c0 && wpa_have_pmk) {     /* message 3 */
		uint8_t copy[99 + 256], mic[20], plain[256];
		const uint8_t *d[1];
		size_t dlen[1];

		if (dl > 256 || memcmp(e + 17, wpa_anonce, 32) != 0) {
			printf("wifi: wpa: message 3 with another ANonce or too much key data: ignored\n");
			return;
		}
		memcpy(copy, e, 99u + dl);
		bzero(copy + 81, 16);
		d[0] = copy;
		dlen[0] = 99u + dl;
		wpa_hmac_sha1(wpa_ptk, 16, d, dlen, 1, mic);
		if (memcmp(mic, e + 81, 16) != 0) {
			printf("wifi: wpa: message 3's MIC is WRONG: the key does not match this network's password\n");
			join_error = EAUTH;
			return;
		}
		wpa_gtk_len = 0;
		if ((info & 0x1000) && dl >= 24 && (dl % 8) == 0 && wpa_aes_unwrap(wpa_ptk + 16, e + 99, dl / 8 - 1, plain) == 0) {
			for (uint32_t at = 0; at + 2 <= dl - 8u;) {
				uint8_t id = plain[at], l = plain[at + 1];

				if (at + 2u + l > dl - 8u) {
					break;
				}
				if (id == 0xdd && l >= 6 + 16 && plain[at + 2] == 0x00 && plain[at + 3] == 0x0f && plain[at + 4] == 0xac &&
				    plain[at + 5] == 1 && (uint32_t)(l - 6) <= sizeof(wpa_gtk)) {
					wpa_gtk_idx = plain[at + 6] & 3;
					wpa_gtk_len = (uint32_t)l - 6;
					memcpy(wpa_gtk, plain + at + 8, wpa_gtk_len);
				}
				at += 2u + l;
			}
		}
		bzero(plain, sizeof(plain));
		r5q_eapol_send(e[0], 0x030a, e + 9, NULL, NULL, 0);
		wpa_msg4_us = now_us();
		printf("wifi: wpa: message 3 (MIC good; group key %u bytes, index %u) -> message 4 sent: 4-WAY HANDSHAKE DONE\n",
		    wpa_gtk_len, wpa_gtk_idx);
		if (join_step == 20) {
			join_step = 21;
		} else if (join_step == 24) {
			wpa_rekey_us = now_us();        /* renewed keys: installed once message 4 is out (r5q_service) */
		}
	} else if (!(info & 0x0008) && (info & 0x0180) == 0x0180 && wpa_have_pmk && join_step >= 24) {     /* group message 1 */
		uint8_t copy[99 + 256], mic[20], plain[256];
		const uint8_t *d[1];
		size_t dlen[1];

		if (dl > 256 || dl < 24 || (dl % 8) != 0) {
			return;
		}
		memcpy(copy, e, 99u + dl);
		bzero(copy + 81, 16);
		d[0] = copy;
		dlen[0] = 99u + dl;
		wpa_hmac_sha1(wpa_ptk, 16, d, dlen, 1, mic);
		if (memcmp(mic, e + 81, 16) != 0 || wpa_aes_unwrap(wpa_ptk + 16, e + 99, dl / 8 - 1, plain) != 0) {
			printf("wifi: wpa: group key message with a bad MIC or key data: ignored\n");
			return;
		}
		wpa_gtk_len = 0;
		for (uint32_t at = 0; at + 2 <= dl - 8u;) {
			uint8_t id = plain[at], l = plain[at + 1];

			if (at + 2u + l > dl - 8u) {
				break;
			}
			if (id == 0xdd && l >= 6 + 16 && plain[at + 2] == 0x00 && plain[at + 3] == 0x0f && plain[at + 4] == 0xac &&
			    plain[at + 5] == 1 && (uint32_t)(l - 6) <= sizeof(wpa_gtk)) {
				wpa_gtk_idx = plain[at + 6] & 3;
				wpa_gtk_len = (uint32_t)l - 6;
				memcpy(wpa_gtk, plain + at + 8, wpa_gtk_len);
			}
			at += 2u + l;
		}
		bzero(plain, sizeof(plain));
		r5q_eapol_send(e[0], 0x0302, e + 9, NULL, NULL, 0);
		if (wpa_gtk_len != 0 && svc_wmi_eid != 0xff) {
			r5q_install_key(svc_wmi_eid, wpa_gtk_idx, 1, wpa_gtk, wpa_gtk_len);
		}
		bzero(wpa_gtk, sizeof(wpa_gtk));
		printf("wifi: wpa: the access point renewed the group key (index %u): answered and installed\n", wpa_gtk_idx);
	} else {
		printf("wifi: wpa: EAPOL-Key not handled here (info %#x)\n", info);
	}
}

static void
r5q_install_key(uint8_t eid, uint32_t idx, uint32_t flags, const uint8_t *key, uint32_t klen)
{
	uint32_t v[23];
	uint8_t m[4 + 92 + 4 + 32];
	uint32_t n;

	bzero(v, sizeof(v));
	memcpy(&v[1], join_bss.bssid, 6);
	v[3] = idx;
	v[4] = flags;                           /* 0 pairwise, 1 group */
	v[5] = 4;                               /* WMI_TLV_CIPHER_AES_CCM */
	v[20] = klen;
	n = tlv_put(m, 0x60, v, sizeof(v));
	n += tlv_put(m + n, 0x11, key, (uint16_t)((klen + 3) & ~3u));
	r5q_wmi_send(eid, 0x5009, m, (uint16_t)n);
	bzero(m, sizeof(m));
}

static void
r5q_join_next(uint8_t eid)
{
	uint8_t m[400];
	uint32_t n = 0, mhz = chan_mhz(join_bss.chan);
	const uint8_t *ht = ie_find(join_bss.ies, join_bss.ielen, 45), *rsn = ie_find(join_bss.ies, join_bss.ielen, 48);
	uint64_t now = now_us();

	switch (join_step) {
	case 10: {
		uint32_t v[18], c[6];                   /* wmi_tlv_vdev_start_cmd: 18 words */

		bzero(v, sizeof(v));
		v[0] = 0;                               /* vdev */
		v[2] = join_bss.bint ? join_bss.bint : 100;
		v[3] = 1;                               /* dtim */
		/* v[5..13]: ssid (len + 32 bytes): empty for a station; bcn rate/power 0; noa 0; hw ack on */
		c[0] = mhz;
		c[1] = mhz;
		c[2] = 0;
		c[3] = (mhz < 3000 ? 5u : 4u) | (1u << 11);     /* HT20, ALLOW_HT, not passive */
		c[4] = (40u << 8) | (40u << 16);
		c[5] = (40u << 8);
		n += tlv_put(m + n, 0x58, v, sizeof(v));
		n += tlv_put(m + n, 0x50, c, sizeof(c));
		n += tlv_put(m + n, 0x12, NULL, 0);
		join_ev = false;
		r5q_wmi_send(eid, 0x5003, m, (uint16_t)n);
		printf("wifi: join: VDEV_START on %u MHz (channel %u) for %02x:%02x:%02x:%02x:%02x:%02x, %d dBm\n", mhz,
		    join_bss.chan, join_bss.bssid[0], join_bss.bssid[1], join_bss.bssid[2], join_bss.bssid[3], join_bss.bssid[4],
		    join_bss.bssid[5], join_bss.rssi);
		join_step = 11;
		join_t = now;
		break;
	}
	case 11:
	case 13:
		if (join_ev || now - join_t > (join_step == 11 ? 2000000u : 1000000u)) {
			if (!join_ev) {
				printf("wifi: join: no %s in time: going on\n", join_step == 11 ? "VDEV_START response" : "PEER_MAP");
			}
			join_step++;
		}
		break;
	case 12: {
		uint32_t v[4] = { 0, 0, 0, 0 };

		memcpy(&v[1], join_bss.bssid, 6);
		join_ev = false;
		n += tlv_put(m + n, 0x61, v, sizeof(v));
		r5q_wmi_send(eid, 0x6001, m, (uint16_t)n);
		printf("wifi: join: PEER_CREATE for the BSSID\n");
		join_step = 13;
		join_t = now;
		break;
	}
	case 14: {
		uint8_t a[6] = { 0, 0, 1, 0, 0, 0 };    /* open system, sequence 1, status 0 */

		join_ev = false;
		r5q_mgmt_tx(eid, 0xb0, a, sizeof(a));
		printf("wifi: join: authentication (open system) sent, try %u\n", join_tries + 1);
		join_step = 15;
		join_t = now;
		break;
	}
	case 15:
	case 17:
		if (join_ev) {
			join_step++;
			join_tries = 0;
		} else if (now - join_t > 1000000) {
			if (++join_tries >= 4) {
				printf("wifi: join: no %s answer after 4 tries: giving up\n", join_step == 15 ? "authentication" :
				    "association");
				join_error = ETIMEDOUT;
				join_step = 30;
			} else {
				join_step--;
			}
		}
		break;
	case 16: {
		uint8_t b[300];
		uint32_t k = 0;
		const uint8_t rates5[8] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
		const uint8_t rates2[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
		const uint8_t rates2x[4] = { 0x30, 0x48, 0x60, 0x6c };

		b[k++] = 0x01;                          /* capability: ESS */
		b[k++] = 0x00;
		b[k++] = 10;                            /* listen interval */
		b[k++] = 0;
		b[k++] = 0;                             /* SSID */
		b[k++] = join_ssid_len;
		memcpy(b + k, join_ssid, join_ssid_len);
		k += join_ssid_len;
		b[k++] = 1;
		b[k++] = 8;
		memcpy(b + k, mhz < 3000 ? rates2 : rates5, 8);
		k += 8;
		if (mhz < 3000) {
			b[k++] = 50;
			b[k++] = 4;
			memcpy(b + k, rates2x, 4);
			k += 4;
		}
		if (rsn != NULL && rsn[1] >= 8) {
			/* version 1, the AP's group cipher, pairwise CCMP, AKM PSK, no capabilities */
			const uint8_t r[] = { 48, 20, 1, 0, rsn[4], rsn[5], rsn[6], rsn[7], 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0,
				0x00, 0x0f, 0xac, 2, 0, 0 };

			memcpy(b + k, r, sizeof(r));
			memcpy(wpa_rsn, r, sizeof(r));          /* message 2 carries the same element */
			k += sizeof(r);
		}
		if (ht != NULL) {
			/* HT capabilities: SM power save off, 20 MHz; A-MPDU 64 KB; MCS 0-7 */
			const uint8_t h[28] = { 45, 26, 0x0c, 0x00, 0x03, 0xff };

			memcpy(b + k, h, sizeof(h));
			k += sizeof(h);
			{
				const uint8_t w[9] = { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };   /* WMM information */

				memcpy(b + k, w, sizeof(w));
				k += sizeof(w);
			}
		}
		join_ev = false;
		r5q_mgmt_tx(eid, 0x00, b, k);
		printf("wifi: join: association request sent (%u bytes; RSN %s, HT %s), try %u\n", k, rsn ? "WPA2-PSK/CCMP" :
		    "none (open)", ht ? "yes" : "no", join_tries + 1);
		join_step = 17;
		join_t = now;
		break;
	}
	case 18: {
		uint32_t v[19], vht[4] = { 0, 0, 0, 0 }, nl = 0;
		uint8_t legacy[12], htr[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
		const uint8_t *sr = ie_find(join_bss.ies, join_bss.ielen, 1), *xr = ie_find(join_bss.ies, join_bss.ielen, 50);

		for (int pass = 0; pass < 2; pass++) {
			const uint8_t *e = pass ? xr : sr;

			for (uint32_t i = 0; e != NULL && i < e[1] && nl < sizeof(legacy); i++) {
				uint8_t r = e[2 + i] & 0x7f;

				if (r == 0x7f) {                /* (BSS membership selector) */
					continue;
				}
				legacy[nl++] = (uint8_t)(r | ((r == 2 || r == 4 || r == 11 || r == 22) ? 0x80 : 0));
			}
		}
		bzero(v, sizeof(v));
		memcpy(&v[0], join_bss.bssid, 6);
		v[2] = 0;                               /* vdev */
		v[3] = 1;                               /* new association */
		v[4] = join_aid;
		v[5] = 0x1 | (rsn ? 0x4 : 0) | (ht ? (0x2 | 0x1000) : 0);     /* AUTH, NEED_PTK_4_WAY, QOS + HT */
		v[6] = join_bss.cap;
		v[7] = 1;                               /* listen interval */
		if (ht != NULL && ht[1] >= 3) {
			static const uint8_t dens[8] = { 0, 1, 1, 1, 2, 4, 8, 16 };

			v[8] = (uint32_t)(ht[2] | ht[3] << 8);
			v[9] = (1u << (13 + (ht[4] & 3))) - 1;
			v[10] = dens[(ht[4] >> 2) & 7];
			v[11] = 0x08;                   /* WMI_RC_HT_FLAG */
		}
		v[12] = 1;                              /* spatial streams */
		v[14] = ht ? (mhz < 3000 ? 5u : 4u) : (mhz < 3000 ? 1u : 0u);
		v[17] = nl;
		v[18] = ht ? 8 : 0;
		n += tlv_put(m + n, 0x65, v, sizeof(v));
		n += tlv_put(m + n, 0x11, legacy, (uint16_t)((nl + 3) & ~3u));
		n += tlv_put(m + n, 0x11, htr, ht ? 8 : 0);
		n += tlv_put(m + n, 0x66, vht, sizeof(vht));
		r5q_wmi_send(eid, 0x6005, m, (uint16_t)n);
		printf("wifi: join: PEER_ASSOC sent (aid %u, flags %#x, %u legacy rates, phy mode %u)\n", join_aid, v[5], nl, v[14]);
		join_step = 19;
		break;
	}
	case 19: {
		uint32_t v[4] = { 0, join_aid, 0, 0 };

		memcpy(&v[2], join_bss.bssid, 6);
		n += tlv_put(m + n, 0x5c, v, sizeof(v));
		r5q_wmi_send(eid, 0x5005, m, (uint16_t)n);
		printf("wifi: join: VDEV_UP: ASSOCIATED to the access point%s\n", rsn ? "; watching for its EAPOL message 1" :
		    " (an open network: no keys)");
		join_step = rsn ? 20 : 23;
		wmi_hold_us = now + 15000000;
		break;
	}
	case 21:
		if (now - wpa_msg4_us < 60000) {        /* message 4 leaves unencrypted first */
			break;
		}
		r5q_install_key(eid, 0, 0, wpa_ptk + 32, 16);
		bzero(wpa_ptk + 32, 16);
		r5q_keyed = true;
		printf("wifi: wpa: pairwise key installed (AES-CCM)\n");
		join_step = 22;
		break;
	case 22:
		if (wpa_gtk_len != 0) {
			r5q_install_key(eid, wpa_gtk_idx, 1, wpa_gtk, wpa_gtk_len);
			printf("wifi: wpa: group key installed (index %u)\n", wpa_gtk_idx);
		}
		bzero(wpa_gtk, sizeof(wpa_gtk));
		join_step = 23;
		break;
	case 23: {
		uint32_t v[5] = { 0, 0, 0, 3, 1 };     /* vdev 0, the BSSID, AUTHORIZE = 1 */

		memcpy(&v[1], join_bss.bssid, 6);
		n += tlv_put(m + n, 0x64, v, sizeof(v));
		r5q_wmi_send(eid, 0x6004, m, (uint16_t)n);
		printf("wifi: join: CONNECTED: keys installed, the access point authorized as our peer\n");
		join_step = 24;
		wmi_hold_us = now + 1000000;            /* (Test 120) then the net thread takes the rings over */
		break;
	}
	default:
		break;
	}
}

/* management frames for the join: authentication / association answers, and being sent away */
static void
r5q_join_rx(const uint8_t *f, uint32_t flen)
{
	uint8_t st = f[0];

	if (join_step < 10 || flen < 24 || memcmp(f + 16, join_bss.bssid, 6) != 0) {
		return;
	}
	if (st == 0xb0 && flen >= 30) {
		uint16_t alg = (uint16_t)(f[24] | f[25] << 8), seq = (uint16_t)(f[26] | f[27] << 8), status = (uint16_t)(f[28] | f[29] << 8);

		printf("wifi: join: authentication answer: algorithm %u, sequence %u, status %u\n", alg, seq, status);
		if (join_step == 15 && status == 0 && seq == 2) {
			join_ev = true;
		}
	} else if ((st == 0x10 || st == 0x30) && flen >= 30) {
		uint16_t cap = (uint16_t)(f[24] | f[25] << 8), status = (uint16_t)(f[26] | f[27] << 8), aid = (uint16_t)(f[28] | f[29] << 8);

		printf("wifi: join: association answer: status %u, aid %u, capability %#x\n", status, aid & 0x3fff, cap);
		if (join_step == 17 && status == 0) {
			join_aid = aid & 0x3fff;
			join_ev = true;
		} else if (join_step == 17) {
			join_error = ECONNREFUSED;
			join_step = 30;
		}
	} else if (st == 0xc0 || st == 0xa0) {
		printf("wifi: join: %s by the access point, reason %u\n", st == 0xc0 ? "DEAUTHENTICATED" : "DISASSOCIATED",
		    flen >= 26 ? (unsigned)(f[24] | f[25] << 8) : 0u);
		{
			unsigned why = flen >= 26 ? (unsigned)(f[24] | f[25] << 8) : 0u;

			if (join_step == 24) {
				redwood_net_link_state(false);
				if (r5q_sta_attached) {
					redwood_sta_lost(ECONNRESET);
				}
			} else if (join_step >= 20 && (why == 15 || why == 2)) {
				join_error = EAUTH;             /* the handshake did not come through: most likely the passphrase */
			} else if (join_error == 0) {
				join_error = ECONNREFUSED;
			}
		}
		r5q_keyed = false;
		join_step = 30;
	}
}

static void
r5q_scan_event(const uint8_t *v, uint32_t len)
{
	for (uint32_t at = 0; at + 4 <= len;) {
		uint16_t tl = (uint16_t)(v[at] | v[at + 1] << 8), tag = (uint16_t)(v[at + 2] | v[at + 3] << 8);

		if (at + 4 + tl > len) {
			break;
		}
		if (tag == 0x24 && tl >= 24) {
			uint32_t e[6];

			memcpy(e, v + at + 4, sizeof(e));
			if (e[0] == 0x8) {
				if (!r5q_link_ready) {
					printf("wifi: scan: on %u MHz\n", e[2]);
				}
			} else if (e[0] != 0x100 && e[0] != 0x4 && (!r5q_link_ready || e[0] != 1)) {
				printf("wifi: scan: event %#x%s, reason %u, %u MHz, scan id %#x\n", e[0],
				    e[0] == 1 ? " STARTED" : e[0] == 2 ? " COMPLETED" : e[0] == 0x10 ? " DEQUEUED" : e[0] == 0x40 ?
				    " START FAILED" : e[0] == 0x20 ? " PREEMPTED" : "", e[1], e[2], e[4]);
			}
			if ((e[0] == 2 || e[0] == 0x40) && r5q_link_ready) {
				scan_running = false;           /* (a scan iOS asked for) */
			} else if (e[0] == 2 || e[0] == 0x40) {
				printf("wifi: scan: done: %u beacons / probe responses, %u networks\n", scan_frames, scan_nets);
				wmi_hold_us = now_us() + 1500000;
				if (join_ssid_len != 0 && !join_have) {
					printf("wifi: join: the wanted network (%u-byte name) was not seen\n", join_ssid_len);
				} else if (join_have && join_step == 0) {
					const uint8_t *r = ie_find(join_bss.ies, join_bss.ielen, 48);

					printf("wifi: join: best BSS of the wanted network: %02x:%02x:%02x:%02x:%02x:%02x, channel %u, %d dBm, "
					    "capability %#x, beacon %u TU, RSN %s\n", join_bss.bssid[0], join_bss.bssid[1], join_bss.bssid[2],
					    join_bss.bssid[3], join_bss.bssid[4], join_bss.bssid[5], join_bss.chan, join_bss.rssi, join_bss.cap,
					    join_bss.bint, r ? "yes" : "no");
					if (r != NULL) {
						htc_show("its RSN element", r, (uint32_t)r[1] + 2);
					}
					join_step = 10;
					wmi_hold_us = now_us() + 20000000;
				}
			}
		}
		at += 4u + tl;
	}
}

static void
r5q_mgmt_rx(const uint8_t *v, uint32_t len)
{
	const uint8_t *frame = NULL;
	uint32_t hdr[10], flen = 0;
	bool have_hdr = false;

	for (uint32_t at = 0; at + 4 <= len;) {
		uint16_t tl = (uint16_t)(v[at] | v[at + 1] << 8), tag = (uint16_t)(v[at + 2] | v[at + 3] << 8);

		if (at + 4 + tl > len) {
			break;
		}
		if (tag == 0x2c && tl >= 24) {
			bzero(hdr, sizeof(hdr));
			memcpy(hdr, v + at + 4, tl < sizeof(hdr) ? tl : sizeof(hdr));
			have_hdr = true;
		} else if (tag == 0x11) {
			frame = v + at + 4;
			flen = tl;
		}
		at += 4u + tl;
	}
	if (!have_hdr || frame == NULL) {
		return;
	}
	if (hdr[4] < flen) {
		flen = hdr[4];
	}
	scan_frames++;
	r5q_join_rx(frame, flen);
	if (flen < 36 || (frame[0] != 0x80 && frame[0] != 0x50)) {      /* beacon, probe response */
		return;
	}
	if (!r5q_link_ready && join_step == 0) {
		r5q_join_consider(frame, flen, (int)hdr[1] - 95, hdr[0]);      /* (never once a join has begun) */
	}
	r5q_heard_add(frame, flen, (int)hdr[1] - 95, hdr[0]);
	if (r5q_link_ready) {
		return;                                 /* (the bring-up's list of networks is not printed again) */
	}
	for (uint32_t i = 0; i < scan_nets && i < 64; i++) {
		if (memcmp(scan_bss[i], frame + 16, 6) == 0) {
			return;
		}
	}
	{
		char ssid[33];
		uint32_t sl = 0, chan = 0;

		for (uint32_t at = 36; at + 2 <= flen;) {
			uint8_t id = frame[at], l = frame[at + 1];

			if (at + 2 + l > flen) {
				break;
			}
			if (id == 0 && l <= 32) {
				for (uint32_t k = 0; k < l; k++) {
					uint8_t c = frame[at + 2 + k];

					ssid[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
				}
				sl = l;
			} else if (id == 3 && l == 1) {
				chan = frame[at + 2];
			}
			at += 2u + l;
		}
		ssid[sl] = 0;
		if (scan_nets < 64) {
			memcpy(scan_bss[scan_nets], frame + 16, 6);
		}
		scan_nets++;
		printf("wifi: scan: network %u: %02x:%02x:%02x:%02x:%02x:%02x channel %u (rx channel %u), %d dBm, %s \"%s\"\n",
		    scan_nets, frame[16], frame[17], frame[18], frame[19], frame[20], frame[21], chan, hdr[0], (int)hdr[1] - 95,
		    frame[0] == 0x80 ? "beacon" : "probe response", sl && ssid[0] ? ssid : "(hidden)");
	}
}

static void
r5q_wmi_event(uint32_t id, const uint8_t *v, uint32_t len, uint8_t eid)
{
	if (id == 0x1) {
		uint32_t mem_reqs = 0;

		for (uint32_t at = 0; at + 4 <= len;) {
			uint16_t tl = (uint16_t)(v[at] | v[at + 1] << 8), tag = (uint16_t)(v[at + 2] | v[at + 3] << 8);
			const uint8_t *val = v + at + 4;

			if (at + 4 + tl > len) {
				break;
			}
			if (tag == 0x20 && tl >= 80) {          /* the service ready structure */
				uint32_t w[20];

				memcpy(w, val, sizeof(w));
				mem_reqs = w[18];
				printf("wifi: wmi: service ready: fw build %#x, abi %#x/%u, phy capability %#x, rf chains %u, ht %#x, vht %#x, "
				    "tx power %u..%u, host memory requests %u, scan channels %u\n", w[0], w[1], w[2], w[7], w[9], w[10], w[11],
				    w[13], w[14], w[18], w[19]);
			} else if (tag == 0x22 && tl >= 16) {  /* a host memory request */
				uint32_t r[4];

				memcpy(r, val, sizeof(r));
				printf("wifi: wmi: host memory request id %u: %u units of %u bytes\n", r[0], r[2], r[1]);
			} else if (tag == 0x10 && tl == 128) {     /* the service bitmap (ath10k wmi_tlv_service) */
				printf("wifi: wmi: services: RX_FULL_REORDER %u, MGMT_TX_WMI %u, STA_PMF_OFFLOAD %u\n", (val[65 / 8] >> (65 % 8)) & 1,
				    (val[76 / 8] >> (76 % 8)) & 1, (val[81 / 8] >> (81 % 8)) & 1);
			} else {
				printf("wifi: wmi: service ready TLV tag %#x, %u bytes\n", tag, tl);
			}
			/* (an array's elements are TLVs of their own: walk into ARRAY_STRUCT 0x12) */
			at += (tag == 0x12) ? 4 : 4 + tl;
		}
		if (!wmi_init_sent) {
			static const uint32_t cfg[44] = {
				4, 33, 0, 0, 2, 66, 16, 7, 7, 100, 100, 100, 40,       /* vdevs .. rx timeouts */
				1, 4, 4, 4, 8, 0, 0, 0, 0x400, 2, 0, 0, 0, 0, 2,       /* decap native wifi .. gtk offload */
				1056, 2, 1, 0x20, 2, 5, 0, 6, 0, 1, 1, 0, 0, 0, 0,    /* msdu descs .. ocb */
				1u << 9,                                               /* host_capab: MGMT_BUNDLE_TX_COMPL */
			};
			uint8_t m[4 + 28 + 4 + 176 + 4];
			uint32_t abi[7] = { 0x01000000, 53, 0x5f414351, 0x00004c4d, 0, 0, 0 };
			uint16_t h[2];

			h[0] = 28;
			h[1] = 0x4a;
			memcpy(m, h, 4);
			memcpy(m + 4, abi, 28);
			h[0] = 176;
			h[1] = 0x4b;
			memcpy(m + 32, h, 4);
			memcpy(m + 36, cfg, 176);
			h[0] = 0;
			h[1] = 0x12;
			memcpy(m + 212, h, 4);
			wmi_init_sent = true;
			r5q_wmi_send(eid, 0x1, m, sizeof(m));
			printf("wifi: wmi: WMI_INIT sent (%zu bytes of TLVs; %u host memory requests left unanswered)\n", sizeof(m),
			    mem_reqs);
		}
	} else if (id == 0x2) {
		for (uint32_t at = 0; at + 4 <= len;) {
			uint16_t tl = (uint16_t)(v[at] | v[at + 1] << 8), tag = (uint16_t)(v[at + 2] | v[at + 3] << 8);
			const uint8_t *val = v + at + 4;

			if (at + 4 + tl > len) {
				break;
			}
			if (tag == 0x23 && tl >= 36) {
				uint32_t status;

				memcpy(&status, val + 32, 4);
				printf("wifi: wmi: WMI READY: MAC %02x:%02x:%02x:%02x:%02x:%02x, status %u, abi %#x\n", val[24], val[25],
				    val[26], val[27], val[28], val[29], status, val[0] | val[1] << 8 | val[2] << 16 | (uint32_t)val[3] << 24);
				if (status == 0 && wmi_step == 0) {
					uint32_t scan = 1;

					PE_parse_boot_argn("wifi_scan", &scan, sizeof(scan));
					if (scan) {
						uint32_t htt = 1;

						PE_parse_boot_argn("wifi_htt", &htt, sizeof(htt));
						r5q_join_args();
						r5q_wifi_key_load();
						wmi_step = htt ? 1 : 3;
						wmi_hold_us = now_us() + 15000000;
						printf("wifi: wmi: %d WMI credit(s) left; scanning next (wifi_scan=0 stops here)\n", wmi_credits);
					}
				}
			}
			at += 4 + tl;
		}
	} else if (id == 0x3001) {
		r5q_scan_event(v, len);
	} else if (id == 0x7001) {
		r5q_mgmt_rx(v, len);
	} else if (id == 0x5001 && len >= 20) {         /* VDEV_START_RESP: tag 0x28 {vdev, req, type, status} */
		uint32_t e[4];

		memcpy(e, v + 4, sizeof(e));
		printf("wifi: join: VDEV_START response: vdev %u, type %u, status %u\n", e[0], e[2], e[3]);
		if (join_step == 11 && e[3] == 0) {
			join_ev = true;
		}
	} else if (id == 0x7006 && len >= 12) {         /* MGMT_TX_COMPLETION: tag 0x1a7 {desc, status, ...} */
		uint32_t e[2];

		memcpy(e, v + 4, sizeof(e));
		printf("wifi: join: frame %u sent: status %u%s\n", e[0], e[1], e[1] == 0 ? " (acknowledged)" : "");
	} else if (id == 0x7007 || id == 0x6001) {
		htc_show(id == 0x6001 ? "wmi PEER_STA_KICKOUT" : "wmi MGMT_TX_BUNDLE_COMPLETION", v, len);
	} else if (id != 0x1 && id != 0x3 && id != 0x4009 && id != 0x4022 && id != 0x3a001) {
		htc_show("wmi other event (TLVs)", v, len);
	}
}

/*
 * (Test 120) One look at the rings: what the bring-up loop did each millisecond, and from the moment the link is handed
 * over what the net thread (redwood_net.c) has done every 2-10 ms: CE2 (HTC control, WMI events) and CE1 (HTT: rx
 * indications, transmit completions), the next WMI step, renewed keys to install. Returns how many frames there were.
 */
static unsigned
r5q_service(void)
{
	uint32_t len;
	const uint8_t *p;
	unsigned frames = 0;
	struct r5q_ce *rings[2] = { &ce2, &ce1 };

	for (int r = 0; r < 2; r++) {
		while ((p = ce_recv(rings[r], &len)) != NULL) {
			uint8_t eid = p[0];

			frames++;
			uint16_t hl = (uint16_t)(p[2] | p[3] << 8), msg = (uint16_t)(p[8] | p[9] << 8);

			uint16_t pl = (uint16_t)((p[1] & 0x2) && p[4] <= hl ? hl - p[4] : hl);    /* payload without the trailer */
			uint32_t evid = (uint32_t)(p[8] | p[9] << 8 | p[10] << 16);
			bool quiet = (eid != 0 && eid == svc_wmi_eid && (evid == 0x7001 || evid == 0x3001)) || (eid != 0 && eid == svc_htt_eid);

			if (!quiet) {
				printf("wifi: htc: +%llu ms: CE%u frame, endpoint %u, flags %#x, length %u\n", (now_us() - svc_t0) / 1000,
				    rings[r]->id, eid, p[1], hl);
			}
			htc_trailer(p, hl, svc_wmi_eid);
			if (eid == 0 && msg == 1) {
				printf("wifi: htc: HTC READY: credits %u of %u bytes, %u endpoints, version %u, bundle %u\n",
				    p[10] | p[11] << 8, p[12] | p[13] << 8, p[14], len >= 18 ? p[16] : 0, len >= 18 ? p[17] : 0);
				svc_ready = true;
				svc_credits = p[10];
			} else if (eid == 0 && msg == 3) {
				uint16_t svc = (uint16_t)(p[10] | p[11] << 8);

				printf("wifi: htc: connected service %#x: status %u, endpoint %u, max message %u\n", svc, p[12], p[13],
				    p[14] | p[15] << 8);
				if (p[12] == 0 && svc == 0x100) {
					svc_wmi_eid = p[13];
					wmi_credits = svc_credits;
				} else if (p[12] == 0 && svc == 0x300) {
					svc_htt_eid = p[13];
					htt_ep = svc_htt_eid;
				}
			} else if (eid != 0 && eid == svc_htt_eid) {
				r5q_htt_msg(p, pl);
			} else if (eid != 0 && eid == svc_wmi_eid) {
				uint32_t id = (uint32_t)(p[8] | p[9] << 8 | p[10] << 16);

				if (!quiet) {
					printf("wifi: wmi: +%llu ms: EVENT %#x%s (%u bytes)\n", (now_us() - svc_t0) / 1000, id,
					    id == 0x1 ? " = SERVICE READY" : id == 0x2 ? " = READY" : id == 0x3 ? " = SERVICE AVAILABLE" : "",
					    hl);        /* (ath10k wmi-tlv.h: WMI_TLV_SERVICE_READY_EVENTID 0x1, READY 0x2, ...) */
					htc_show("wmi event", p + 8, pl);
				}
				r5q_wmi_event(id, p + 12, pl >= 4 ? pl - 4 : 0, svc_wmi_eid);
			} else {
				htc_show("frame", p, len);
			}
		}
	}
	if (svc_ready) {
		uint8_t conn[8] = { 2, 0, 0x00, 0x03, 0, 0, 0, 0 };   /* CONNECT_SERVICE: HTT data (0x300) */
		/*
		 * ... WMI control (0x100), with all the transmit credits in RECV_ALLOC (flags bits 15:8), as ath10k's
		 * ath10k_htc_get_credit_allocation: asked for with none (Test 115), the target posted no buffers for WMI's pipe
		 */
		uint8_t wmi[8] = { 2, 0, 0x00, 0x01, 0, svc_credits, 0, 0 };

		svc_ready = false;
		ce_send_htc(&ce0, 0x0, 0, conn, sizeof(conn));
		ce_send_htc(&ce0, 0x0, 0, wmi, sizeof(wmi));
		printf("wifi: htc: asked to connect HTT and WMI\n");
	}
	if (svc_wmi_eid != 0xff && svc_htt_eid != 0xff && htc_seq == 2) {
		uint8_t setup[12] = { 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };     /* SETUP_COMPLETE_EX, no bundling */

		ce_send_htc(&ce0, 0x0, 0, setup, sizeof(setup));
		printf("wifi: htc: setup complete sent (WMI endpoint %u, HTT endpoint %u); waiting for WMI's events\n", svc_wmi_eid,
		    svc_htt_eid);
	}
	if (svc_wmi_eid != 0xff) {
		r5q_wmi_next(svc_wmi_eid);
	}
	if (wpa_rekey_us != 0 && now_us() - wpa_rekey_us > 60000 && svc_wmi_eid != 0xff) {
		wpa_rekey_us = 0;
		r5q_install_key(svc_wmi_eid, 0, 0, wpa_ptk + 32, 16);
		bzero(wpa_ptk + 32, 16);
		if (wpa_gtk_len != 0) {
			r5q_install_key(svc_wmi_eid, wpa_gtk_idx, 1, wpa_gtk, wpa_gtk_len);
		}
		bzero(wpa_gtk, sizeof(wpa_gtk));
		printf("wifi: wpa: renewed pairwise key installed\n");
	}
	if (r5q_link_ready) {
		static uint64_t said_us;
		uint64_t now = now_us();

		if (now - said_us > 30000000) {
			char a[24];

			said_us = now;
			r5q_en0_address(a, sizeof(a));
			r5q_flows_say();
			printf("wifi: net: frames sent %u (completed %u, %u not ok, dropped %u), received to en0 %u, rx buffers %u; "
			    "step %u; en0's IPv4 address: %s\n", tx_sent, tx_compl, tx_compl_bad, tx_drops, rx_to_net, htt_msdus,
			    join_step, a);
			/* (Test 123) the receive rings: our read/write slots, the engine's write index, the next slot's length */
			printf("wifi: net: CE1 sw %u wr %u hw %u len@sw %u; CE2 sw %u wr %u hw %u; CE4 wr %u read %u; rx ring %u\n",
			    ce1.sw, ce1.wr, rd(wlan_regs + CE_REG(1, CE_CUR_DRRI)), ce1.ring[16 * ce1.sw + 8] | ce1.ring[16 * ce1.sw + 9] << 8,
			    ce2.sw, ce2.wr, rd(wlan_regs + CE_REG(2, CE_CUR_DRRI)), ce4.wr, rd(wlan_regs + CE_REG(4, CE_CUR_SRRI)), rxr_alloc);
		}
	}
	return frames;
}

/*
 * (Test 120) en0 (bsd/dev/redwood_net.c, the donor's) on this radio: made as the kernel starts, as iOS looks for its
 * Wi-Fi interface once (the address is ours, fixed), its link up once the keys are in. Frames are Ethernet there and
 * native wifi here: out, an 802.11 data header (to DS, protected: the firmware encrypts with the installed key;
 * addr1 the BSSID, addr2 us, addr3 the destination) and LLC/SNAP with the type; in, the reverse (from DS: addr1 the
 * destination, addr3 the source). The rings are the net thread's from the handover on (r5q_link_ready); a lock keeps
 * its poll and the stack's transmit apart.
 */
static LCK_GRP_DECLARE(r5q_net_grp, "r5q-wifi-net");
static LCK_MTX_DECLARE(r5q_net_lock, &r5q_net_grp);
static bool r5q_net_attached;

/*
 * (Test 121) What goes through en0, from here (bsd/dev/redwood_net.c's own trace is not switched on in this kernel):
 * DHCP both ways (option 53's type, the address offered), the names asked of DNS and the TCP connections begun
 * (the first 40), and en0's IPv4 address in the 30-second line.
 */
/*
 * (Test 127) Each TCP connection through en0, summed: bytes and packets each way, what the client sent after the
 * server's first data (a TLS client that stops there is waiting on its certificate check), and how it ended. Said
 * when it ends (FIN / RST) and, for those still open, every 30 s (r5q_flows_say).
 */
struct r5q_flow {
	uint8_t rip[4];
	uint16_t lport, rport;
	uint32_t out_b, in_b, out_p, in_p, out_after_in, first_out, first_in;
	uint64_t t0, last;
	uint8_t end;            /* 0 open, 'F' fin, 'R' rst */
	bool said;
};
#define R5Q_FLOWS 48
static struct r5q_flow flows[R5Q_FLOWS];
static uint32_t flows_said;

static void
r5q_flow_say(struct r5q_flow *f, const char *why)
{
	if (flows_said >= 400) {
		return;
	}
	flows_said++;
	printf("wifi: tcp: %u.%u.%u.%u:%u <- :%u %s after %llu ms: out %u B / %u pkts (first data %u B), in %u B / %u pkts "
	    "(first %u B), out after the first in: %u B\n", f->rip[0], f->rip[1], f->rip[2], f->rip[3], f->rport, f->lport, why,
	    (f->last - f->t0) / 1000, f->out_b, f->out_p, f->first_out, f->in_b, f->in_p, f->first_in, f->out_after_in);
}

static void
r5q_flows_say(void)
{
	for (int i = 0; i < R5Q_FLOWS; i++) {
		if (flows[i].t0 != 0 && !flows[i].said && flows[i].end == 0) {
			r5q_flow_say(&flows[i], "still open");
		}
	}
}

static void
r5q_flow(const uint8_t *ip, const uint8_t *l4, const uint8_t *end, bool out)
{
	const uint8_t *rip = out ? ip + 16 : ip + 12;
	uint16_t lport = out ? (uint16_t)(l4[0] << 8 | l4[1]) : (uint16_t)(l4[2] << 8 | l4[3]);
	uint16_t rport = out ? (uint16_t)(l4[2] << 8 | l4[3]) : (uint16_t)(l4[0] << 8 | l4[1]);
	uint32_t iplen = (uint32_t)(ip[2] << 8 | ip[3]), hl = 4u * (ip[0] & 0xf), th = 4u * (l4[12] >> 4), data;
	uint8_t fl = l4[13];
	struct r5q_flow *f = NULL, *old = NULL;
	uint64_t now = now_us();

	if (l4 + 20 > end || iplen < hl + th) {
		return;
	}
	data = iplen - hl - th;
	for (int i = 0; i < R5Q_FLOWS; i++) {
		if (flows[i].t0 != 0 && flows[i].lport == lport && flows[i].rport == rport && memcmp(flows[i].rip, rip, 4) == 0) {
			f = &flows[i];
			break;
		}
		if (old == NULL || flows[i].t0 == 0 || (old->t0 != 0 && flows[i].last < old->last)) {
			old = &flows[i];
		}
	}
	if (f == NULL) {
		if (!(out && (fl & 0x12) == 0x02)) {
			return;                 /* only connections seen from their SYN */
		}
		f = old;
		bzero(f, sizeof(*f));
		memcpy(f->rip, rip, 4);
		f->lport = lport;
		f->rport = rport;
		f->t0 = now;
	}
	f->last = now;
	if (out) {
		f->out_p++;
		f->out_b += data;
		if (data != 0 && f->first_out == 0) {
			f->first_out = data;
		}
		if (f->in_b != 0) {
			f->out_after_in += data;
		}
	} else {
		f->in_p++;
		f->in_b += data;
		if (data != 0 && f->first_in == 0) {
			f->first_in = data;
		}
	}
	if ((fl & 0x05) != 0 && f->end == 0) {
		f->end = (fl & 0x04) ? 'R' : 'F';
		f->said = true;
		r5q_flow_say(f, (fl & 0x04) ? (out ? "RESET by us" : "RESET by them") : (out ? "closed by us" : "closed by them"));
	}
}

static void
r5q_sniff(const uint8_t *e, size_t len, bool out)
{
	static uint32_t said;
	const uint8_t *ip = e + 14, *l4, *end = e + len;
	uint16_t sport, dport;

	if (len < 14 + 28 || e[12] != 0x08 || e[13] != 0x00) {
		return;
	}
	l4 = ip + 4 * (ip[0] & 0xf);
	if (l4 + 8 > end) {
		return;
	}
	sport = (uint16_t)(l4[0] << 8 | l4[1]);
	dport = (uint16_t)(l4[2] << 8 | l4[3]);
	if (ip[9] == 6) {
		r5q_flow(ip, l4, end, out);
	}
	if (ip[9] == 17 && (sport == 67 || sport == 68) && (dport == 67 || dport == 68) && l4 + 8 + 240 <= end) {
		const uint8_t *b = l4 + 8, *o = b + 240;
		unsigned type = 0;

		while (o + 2 <= end && o[0] != 255) {
			if (o[0] == 0) {
				o++;
				continue;
			}
			if (o[0] == 53 && o[1] >= 1 && o + 3 <= end) {
				type = o[2];
			}
			o += 2 + o[1];
		}
		printf("wifi: net: DHCP %s %s: your address %u.%u.%u.%u, server %u.%u.%u.%u\n", out ? "sent" : "received",
		    type == 1 ? "DISCOVER" : type == 2 ? "OFFER" : type == 3 ? "REQUEST" : type == 5 ? "ACK" : type == 6 ? "NAK" :
		    type == 7 ? "RELEASE" : type == 8 ? "INFORM" : "(other)", b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23]);
		return;
	}
	if (!out || said >= 200) {
		return;
	}
	if (ip[9] == 17 && dport == 53 && l4 + 8 + 12 < end) {
		char name[96];
		const uint8_t *label = l4 + 8 + 12;
		size_t n = 0;

		while (label < end && *label != 0 && *label < 64 && label + 1 + *label < end && n + *label + 2 < sizeof(name)) {
			memcpy(name + n, label + 1, *label);
			n += *label;
			name[n++] = '.';
			label += 1 + *label;
		}
		name[n ? n - 1 : 0] = 0;
		said++;
		printf("wifi: net: DNS: asks for %s (of %u.%u.%u.%u)\n", name, ip[16], ip[17], ip[18], ip[19]);
	} else if (ip[9] == 6 && l4 + 14 <= end && (l4[13] & 0x12) == 0x02) {
		said++;
		printf("wifi: net: TCP: connects to %u.%u.%u.%u port %u\n", ip[16], ip[17], ip[18], ip[19], dport);
	}
}

static void
r5q_en0_address(char *out, size_t size)
{
	ifnet_t ifp = NULL;
	ifaddr_t *list = NULL;

	snprintf(out, size, "none");
	if (ifnet_find_by_name("en0", &ifp) != 0 || ifp == NULL) {
		snprintf(out, size, "no en0");
		return;
	}
	if (ifnet_get_address_list_family(ifp, &list, AF_INET) == 0 && list != NULL) {
		for (int i = 0; list[i] != NULL; i++) {
			struct sockaddr_in sin;

			if (ifaddr_address(list[i], (struct sockaddr *)&sin, sizeof(sin)) == 0 && sin.sin_family == AF_INET) {
				const uint8_t *a = (const uint8_t *)&sin.sin_addr;

				snprintf(out, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
				break;
			}
		}
		ifnet_free_address_list(list);
	}
	ifnet_release(ifp);
}

static unsigned
r5q_poll(void)
{
	unsigned n;

	if (!r5q_link_ready) {
		return 0;
	}
	lck_mtx_lock(&r5q_net_lock);
	n = r5q_service();
	lck_mtx_unlock(&r5q_net_lock);
	return n;
}

static bool
r5q_transmit(mbuf_t packet, size_t length)
{
	static uint8_t eth[REDWOOD_NET_FRAME_MAX], f[REDWOOD_NET_FRAME_MAX + 32];
	bool ok;

	if (!r5q_link_ready || join_step != 24 || length < 14 || length > REDWOOD_NET_FRAME_MAX ||
	    mbuf_copydata(packet, 0, length, eth) != 0) {
		return false;
	}
	bzero(f, 32);
	f[0] = 0x08;                            /* data */
	f[1] = 0x41;                            /* to DS, protected */
	memcpy(f + 4, join_bss.bssid, 6);
	memcpy(f + 10, r5q_mac, 6);
	memcpy(f + 16, eth, 6);
	f[24] = 0xaa;
	f[25] = 0xaa;
	f[26] = 0x03;
	f[30] = eth[12];
	f[31] = eth[13];
	memcpy(f + 32, eth + 14, length - 14);
	r5q_sniff(eth, length, true);
	lck_mtx_lock(&r5q_net_lock);
	ok = r5q_htt_tx(f, (uint32_t)(32 + length - 14), 0);
	lck_mtx_unlock(&r5q_net_lock);
	return ok;
}

static void
r5q_rx_to_net(const uint8_t *f, uint32_t ml)
{
	static uint8_t eth[REDWOOD_NET_FRAME_MAX];
	const uint8_t *da, *sa;

	if (!r5q_net_attached || ml < 32 || ml - 32 + 14 > REDWOOD_NET_FRAME_MAX || (f[0] & 0x0c) != 0x08 ||
	    f[24] != 0xaa || f[25] != 0xaa || f[26] != 0x03) {
		return;
	}
	if ((f[1] & 3) == 2) {
		da = f + 4;
		sa = f + 16;
	} else if ((f[1] & 3) == 0) {
		da = f + 4;
		sa = f + 10;
	} else {
		return;
	}
	if (memcmp(sa, r5q_mac, 6) == 0) {
		return;                         /* our own multicast, back from the access point */
	}
	memcpy(eth, da, 6);
	memcpy(eth + 6, sa, 6);
	eth[12] = f[30];
	eth[13] = f[31];
	memcpy(eth + 14, f + 32, ml - 32);
	rx_to_net++;
	r5q_sniff(eth, ml - 32 + 14, false);
	redwood_net_input(eth, ml - 32 + 14);
}

static struct redwood_net_link r5q_link = {
	.name = "wcn3990",
	.wireless = true,
	.up = false,
	.transmit = r5q_transmit,
	.poll = r5q_poll,
};

static void
r5q_net_early(void)
{
	memcpy(r5q_link.address, r5q_mac, 6);
	r5q_net_attached = redwood_net_attach(&r5q_link);
}

/*
 * (Test 122) The radio as iOS's Wi-Fi side wants it: the donor's station boundary (sys/redwood_sta.h,
 * bsd/dev/redwood_sta.c) calls these on its own thread, one at a time, and turns the outcomes into Apple80211 events
 * for wifid (bsd/dev/redwood_apple80211.c). A scan is a START_SCAN on the station vdev (off its channel while joined)
 * whose beacons fill the list heard() gives; a join leaves whatever network there was (deauthentication, VDEV_DOWN
 * 0x5007, PEER_DELETE 0x6002, VDEV_STOP 0x5006), takes the strongest access point of that name from the last scan
 * (scanning first if it is not there) and runs the join of Tests 118-119 from VDEV_START on, the key made with
 * PBKDF2 when a passphrase is given (Settings). The network joined at the start from the stored key is handed over as
 * the "standing" network: asked for again, it is found joined already, and wifid hears it was joined.
 */
static uint64_t heard_us[R5Q_HEARD];
static uint32_t heard_n, scan_seq = 1;

static void
r5q_heard_add(const uint8_t *f, uint32_t flen, int rssi, uint32_t rx_chan)
{
	struct redwood_sta_bss *b;
	const uint8_t *e, *ies = f + 36;
	uint32_t i, il = flen - 36, ch = rx_chan;

	if (heard_tab == NULL || flen < 36) {
		return;
	}
	for (i = 0; i < heard_n && memcmp(heard_tab[i].bssid, f + 16, 6) != 0; i++) {
	}
	if (i == heard_n) {
		if (heard_n == R5Q_HEARD) {
			return;
		}
		heard_n++;
	}
	b = &heard_tab[i];
	bzero(b, sizeof(*b));
	memcpy(b->bssid, f + 16, 6);
	if ((e = ie_find(ies, il, 0)) != NULL && e[1] <= 32) {
		memcpy(b->ssid, e + 2, e[1]);
		b->ssid_length = e[1];
	}
	if ((e = ie_find(ies, il, 3)) != NULL && e[1] == 1) {
		ch = e[2];
	} else if ((e = ie_find(ies, il, 61)) != NULL && e[1] >= 1) {
		ch = e[2];
	}
	b->frequency = (uint16_t)chan_mhz(ch);
	b->rssi = (int16_t)rssi;
	b->beacon_interval = (uint16_t)(f[32] | f[33] << 8);
	b->capability = (uint16_t)(f[34] | f[35] << 8);
	b->elements_length = (uint16_t)(il < REDWOOD_STA_ELEMENTS_MOST ? il : REDWOOD_STA_ELEMENTS_MOST);
	memcpy(b->elements, ies, b->elements_length);
	heard_us[i] = now_us();
}

/* START_SCAN again (the channel list went to the firmware at the start) */
static void
r5q_scan_send(uint8_t eid)
{
	static uint8_t m[600];
	uint32_t s[25], ch[R5Q_SCAN_N], bssid[2] = { 0xffffffffu, 0xffffu }, n = 0;

	bzero(s, sizeof(s));
	scan_seq = (scan_seq + 1) & 0xfff;
	s[0] = 0xA000 | scan_seq;
	s[1] = 0xA000 | 1;
	s[3] = 1;
	s[4] = 0x1ff;
	s[5] = 50;
	s[6] = 110;                     /* passive dwell: a beacon interval and a bit */
	s[7] = 50;
	s[8] = 500;
	s[12] = 20000;
	s[13] = 5;
	s[14] = 0x1 ^ 0x20;
	s[16] = R5Q_SCAN_N;
	s[17] = 1;
	s[20] = 3;
	for (uint32_t i = 0; i < R5Q_SCAN_N; i++) {
		ch[i] = r5q_scan_mhz[i];
	}
	n += tlv_put(m + n, 0x4d, s, sizeof(s));
	n += tlv_put(m + n, 0x10, ch, sizeof(ch));
	n += tlv_put(m + n, 0x13, NULL, 0);
	n += tlv_put(m + n, 0x13, bssid, sizeof(bssid));
	n += tlv_put(m + n, 0x11, NULL, 0);
	r5q_wmi_send(eid, 0x3001, m, (uint16_t)n);
}

static int
r5q_sta_scan(__unused const struct redwood_sta_scan *what)
{
	uint64_t until;

	if (!r5q_link_ready || svc_wmi_eid == 0xff) {
		return ENXIO;
	}
	lck_mtx_lock(&r5q_net_lock);
	heard_n = 0;
	scan_running = true;
	r5q_scan_send(svc_wmi_eid);
	lck_mtx_unlock(&r5q_net_lock);
	for (until = now_us() + 15000000; scan_running && now_us() < until;) {
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	printf("wifi: sta: scan %s: %u access points heard\n", scan_running ? "TIMED OUT" : "done", heard_n);
	if (scan_running) {
		scan_running = false;
		return ETIMEDOUT;
	}
	return 0;
}

static void
r5q_sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	static struct redwood_sta_bss one;
	uint64_t now = now_us();

	for (uint32_t i = 0;; i++) {
		lck_mtx_lock(&r5q_net_lock);
		if (i >= heard_n) {
			lck_mtx_unlock(&r5q_net_lock);
			break;
		}
		one = heard_tab[i];
		one.age_ms = (uint32_t)((now - heard_us[i]) / 1000);
		lck_mtx_unlock(&r5q_net_lock);
		each(&one, context);
	}
}

static int
r5q_sta_leave(void)
{
	uint8_t m[64], why[2] = { 3, 0 };       /* deauthentication: leaving */
	uint32_t v[3] = { 0, 0, 0 }, n;
	uint8_t was;

	lck_mtx_lock(&r5q_net_lock);
	was = join_step;
	if (was == 0 || svc_wmi_eid == 0xff) {
		lck_mtx_unlock(&r5q_net_lock);
		return 0;
	}
	if (was >= 16 && was <= 24) {
		r5q_mgmt_tx(svc_wmi_eid, 0xc0, why, sizeof(why));
	}
	if (was == 24) {
		redwood_net_link_state(false);
	}
	n = tlv_put(m, 0x5e, v, 4);                     /* VDEV_DOWN */
	r5q_wmi_send(svc_wmi_eid, 0x5007, m, (uint16_t)n);
	memcpy(&v[1], join_bss.bssid, 6);
	n = tlv_put(m, 0x62, v, 12);                    /* PEER_DELETE */
	r5q_wmi_send(svc_wmi_eid, 0x6002, m, (uint16_t)n);
	v[1] = v[2] = 0;
	n = tlv_put(m, 0x5d, v, 4);                     /* VDEV_STOP */
	r5q_wmi_send(svc_wmi_eid, 0x5006, m, (uint16_t)n);
	join_step = 0;
	r5q_keyed = false;
	wpa_have_snonce = false;
	lck_mtx_unlock(&r5q_net_lock);
	delay_for_interval(300, NSEC_PER_MSEC);
	printf("wifi: sta: left the network (it was at step %u)\n", was);
	return 0;
}

static int
r5q_sta_join(const struct redwood_sta_join *w)
{
	static uint8_t pmk[32];
	bool psk = w->security == REDWOOD_STA_WPA2_PSK, again = false;
	struct redwood_sta_bss *b = NULL;
	uint64_t until;
	int error;

	if (!r5q_link_ready || svc_wmi_eid == 0xff) {
		return ENXIO;
	}
	if (w->ssid_length == 0 || w->ssid_length > 32) {
		return EINVAL;
	}
	if (psk && w->passphrase_length != 0) {
		if (w->passphrase_length < 8 || w->passphrase_length > 63) {
			return EAUTH;
		}
		wpa_pbkdf2_sha1(w->passphrase, w->passphrase_length, w->ssid, w->ssid_length, 4096, pmk, 32);
	} else if (psk) {
		memcpy(pmk, w->pmk, 32);
	}
	lck_mtx_lock(&r5q_net_lock);
	again = join_step == 24 && w->ssid_length == join_ssid_len && memcmp(w->ssid, join_ssid, join_ssid_len) == 0 &&
	    (!psk || (wpa_have_pmk && memcmp(pmk, wpa_pmk, 32) == 0));
	lck_mtx_unlock(&r5q_net_lock);
	if (again) {
		bzero(pmk, sizeof(pmk));
		printf("wifi: sta: asked to join the network it is on: it is joined\n");
		return 0;
	}
	r5q_sta_leave();
	for (int pass = 0; pass < 2 && b == NULL; pass++) {
		if (pass == 1 && r5q_sta_scan(NULL) != 0) {
			break;
		}
		lck_mtx_lock(&r5q_net_lock);
		for (uint32_t i = 0; i < heard_n; i++) {
			struct redwood_sta_bss *h = &heard_tab[i];

			if (h->ssid_length == w->ssid_length && memcmp(h->ssid, w->ssid, w->ssid_length) == 0 &&
			    now_us() - heard_us[i] < 120000000ull && (b == NULL || h->rssi > b->rssi)) {
				b = h;
			}
		}
		if (b != NULL) {
			bzero(&join_bss, sizeof(join_bss));
			memcpy(join_bss.bssid, b->bssid, 6);
			join_bss.chan = (uint8_t)(b->frequency < 3000 ? (b->frequency == 2484 ? 14 : (b->frequency - 2407) / 5) :
			    (b->frequency - 5000) / 5);
			join_bss.rssi = b->rssi;
			join_bss.cap = b->capability;
			join_bss.bint = b->beacon_interval;
			join_bss.ielen = (uint16_t)(b->elements_length < sizeof(join_bss.ies) ? b->elements_length : sizeof(join_bss.ies));
			memcpy(join_bss.ies, b->elements, join_bss.ielen);
			memcpy(join_ssid, w->ssid, w->ssid_length);
			join_ssid_len = w->ssid_length;
			bzero(wpa_pmk, sizeof(wpa_pmk));
			if (psk) {
				memcpy(wpa_pmk, pmk, 32);
			}
			wpa_have_pmk = psk;
			wpa_have_snonce = false;
			wpa_dry_sent = false;
			r5q_keyed = false;
			join_error = 0;
			join_tries = 0;
			join_have = true;
			join_step = 10;
		}
		lck_mtx_unlock(&r5q_net_lock);
	}
	bzero(pmk, sizeof(pmk));
	if (b == NULL) {
		printf("wifi: sta: the network asked for was not heard\n");
		return ENOENT;
	}
	for (until = now_us() + 25000000; now_us() < until && join_step != 24 && join_step != 30;) {
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	if (join_step == 24) {
		redwood_net_link_state(true);
		printf("wifi: sta: JOINED (asked by iOS); en0's link is up\n");
		return 0;
	}
	error = join_step == 30 ? (join_error != 0 ? join_error : ECONNREFUSED) : ETIMEDOUT;
	printf("wifi: sta: the join failed at step %u: error %d\n", join_step, error);
	r5q_sta_leave();
	return error;
}

static int
r5q_sta_power(bool on)
{
	if (!on) {
		r5q_sta_leave();
	}
	printf("wifi: sta: power %s asked\n", on ? "on" : "off");
	return 0;
}

static void
r5q_sta_state(struct redwood_sta_state *st)
{
	uint8_t step = join_step;

	bzero(st, sizeof(*st));
	st->phase = step == 24 ? REDWOOD_STA_JOINED : (step >= 10 && step < 24) ? REDWOOD_STA_JOINING : REDWOOD_STA_IDLE;
	st->last_error = join_error;
	if (step >= 10 && step <= 24) {
		memcpy(st->bss.bssid, join_bss.bssid, 6);
		memcpy(st->bss.ssid, join_ssid, join_ssid_len);
		st->bss.ssid_length = join_ssid_len;
		st->bss.frequency = (uint16_t)chan_mhz(join_bss.chan);
		st->bss.rssi = (int16_t)join_bss.rssi;
		st->bss.beacon_interval = join_bss.bint;
		st->bss.capability = join_bss.cap;
		st->bss.elements_length = join_bss.ielen;
		memcpy(st->bss.elements, join_bss.ies, join_bss.ielen);
	}
}

static unsigned
r5q_sta_channels(struct redwood_sta_channel *c, unsigned most)
{
	unsigned n = 0;

	for (uint32_t i = 0; i < R5Q_SCAN_N && n < most; i++, n++) {
		uint16_t f = r5q_scan_mhz[i];

		c[n].frequency = f;
		c[n].flags = (f >= 5260 && f <= 5700) ? (REDWOOD_STA_CHANNEL_PASSIVE | REDWOOD_STA_CHANNEL_RADAR) : 0;
	}
	return n;
}

static struct redwood_sta_ops r5q_sta_ops = {
	.power = r5q_sta_power,
	.scan = r5q_sta_scan,
	.heard = r5q_sta_heard,
	.join = r5q_sta_join,
	.leave = r5q_sta_leave,
	.state = r5q_sta_state,
	.channels = r5q_sta_channels,
};

/*
 * (Test 128) Safari's page comes in over Wi-Fi (Test 127: example.com's 2.5 KB) and is never drawn, while
 * Settings works. What its processes are doing, from here: every 30 s, for MobileSafari, the WebKit helpers,
 * trustd, apsd and runningboardd -- the task's suspend count (RunningBoard suspends what it does not keep
 * running), its threads' states, CPU time (busy or idle), and the main thread's state and priority.
 */
struct r5q_pti {
	uint64_t ut, st;
	int32_t cpu, policy, run_state, flags, sleep, curpri, pri, maxpri;
	char name[64];
};
extern int fill_taskthreadinfo(task_t task, uint64_t thaddr, bool thuniqueid, struct r5q_pti *ptinfo, void *vpp, int *vidp);
extern int fill_taskthreadlist(task_t task, void *buffer, int thcount, bool thuniqueid);
extern task_t proc_task(proc_t proc);
extern char *proc_best_name(proc_t p);
extern kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *count);

static bool
r5q_has(const char *s, const char *w)
{
	size_t n = strlen(w);

	for (; *s; s++) {
		if (strncmp(s, w, n) == 0) {
			return true;
		}
	}
	return false;
}

static void
r5q_procs_say(void)
{
	static const char *const want[] = { "MobileSafari", "WebKit", "WebContent", "trustd", "apsd", "runningboardd" };
	static uint64_t ids[256];

	for (int pid = 1; pid < 2000; pid++) {
		proc_t p = proc_find(pid);
		const char *name;
		bool match = false;

		if (p == NULL) {
			continue;
		}
		name = proc_best_name(p);
		for (unsigned i = 0; name != NULL && i < sizeof(want) / sizeof(want[0]); i++) {
			match |= r5q_has(name, want[i]);
		}
		if (match) {
			task_t task = proc_task(p);
			struct mach_task_basic_info bi;
			mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
			int n, states[6] = { 0 };
			uint64_t cpu = 0;
			struct r5q_pti main_t, t;

			bzero(&bi, sizeof(bi));
			bzero(&main_t, sizeof(main_t));
			task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&bi, &count);
			n = fill_taskthreadlist(task, ids, 256, true) / (int)sizeof(uint64_t);
			for (int k = 0; k < n; k++) {
				bzero(&t, sizeof(t));
				if (fill_taskthreadinfo(task, ids[k], true, &t, NULL, NULL) == 0) {
					states[t.run_state >= 1 && t.run_state <= 5 ? t.run_state : 0]++;
					cpu += t.ut + t.st;
					if (k == 0) {
						main_t = t;
					}
				}
			}
			printf("wifi: proc: %d %s: suspend %d, %d threads (run %d wait %d stop %d unint %d halt %d), cpu %llu ms; "
			    "main: state %d, sleep %d s, pri %d, cpu %llu ms\n", pid, name, bi.suspend_count, n, states[1], states[3],
			    states[2], states[4], states[5], cpu / 1000000, main_t.run_state, main_t.sleep, main_t.curpri,
			    (main_t.ut + main_t.st) / 1000000);
		}
		proc_rele(p);
	}
}

static void
r5q_procs_thread(__unused void *param, __unused wait_result_t wr)
{
	for (int i = 0; i < 40; i++) {
		delay_for_interval(30000, NSEC_PER_MSEC);
		printf("wifi: proc: --- %d s after the link ---\n", 30 * (i + 1));
		r5q_procs_say();
	}
	thread_terminate(current_thread());
}

static void
r5q_sta_start(void)
{
	{
		thread_t th;
		uint32_t watch = 0;             /* (Test 137: a diagnostic, now on request only: wifi_procs=1) */

		if (PE_parse_boot_argn("wifi_procs", &watch, sizeof(watch)) && watch != 0 &&
		    kernel_thread_start(r5q_procs_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
	uint32_t on = 1;

	if (PE_parse_boot_argn("wifi_sta", &on, sizeof(on)) && on == 0) {
		return;
	}
	if (r5q_sta_attached) {
		printf("wifi: sta: (again after a restart: the station stays attached)\n");
		return;
	}
	memcpy(r5q_sta_ops.address, r5q_mac, 6);
	redwood_sta_attach(&r5q_sta_ops);
	r5q_sta_attached = true;
	printf("wifi: sta: the radio is the station for iOS's Wi-Fi (redwood_sta)\n");
	if (join_step == 24 && wpa_have_pmk) {
		static struct redwood_sta_join j;

		bzero(&j, sizeof(j));
		memcpy(j.ssid, join_ssid, join_ssid_len);
		j.ssid_length = join_ssid_len;
		j.security = REDWOOD_STA_WPA2_PSK;
		memcpy(j.pmk, wpa_pmk, 32);
		printf("wifi: sta: the network joined at the start is the standing one: %d\n", redwood_sta_ask_join_standing(&j));
		bzero(&j, sizeof(j));
	}
}

static void
r5q_htc_bringup(void)
{
	uint32_t misc;

	uint32_t smmu = 0;
	static bool smmu_ok;
	bool again = wlan_regs != 0;            /* (Test 172) after the Wi-Fi domain restarted */

	svc_t0 = now_us();
	if (!again) {
		wlan_regs = ml_io_map(R5Q_WLAN_PHYS + R5Q_WLAN_CE_OFF, 0x10000);
		wlan_shadow = ml_io_map(R5Q_WLAN_PHYS + R5Q_WLAN_SHADOW_OFF, 0x1000);
	}
	if (!again && PE_parse_boot_argn("wifi_smmu", &smmu, sizeof(smmu)) && smmu) {
		ce_mark("about to read the apps SMMU's registers");
		smmu_ok = r5q_smmu_look(smmu >= 2);
	}
	ce_mark("about to read CE0's control register");
	printf("wifi: ce: CE0 ctrl1 %#x, CE2 ctrl1 %#x, CE2 drri %#x\n", rd(wlan_regs + CE_REG(0, CE_CTRL1)),
	    rd(wlan_regs + CE_REG(2, CE_CTRL1)), rd(wlan_regs + CE_REG(2, CE_CUR_DRRI)));
	ce_mark("about to write the RRI address and the update bits");
	{
		static uint64_t rri_pa;
		uint8_t *rri = wlan_rri != NULL ? wlan_rri : r5q_dma_page(&rri_pa, PAGE_SIZE);

		wlan_rri = rri;
		if (rri != NULL) {
			bzero(rri, PAGE_SIZE);
		}

		if (rri == NULL) {
			return;
		}
		wr(wlan_regs + CE_RRI_LOW, (uint32_t)rri_pa);
		wr(wlan_regs + CE_RRI_HIGH, (uint32_t)(rri_pa >> 32) & 0x1f);
		for (uint32_t i = 0; i < 12; i++) {
			wr(wlan_regs + CE_REG(i, CE_CTRL1), rd(wlan_regs + CE_REG(i, CE_CTRL1)) | 0x80000u);
		}
		printf("wifi: ce: read indices into host memory at %#llx\n", rri_pa);
	}
	ce_mark("about to set up the rings");
	if (!ce_setup(&ce0, 0, 16, false) || !ce_setup(&ce3, 3, 32, false) || !ce_setup(&ce2, 2, 64, true) ||
	    !ce_setup(&ce1, 1, 32, true) || !ce_setup(&ce4, 4, 256, false)) {
		printf("wifi: ce: no memory for the rings\n");
		return;
	}
	{
		uint32_t post = 1;

		if (!smmu_ok) {
			printf("wifi: ce: the WLAN's stream (SID 0x640) has no SMMU match (wifi_smmu=2 makes one): no buffers posted, "
			    "as their DMA would fault and reset the phone\n");
			return;
		}
		{
			uint32_t share = 0, refused = 0;      /* (Test 113: TrustZone refuses, -2: off unless wifi_share=1) */

			PE_parse_boot_argn("wifi_share", &share, sizeof(share));
			if (share) {
				struct r5q_ce *all[4] = { &ce0, &ce1, &ce2, &ce3 };

				ce_mark("about to share the rings and buffers with the WLAN (TrustZone assign_mem)");
				refused += r5q_share_pages(wlan_rri, PAGE_SIZE);
				for (int i = 0; i < 4; i++) {
					refused += r5q_share_pages(all[i]->ring, PAGE_SIZE);
					refused += r5q_share_pages(all[i]->bufs, (size_t)all[i]->n * CE_BUF);
				}
				printf("wifi: ce: rings and buffers shared with WLAN + WLAN_CE (%u pages refused)\n", refused);
			}
		}
		if (PE_parse_boot_argn("wifi_ce_post", &post, sizeof(post)) && post == 0) {
			printf("wifi: ce: wifi_ce_post=0: rings set up, no buffers posted (no DMA): stopping here\n");
			return;
		}
	}
	ce_mark("about to post the receive buffers");
	ce_post_all(&ce2);
	ce_post_all(&ce1);
	printf("wifi: ce: receive buffers posted (CE2 %u, CE1 %u); waiting for HTC READY\n", ce2.n - 1, ce1.n - 1);
	for (uint64_t until = now_us() + 10000000; now_us() < until || now_us() < wmi_hold_us;) {
		r5q_service();
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (wmi_step != 0) {
		printf("wifi: scan: listened until +%llu ms: %u management frames, %u networks (WMI credits %d)\n",
		    (now_us() - svc_t0) / 1000, scan_frames, scan_nets, wmi_credits);
		printf("wifi: htt: %u in-order indications (%u buffers returned, %u unknown addresses), %u other messages; "
		    "ring index %u, CE4 write %u read %u\n", htt_inord, htt_msdus, htt_lost, htt_other, rxr_alloc, ce4.wr,
		    rd(wlan_regs + CE_REG(4, CE_CUR_SRRI)));
		printf("wifi: join: ended at step %u (20 = associated, 30 = refused / no answer)\n", join_step);
	}
	FlushPoC_DcacheRegion((vm_offset_t)ce2.ring, 16 * ce2.n);
	FlushPoC_DcacheRegion((vm_offset_t)ce2.bufs, 2 * CE_BUF);
	htc_show("CE2 descriptor 0", ce2.ring, 16);
	htc_show("CE2 descriptor 1", ce2.ring + 16, 16);
	htc_show("CE2 buffer 0", ce2.bufs, 40);
	htc_show("CE2 buffer 1", ce2.bufs + CE_BUF, 40);
	htc_show("CE1 descriptor 0", ce1.ring, 16);
	if (wlan_rri != NULL) {
		FlushPoC_DcacheRegion((vm_offset_t)wlan_rri, 64);
		htc_show("read indices in host memory (CE0..CE11, 4 bytes each)", wlan_rri, 48);
	}
	if (smmu_regs != 0) {
		/* global fault: sGFSR 0x48, sGFSYNR0/1/2 0x50/0x54/0x58 (SYNR1: the stream id), sGFAR 0x40 */
		printf("wifi: smmu: global fault status %#x, syndromes %#x %#x %#x, address %#x%08x\n", rd(smmu_regs + 0x48),
		    rd(smmu_regs + 0x50), rd(smmu_regs + 0x54), rd(smmu_regs + 0x58), rd(smmu_regs + 0x44), rd(smmu_regs + 0x40));
		if (smmu_cb != ~0u) {
			vm_offset_t c = smmu_regs + 0x80000 + (vm_offset_t)smmu_cb * 0x1000;   /* 128 global pages of 4 KB */

			printf("wifi: smmu: CB%u SCTLR %#x FSR %#x FSYNR0 %#x FAR %#x%08x\n", smmu_cb, rd(c), rd(c + 0x58), rd(c + 0x68),
			    rd(c + 0x64), rd(c + 0x60));
		}
	}
	printf("wifi: ce: CE3 (WMI out): our write index %u, its read index %u (register), source write index register %u, "
	    "errors %#x; CE0: write %u read %u\n", ce3.wr, rd(wlan_regs + CE_REG(3, CE_CUR_SRRI)), rd(wlan_regs + CE_REG(3, CE_SR_WR)),
	    rd(wlan_regs + CE_REG(3, CE_MISC_IS)), ce0.wr, rd(wlan_regs + CE_REG(0, CE_CUR_SRRI)));
	htc_show("CE3 descriptor 0", ce3.ring, 16);
	htc_show("CE3 buffer 0", ce3.bufs, 40);
	misc = rd(wlan_regs + CE_REG(2, CE_MISC_IS));
	printf("wifi: ce: after 5 s: CE2 sw %u wr %u (hardware read index %u), CE0 hw read index %u, CE2 errors %#x\n", ce2.sw,
	    ce2.wr, rd(wlan_regs + CE_REG(2, CE_CUR_DRRI)), rd(wlan_regs + CE_REG(0, CE_CUR_SRRI)), misc);
	/* (Test 120) the rings are the net thread's from here on */
	r5q_link_ready = true;
	if (join_step == 24 && r5q_net_attached) {
		redwood_net_link_state(true);
		printf("wifi: net: en0's link is UP (the radio carries its frames; configd's DHCP can start)\n");
	} else {
		printf("wifi: net: not connected (step %u): en0's link stays down\n", join_step);
	}
	if (wmi_step >= 6) {
		r5q_sta_start();
	}
}

static bool wlfw_again;                 /* (Test 172) the handshake once more, after the Wi-Fi domain restarted */

static void
r5q_wlfw_handshake(void)
{
	static struct wlfw_cap_resp_msg_v01 cap;
	struct wlfw_ind_register_req_msg_v01 ind;
	struct wlfw_ind_register_resp_msg_v01 ind_resp;
	struct wlfw_host_cap_req_msg_v01 host;
	struct wlfw_host_cap_resp_msg_v01 host_resp;
	struct wlfw_msa_info_req_msg_v01 msa;
	static struct wlfw_msa_info_resp_msg_v01 msa_resp;
	struct wlfw_msa_ready_req_msg_v01 ready;
	struct wlfw_msa_ready_resp_msg_v01 ready_resp;
	struct wlfw_cap_req_msg_v01 cap_req;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("wifi_wlfw", &on, sizeof(on)) || on == 0) {
		return;
	}
	wlfw10_began = now_us();
	if (!wlfw_again && (qmi_handle_init(&wlfw10, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN + 64, &wlfw10_ops,
	    wlfw10_handlers) < 0 || qmi_add_lookup(&wlfw10, 0x45, WLFW_SERVICE_VERS_V01, 0) < 0)) {
		printf("wifi: wlfw: no QMI handle\n");
		return;
	}
	for (unsigned ms = 0; ms < (wlfw_again ? 30000u : 3000u) && !wlfw10_server; ms += 10) {
		sleep_ms(10);
	}
	if (!wlfw10_server) {
		printf("wifi: wlfw: the service was not found\n");
		return;
	}
	bzero(&ind, sizeof(ind));
	ind.client_id_valid = 1;
	ind.client_id = R5Q_WLFW_CLIENT_ID;
	ind.fw_ready_enable_valid = 1;
	ind.fw_ready_enable = 1;
	ind.msa_ready_enable_valid = 1;
	ind.msa_ready_enable = 1;
	ind.pin_connect_result_enable_valid = 1;
	ind.pin_connect_result_enable = 1;
	bzero(&ind_resp, sizeof(ind_resp));
	if (!wlfw10_ask("indications", QMI_WLFW_IND_REGISTER_REQ_V01, WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_req_msg_v01_ei, &ind, wlfw_ind_register_resp_msg_v01_ei, &ind_resp, false)) {
		return;
	}
	bzero(&host, sizeof(host));
	host.daemon_support_valid = 1;
	host.daemon_support = 0;
	bzero(&host_resp, sizeof(host_resp));
	if (!wlfw10_ask("host capabilities", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_host_cap_req_msg_v01_ei, &host, wlfw_host_cap_resp_msg_v01_ei, &host_resp, false) &&
	    host_resp.resp.error != QMI_ERR_NOT_SUPPORTED_V01) {
		printf("wifi: wlfw: (host capabilities refused: going on, as with ath10k's skip-host-cap quirk)\n");
	}
	bzero(&msa, sizeof(msa));
	msa.msa_addr = R5Q_MSA_PA;
	msa.size = R5Q_MSA_SIZE;
	bzero(&msa_resp, sizeof(msa_resp));
	if (!wlfw10_ask("MSA information", QMI_WLFW_MSA_INFO_REQ_V01, WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_info_req_msg_v01_ei, &msa, wlfw_msa_info_resp_msg_v01_ei, &msa_resp, false)) {
		return;
	}
	sleep_ms(20);                           /* (ath10k's pause before the XPU change) */
	for (uint32_t i = 0; i < msa_resp.mem_region_info_len && i < QMI_WLFW_MAX_MEM_REG_V01; i++) {
		const struct wlfw_memory_region_info_s_v01 *r = &msa_resp.mem_region_info[i];

		printf("wifi: wlfw: MSA region %u: %#llx + %#x, %s\n", i, r->region_addr, r->size, r->secure_flag ? "secure" :
		    "not secure");
		if (r->region_addr < R5Q_MSA_PA || r->region_addr + r->size > R5Q_MSA_PA + R5Q_MSA_SIZE) {
			printf("wifi: wlfw: THAT REGION IS OUTSIDE THE MSA MEMORY: nothing assigned, stopping\n");
			return;
		}
		if (wlfw10_assign(r->region_addr, r->size, r->secure_flag != 0) != 0) {
			if (!wlfw_again) {
				printf("wifi: wlfw: TrustZone refused the region: stopping\n");
				return;
			}
			printf("wifi: wlfw: (again: the region is still the modem's from the first time; going on)\n");
		}
	}
	bzero(&ready, sizeof(ready));
	bzero(&ready_resp, sizeof(ready_resp));
	if (!wlfw10_ask("MSA ready", QMI_WLFW_MSA_READY_REQ_V01, WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_ready_req_msg_v01_ei, &ready, wlfw_msa_ready_resp_msg_v01_ei, &ready_resp, false)) {
		return;
	}
	bzero(&cap_req, sizeof(cap_req));
	bzero(&cap, sizeof(cap));
	if (wlfw10_ask("capabilities", QMI_WLFW_CAP_REQ_V01, WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei, &cap_req,
	    wlfw_cap_resp_msg_v01_ei, &cap, false)) {
		printf("wifi: wlfw: chip id %#x family %#x (%u), board id %#x (%u), soc id %#x (%u), fw version %#x \"%s\" (%u), "
		    "build \"%s\" (%u), macs %u (%u)\n", cap.chip_info.chip_id, cap.chip_info.chip_family, cap.chip_info_valid,
		    cap.board_info.board_id, cap.board_info_valid, cap.soc_info.soc_id, cap.soc_info_valid,
		    cap.fw_version_info.fw_version, cap.fw_version_info.fw_build_timestamp, cap.fw_version_info_valid,
		    cap.fw_build_id, cap.fw_build_id_valid, cap.num_macs, cap.num_macs_valid);
	}
	for (unsigned ms = 0; ms < 3000 && !wlfw10_msa_ready; ms += 10) {
		sleep_ms(10);
	}
	if (wlfw10_msa_ready) {
		wlfw10_board();
	} else {
		printf("wifi: wlfw: no \"MSA ready\" from the firmware in 3 s: board data not sent\n");
	}
	for (unsigned ms = 0; ms < 10000 && !wlfw10_fw_ready; ms += 10) {
		sleep_ms(10);
	}
	printf("wifi: wlfw: +%llu ms: %s\n", (now_us() - wlfw10_began) / 1000, wlfw10_fw_ready ?
	    "THE WI-FI FIRMWARE IS READY (ath10k would now start the driver: wlan config and mode)" :
	    "the firmware has not said \"ready\" in 10 s");
	if (wlfw10_fw_ready && on >= 2) {
		r5q_wlfw_enable();              /* (Test 110: wifi_wlfw=2) */
		if (on >= 3) {
			r5q_htc_bringup();      /* (Test 111: wifi_wlfw=3) */
		}
	}
}

/*
 * r5q (Test 86): the link with the modem, which the loader has started (boot-arg modem=2) -- the donor's steps 10 and
 * 12 to 14 with the modem's numbers: our SMP2P item 428 (entries "master-kernel" and "ipa") beside its 435, our GLINK
 * queue 479 made as Linux makes it (the modem made the descriptor 478 and its queue 480; the boot chain made nothing
 * here), the modem's SMP2P item watched, then GLINK's versions and the channel IPCRTR, then QRTR. Doorbells: APCS IPC
 * bits 14 (SMP2P) and 12 (GLINK). Boot-arg wifi_modem=1; on a thread of its own, beside the system's start.
 */
/*
 * (Test 172) Everything the first bring-up set up for the rings, HTC, WMI, the join and the keys, back to how it was
 * before it; then the handshake and the bring-up once more. Its memory is reused (ce_setup, r5q_rx_ring_alloc), the
 * TrustZone assignment of the MSA regions stays, iOS's station stays attached.
 */
static void
r5q_wifi_again(void)
{
	uint64_t began = now_us();

	r5q_pd_back = false;
	r5q_pd_down = false;
	printf("wifi: THE WI-FI DOMAIN IS BACK: the bring-up again\n");
	lck_mtx_lock(&r5q_net_lock);
	r5q_link_ready = false;
	htc_seq = 0;
	svc_ready = false;
	svc_wmi_eid = svc_htt_eid = 0xff;
	svc_credits = 0;
	htt_ep = 0xff;
	htt_ver_ok = false;
	wmi_step = 0;
	wmi_init_sent = false;
	wmi_credits = -1;
	wmi_hold_us = 0;
	join_step = 0;
	join_ev = false;
	join_have = false;
	r5q_keyed = false;
	wpa_have_snonce = false;
	wpa_rekey_us = 0;
	scan_running = false;
	bzero(dtx_busy, sizeof(dtx_busy));
	lck_mtx_unlock(&r5q_net_lock);
	wlfw10_msa_ready = false;
	wlfw10_fw_ready = false;
	wlfw_again = true;
	r5q_wlfw_handshake();
	printf("wifi: the bring-up again: done in %llu ms; join step %u, en0's link %s\n", (now_us() - began) / 1000, join_step,
	    join_step == 24 ? "UP" : "down");
}

static void
r5q_modem_link(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	uint32_t size = 0, cacheline = 0, partition, seconds = 5;
	uint64_t began = now_us();

	smem_phys = 0x86000000UL;
	smem_remote = 1;
	item_smp2p_ours = 428;
	item_smp2p_theirs = 435;
	item_crash_reason = 421;
	pause_ms = 0;
	PE_parse_boot_argn("wifi_pause", &pause_ms, sizeof(pause_ms));
	step = 10;
	printf("wifi: r5q: the link with the modem the loader started: SMP2P 428 (ours) / 435 (its), GLINK 478-480, doorbells "
	    "APCS IPC 0x17c0000c bits 14 and 12\n");
	bzero(&last, sizeof(last));
	if (!dsp_words(&last)) {
		printf("wifi: r5q: THE MODEM HAS NO SMP2P ITEM (435): it is not running (the loader starts it with modem=2, with a PC on "
		    "the cable)\n");
		goto out;
	}
	show_words("the modem's item, before ours", &last);
	show_crash_reason();
	r5q_modem = true;
	if (!step_our_item()) {
		goto out;
	}
	step = 11;
	partition = smem_partition(SMEM_US, SMEM_WPSS, &cacheline, &size);
	if (smem_item(ITEM_GLINK_OUR_QUEUE, &size) == 0) {
		announce("make our GLINK queue (item 479, 16 KB) in our partition with the modem, as Linux's qcom_glink_smem does");
		if (partition == 0 || !smem_make(partition, ITEM_GLINK_OUR_QUEUE, 16384)) {
			goto out;
		}
	}
	PE_parse_boot_argn("wifi_wait", &seconds, sizeof(seconds));
	printf("wifi: 11: watching the modem's SMP2P item for %u s\n", seconds);
	for (uint64_t until = now_us() + seconds * 1000000ull; now_us() < until;) {
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			char when[48];

			snprintf(when, sizeof(when), "+%llu ms: the modem's item changes", (now_us() - began) / 1000);
			show_words(when, &words);
			last = words;
			dsp_listen(&words);
			if (dsp_state(&words) & DSP_FATAL) {
				printf("wifi: r5q: THE MODEM SAYS IT HAS GIVEN UP\n");
				show_crash_reason();
				goto out;
			}
		}
		sleep_ms(1);
	}
	printf("wifi: 11: the modem's \"slave-kernel\" says %#x%s\n", dsp_state(&last), (dsp_state(&last) & DSP_READY) ?
	    ": IT IS UP" : "");
	step = 12;
	if (!step_queues()) {
		goto out;
	}
	step = 13;
	if (!step_link()) {
		goto out;
	}
	for (unsigned ms = 0; ms < 2000 && qrtr_hellos == 0; ms++) {
		sleep_ms(1);
	}
	r5q_wifi_rails();                       /* (Test 92: the radio chip powered, before its firmware is started) */
	diag_start();                           /* (Test 103: its F3 messages, before anything else) */
	rmtfs_start();                          /* (Test 88: its EFS, before it brings anything else up) */
	memshare_start();                       /* (Test 95: what heap memory it asks the AP for) */
	tftp_start();                           /* (Test 89: its files, the Wi-Fi firmware among them) */
	pdmap_start();                          /* (Test 90: which domains to start: the Wi-Fi domain) */
	servreg_start();                        /* (Test 94: told of the Wi-Fi domain's state, and acknowledging it) */
	step = 14;
	step_qrtr();
	/* what the modem does next, for a minute: rmtfs requests, and services it tells of (the Wi-Fi firmware's: 0x45) */
	for (unsigned s = 0; s < 60 && wlfw_port == 0; s++) {
		for (unsigned t = 0; t < 20; t++) {
			sleep_ms(50);
			if (diag_masks_due) {   /* (Test 103: the masks once the root PD has its DIAG id, as msm sends them) */
				diag_masks_due = false;
				diag_send_masks(diag_mask_node, diag_mask_port);
			}
		}
		if (s == 2 && diag_cntl_port != NULL) {
			diag_commands();        /* (Test 103: a version request through the modem's command end) */
		}
		if (s == 3 || s == 6 || s == 10 || s == 15 || s == 20 || s == 30 || s == 45 || s == 59) {
			ssctl_ask();            /* (Test 107: the modem's own failure reason, while the Wi-Fi domain dies) */
		}
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].service == SERVICE_WLFW) {
				wlfw_port = qrtr_servers[i].port;
			}
		}
	}
	printf("wifi: r5q: a minute on: rmtfs requests %u (modemst1 r%u w%u, modemst2 r%u w%u, fsg r%u, fsc r%u); the Wi-Fi "
	    "firmware's service %s\n", rmtfs_requests, rmtfs_files[0].reads, rmtfs_files[0].writes, rmtfs_files[1].reads,
	    rmtfs_files[1].writes, rmtfs_files[2].reads, rmtfs_files[3].reads, wlfw_port ? "IS THERE" : "is not there");
	if (wlfw_port != 0) {
		uint32_t restart_at = 0, crash_at = 0;
		uint64_t up_us;

		redwood_qmi_set_transport(&qrtr_ops);   /* (Test 109: the QMI helpers over this link) */
		r5q_wlfw_handshake();
		/*
		 * (Test 172) From here on: when the Wi-Fi domain goes down the link goes down, and when it is back the
		 * bring-up is done again (r5q_wifi_again). Boot-arg wifi_pd_restart=S asks the registry to restart the
		 * domain S seconds after the first bring-up, to see this work.
		 */
		PE_parse_boot_argn("wifi_pd_restart", &restart_at, sizeof(restart_at));
		PE_parse_boot_argn("wifi_fw_crash", &crash_at, sizeof(crash_at));
		up_us = now_us();
		for (;;) {
			sleep_ms(100);
			/*
			 * (Test 180) Boot-arg wifi_fw_crash=S: S seconds on, the Wi-Fi firmware is made to assert, as ath10k's
			 * simulate_fw_crash "assert" does (WMI_TLV_FORCE_FW_HANG_CMDID 0x1d008, TLV 0x98 {type 1 assert, delay 0}):
			 * the domain goes down and comes back, and r5q_wifi_again() is seen to bring Wi-Fi back.
			 */
			if (crash_at != 0 && now_us() - up_us > (uint64_t)crash_at * 1000000 && r5q_link_ready && svc_wmi_eid != 0xff) {
				uint32_t hang[2] = { 1, 0 };
				uint8_t m[16];
				uint32_t n = tlv_put(m, 0x98, hang, sizeof(hang));

				crash_at = 0;
				lck_mtx_lock(&r5q_net_lock);
				r5q_wmi_send(svc_wmi_eid, 0x1d008, m, (uint16_t)n);
				lck_mtx_unlock(&r5q_net_lock);
				printf("wifi: the Wi-Fi firmware told to assert (wifi_fw_crash): the recovery is to be seen\n");
			}
			if (restart_at != 0 && now_us() - up_us > (uint64_t)restart_at * 1000000) {
				restart_at = 0;
				servreg_restart("msm/modem/wlan_pd");
			}
			if (r5q_pd_down && r5q_link_ready) {
				lck_mtx_lock(&r5q_net_lock);
				r5q_link_ready = false;
				lck_mtx_unlock(&r5q_net_lock);
				if (join_step == 24) {
					redwood_net_link_state(false);
				}
				printf("wifi: THE WI-FI DOMAIN WENT DOWN: en0's link is down until it is back\n");
			}
			if (r5q_pd_back) {
				r5q_wifi_again();
				up_us = now_us();
			}
		}
	}
	show_crash_reason();
out:
	printf("wifi: r5q: the link with the modem: done after %llu ms\n", (now_us() - began) / 1000);
	thread_terminate(current_thread());
}

/*
 * (r5q Test 244) Bluetooth, stage 1 (wifi/BLUETOOTH-PLAN.md). The WCN3990's Bluetooth is reached by HCI over a
 * high-speed UART, QUP serial engine 13 (wrapper 2 slot 3, 0xc8c000; pins GPIO43 CTS, 44 RTS, 45 TX, 46 RX, function
 * qup13 = 1, east tile 0x03500000), with three supplies: PM8150 L7 (core 1.8 V), PM8150L L2 (PA 1.3 V), PM8150L L11
 * (3.3 V), all RPMh VRMs. boot-arg bt=1: everything read, nothing written (the engine's clocks, its baud clock's
 * RCG, the pins, the supplies' cmd-db addresses). bt=2: then the three supplies voted on through RPMh and the
 * engine's clocks voted on in GCC, and the engine itself read: its firmware's protocol (2 = UART), whether its FIFO
 * interface is there (the touch engine's is not), its hardware parameters. Nothing is sent to the chip yet.
 */
#define BT_GCC_PHYS             0x00100000UL
#define BT_GCC_VOTE             0x52014u        /* APCS_CLOCK_BRANCH_ENA_VOTE_1: bit 7 S3, 2 M_AHB, 1 S_AHB */
#define BT_GCC_S3_CBCR          0x1e4d4u
#define BT_GCC_M_AHB_CBCR       0x1e004u
#define BT_GCC_S_AHB_CBCR       0x1e008u
#define BT_GCC_S3_RCG           0x1e4d8u        /* CMD, CFG, M, N, D */
#define BT_SE_PHYS              0x00c8c000UL
#define BT_TLMM_EAST            0x03500000UL

/*
 * (r5q Test 254) Bluetooth stage 2: the WCN3990's HCI over SE13 as a polled FIFO-mode GENI UART (Linux
 * qcom_geni_serial.c, qcom-geni-se.c), then its power-on as hci_qca.c's qca_wcn3990_init, then HCI_Reset and the
 * QCA version command. Test 253: the RCG's DFS is off (cmd+0x14 = 0x8020), it runs from XO (19.2 MHz); QUP wrapper 2
 * is v1.3 (0xcc0004 = 0x10030000): 32x UART oversampling. Here: the RCG to 7.3728 MHz (GPLL0 even, M/N 384/15625,
 * as Linux's ftbl): 2400 baud = divider 96, 115200 = divider 2.
 */
#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_M_CLK_CFG          0x048
#define GENI_S_CLK_CFG          0x04c
#define UART_LOOPBACK_CFG       0x22c
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE_EN        0x258
#define UART_TX_TRANS_CFG       0x25c           /* bit 1: CTS ignored */
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define UART_TX_WORD_LEN        0x268
#define UART_TX_STOP_BIT_LEN    0x26c
#define UART_TX_TRANS_LEN       0x270
#define UART_RX_TRANS_CFG       0x280
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define UART_RX_WORD_LEN        0x28c
#define UART_RX_STALE_CNT       0x294
#define UART_TX_PARITY_CFG      0x2a4
#define UART_RX_PARITY_CFG      0x2a8
#define UART_MANUAL_RFR         0x2ac
#define GENI_M_CMD0             0x600
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_CMD0             0x630
#define GENI_S_CMD_CTRL         0x634
#define GENI_S_IRQ_STATUS       0x640
#define GENI_S_IRQ_EN           0x644
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_FIFO_STATUS     0x800
#define GENI_RX_FIFO_STATUS     0x804
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR_WATERMARK   0x814
#define GENI_GSI_EVENT_EN       0xe18
#define GENI_IRQ_EN             0xe1c
#define GENI_DMA_GENERAL_CFG    0xe30

static vm_offset_t bt_se;
static vm_offset_t bt_rcg_page;                 /* (Test 263) the page of the serial engine's baud clock RCG */
static uint32_t bt_se_hz = 7372800u;            /* that clock's rate, as set */

static void
bt_uart_speed(uint32_t baud)
{
	uint32_t div = bt_se_hz / (baud * 32u);

	wr(bt_se + GENI_M_CLK_CFG, (div << 4) | 1);
	wr(bt_se + GENI_S_CLK_CFG, (div << 4) | 1);
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
bt_uart_flow(bool on)
{
	wr(bt_se + UART_TX_TRANS_CFG, on ? 0 : 2);              /* CTS obeyed / ignored */
	wr(bt_se + UART_MANUAL_RFR, on ? 0 : (1u << 31));       /* RFR by the hardware / held "ready" by hand */
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
bt_uart_write(const uint8_t *bytes, uint32_t len)
{
	wr(bt_se + GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + UART_TX_TRANS_LEN, len);
	wr(bt_se + GENI_M_CMD0, 1u << 27);                      /* UART_START_TX */
	for (uint32_t at = 0; at < len; at += 4) {
		uint32_t word = 0;

		for (uint32_t i = 0; i < 4 && at + i < len; i++) {
			word |= (uint32_t)bytes[at + i] << (8 * i);
		}
		for (int us = 0; us < 100000 && (rd(bt_se + GENI_TX_FIFO_STATUS) & 0x0fffffff) >= 14; us += 10) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
		wr(bt_se + GENI_TX_FIFO, word);
	}
	for (int us = 0; us < 2000000; us += 20) {
		if (rd(bt_se + GENI_M_IRQ_STATUS) & 1) {        /* M_CMD_DONE */
			wr(bt_se + GENI_M_IRQ_CLEAR, 1);
			return true;
		}
		delay_for_interval(20, NSEC_PER_USEC);
	}
	return false;
}

static uint32_t
bt_uart_read(uint8_t *bytes, uint32_t max, uint32_t ms)
{
	uint32_t got = 0;

	for (uint32_t t = 0; t < ms * 10 && got < max; t++) {
		uint32_t st = rd(bt_se + GENI_RX_FIFO_STATUS), words = st & 0x1ffffff;

		for (uint32_t w = 0; w < words && got < max; w++) {
			uint32_t word = rd(bt_se + GENI_RX_FIFO), n = 4;

			if (w == words - 1 && (st & (1u << 31)) && ((st >> 28) & 7) != 0) {
				n = (st >> 28) & 7;                     /* the last word: its valid bytes */
			}
			for (uint32_t i = 0; i < n && got < max; i++) {
				bytes[got++] = (uint8_t)(word >> (8 * i));
			}
		}
		if (words == 0) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
		wr(bt_se + GENI_S_IRQ_CLEAR, 0xffffffffu);
	}
	return got;
}

static void
bt_show(const char *what, const uint8_t *b, uint32_t n)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "bt: %s (%u bytes):", what, n);

	for (uint32_t i = 0; i < n && i < 48 && at < (int)sizeof(line) - 4; i++) {
		at += snprintf(line + at, sizeof(line) - at, " %02x", b[i]);
	}
	printf("%s\n", line);
}

static void
bt_stage2(vm_offset_t gcc_vote, vm_offset_t wrap2)
{
	static const uint32_t pins[] = { 43, 44, 45, 46 };
	static const uint32_t pin_cfg[] = { 0x4, 0x5, 0x7, 0x4 };       /* qup13; CTS/RX no pull, RTS down, TX up */
	static const uint8_t power_off = 0xc0, power_on = 0xfc;
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };      /* HCI_Reset */
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 }; /* QCA EDL_PATCH_GETVER */
	static const uint8_t local_version[] = { 0x01, 0x01, 0x10, 0x00 }; /* HCI_Read_Local_Version_Information */
	static uint8_t rx[256];
	uint32_t rcg = BT_GCC_S3_RCG & 0xfff, n;

	/* the clocks (S3, wrapper 2's AHB) on, and the baud clock at 7.3728 MHz */
	bt_rcg_page = wrap2;
	wr(gcc_vote + (BT_GCC_VOTE & 0xfff), rd(gcc_vote + (BT_GCC_VOTE & 0xfff)) | (1u << 7) | (1u << 2) | (1u << 1));
	__asm__ volatile ("dsb sy" ::: "memory");
	delay_for_interval(5, NSEC_PER_MSEC);
	wr(wrap2 + rcg + 8, 384);                                        /* M */
	wr(wrap2 + rcg + 12, ~(15625u - 384u) & 0xffff);                /* N: ~(n - m) */
	wr(wrap2 + rcg + 16, ~(15625u - 384u) & 0xffff);                /* D: ~2d, 2d clamped to n - m */
	wr(wrap2 + rcg + 4, (rd(wrap2 + rcg + 4) & ~0x371fu) | 1 | (6u << 8) | (2u << 12));  /* div 1, GPLL0 even, dual edge */
	wr(wrap2 + rcg, rd(wrap2 + rcg) | 1);                            /* update */
	for (int us = 0; us < 10000 && (rd(wrap2 + rcg) & 1); us += 10) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("bt: stage 2: RCG cmd %#x cfg %#x m %#x n %#x d %#x (7.3728 MHz asked for)\n", rd(wrap2 + rcg),
	    rd(wrap2 + rcg + 4), rd(wrap2 + rcg + 8), rd(wrap2 + rcg + 12), rd(wrap2 + rcg + 16));
	/* the pins: qup13 */
	for (unsigned i = 0; i < 4; i++) {
		vm_offset_t pin = ml_io_map(BT_TLMM_EAST + 0x1000 * pins[i], 0x1000);

		if (pin != 0) {
			wr(pin, pin_cfg[i]);
			printf("bt: stage 2: GPIO%u cfg now %#x\n", pins[i], rd(pin));
		}
	}
	/* the engine: FIFO mode, packing 8 bits x 4 per word, 8N1, RX started */
	bt_se = ml_io_map(BT_SE_PHYS, 0x1000);
	if (bt_se == 0) {
		return;
	}
	wr(bt_se + GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + GENI_S_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + GENI_CGC_CTRL, rd(bt_se + GENI_CGC_CTRL) | 0x7f);
	wr(bt_se + GENI_DMA_GENERAL_CFG, rd(bt_se + GENI_DMA_GENERAL_CFG) | 0xf);
	wr(bt_se + GENI_OUTPUT_CTRL, 0x7f);
	wr(bt_se + GENI_FORCE_DEFAULT, 1);
	wr(bt_se + GENI_IRQ_EN, (rd(bt_se + GENI_IRQ_EN) | (1u << 2) | (1u << 3)) & ~3u);
	wr(bt_se + GENI_DMA_MODE_EN, rd(bt_se + GENI_DMA_MODE_EN) & ~1u);
	wr(bt_se + GENI_GSI_EVENT_EN, 0);
	wr(bt_se + GENI_RX_WATERMARK, 2);
	wr(bt_se + GENI_RX_RFR_WATERMARK, 32 - 2);
	wr(bt_se + GENI_M_IRQ_EN, rd(bt_se + GENI_M_IRQ_EN) | 1);
	wr(bt_se + GENI_S_IRQ_EN, rd(bt_se + GENI_S_IRQ_EN) | (1u << 26) | (1u << 27));
	wr(bt_se + GENI_TX_PACKING_CFG0, 0x4380e);
	wr(bt_se + GENI_TX_PACKING_CFG1, 0xc3e0e);
	wr(bt_se + GENI_RX_PACKING_CFG0, 0x4380e);
	wr(bt_se + GENI_RX_PACKING_CFG1, 0xc3e0e);
	wr(bt_se + GENI_BYTE_GRAN, 0);
	wr(bt_se + UART_LOOPBACK_CFG, 0);
	wr(bt_se + UART_TX_PARITY_CFG, 0);
	wr(bt_se + UART_RX_TRANS_CFG, 0);
	wr(bt_se + UART_RX_PARITY_CFG, 0);
	wr(bt_se + UART_TX_WORD_LEN, 8);
	wr(bt_se + UART_RX_WORD_LEN, 8);
	wr(bt_se + UART_TX_STOP_BIT_LEN, 0);
	wr(bt_se + UART_RX_STALE_CNT, 10 * 16);
	bt_uart_flow(false);
	bt_uart_speed(2400);
	wr(bt_se + GENI_S_CMD0, 1u << 27);                              /* UART_START_READ */
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("bt: stage 2: UART set up (FIFO mode, 8N1); status %#x, M %#x S %#x\n", rd(bt_se + 0x40),
	    rd(bt_se + GENI_M_IRQ_STATUS), rd(bt_se + GENI_S_IRQ_STATUS));
	/* hci_qca's qca_wcn3990_init: the power-off pulse at 2400, the power-on pulse at 115200, 100 ms */
	printf("bt: stage 2: power-off pulse (0xc0 at 2400): %s\n", bt_uart_write(&power_off, 1) ? "sent" : "NOT SENT");
	delay_for_interval(10, NSEC_PER_MSEC);
	bt_uart_speed(115200);
	printf("bt: stage 2: power-on pulse (0xfc at 115200): %s\n", bt_uart_write(&power_on, 1) ? "sent" : "NOT SENT");
	delay_for_interval(100, NSEC_PER_MSEC);
	n = bt_uart_read(rx, sizeof(rx), 50);
	bt_show("after the power-on pulse", rx, n);
	bt_uart_flow(true);
	printf("bt: stage 2: HCI_Reset: %s\n", bt_uart_write(reset, sizeof(reset)) ? "sent" : "NOT SENT");
	n = bt_uart_read(rx, sizeof(rx), 500);
	bt_show("HCI_Reset's answer", rx, n);
	printf("bt: stage 2: QCA version: %s\n", bt_uart_write(version, sizeof(version)) ? "sent" : "NOT SENT");
	n = bt_uart_read(rx, sizeof(rx), 500);
	bt_show("the QCA version's answer", rx, n);
	printf("bt: stage 2: Read_Local_Version: %s\n", bt_uart_write(local_version, sizeof(local_version)) ? "sent" : "NOT SENT");
	n = bt_uart_read(rx, sizeof(rx), 500);
	bt_show("Read_Local_Version's answer", rx, n);
	printf("bt: stage 2: done; status %#x, M %#x S %#x, rx fifo %#x, CTS/RFR io GPIO43 ... (see above)\n",
	    rd(bt_se + 0x40), rd(bt_se + GENI_M_IRQ_STATUS), rd(bt_se + GENI_S_IRQ_STATUS), rd(bt_se + GENI_RX_FIFO_STATUS));
}

/*
 * (r5q Test 255) Bluetooth stage 3: the controller's firmware, as Linux btqca.c (qca_download_firmware,
 * qca_tlv_send_segment): the whole TLV file in 243-byte segments, each an HCI vendor command 0xfc00 with
 * {EDL_PATCH_TLV_REQ_CMD 0x1e, length, bytes}; the controller acknowledges by a vendor event (and/or a command
 * complete), as the patch header's download mode says. First crbtfw21.tlv (the ROM patch), then crnv21.bin (the NVM),
 * whose HCI tag (17) gets baud 115200 (data[2] = 0) so the link stays where it is. Both from the root's /fw (../fw-bt).
 */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t
bt_send_tlv(const char *name, const uint8_t *data, uint32_t size)
{
	static uint8_t cmd[4 + 2 + 243], rx[64];
	uint32_t mode = 0, segments = 0, acks = 0, quiet = 0;

	if (size > 12 && (data[0] & 0xff) == 1) {
		mode = data[4 + 10];                    /* tlv_type_patch: total, data_length, format, signature, mode */
	}
	printf("bt: stage 3: %s: %u bytes, TLV type %u, download mode %u\n", name, size, data[0], mode);
	for (uint32_t at = 0; at < size; at += 243) {
		uint32_t seg = size - at < 243 ? size - at : 243, n;

		cmd[0] = 0x01;                          /* H4: a command */
		cmd[1] = 0x00;
		cmd[2] = 0xfc;                          /* opcode 0xfc00 */
		cmd[3] = (uint8_t)(seg + 2);
		cmd[4] = 0x1e;                          /* EDL_PATCH_TLV_REQ_CMD */
		cmd[5] = (uint8_t)seg;
		memcpy(cmd + 6, data + at, seg);
		if (!bt_uart_write(cmd, 6 + seg)) {
			printf("bt: stage 3: %s: segment %u NOT SENT\n", name, segments);
			return segments;
		}
		segments++;
		n = bt_uart_read(rx, sizeof(rx), mode == 3 ? 2 : 100);     /* (mode 3: no events at all) */
		if (n > 0) {
			acks++;
		} else {
			quiet++;
		}
		if (segments <= 2 || (n > 0 && rx[1] == 0xff && n > 6 && rx[6] != 0) || at + seg >= size) {
			bt_show(segments <= 2 ? "segment's answer" : "an answer", rx, n);
		}
	}
	printf("bt: stage 3: %s: %u segments sent, %u answered, %u without an answer\n", name, segments, acks, quiet);
	return segments;
}

static void
bt_stage3(void)
{
	static uint8_t nvm[8192], rx[256];
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	static const uint8_t local_version[] = { 0x01, 0x01, 0x10, 0x00 };
	uint64_t psize = 0, nsize = 0;
	const uint8_t *patch = redwood_image_file("fw", "crbtfw21.tlv", &psize, NULL);
	const uint8_t *nv = redwood_image_file("fw", "crnv21.bin", &nsize, NULL);
	uint32_t n;

	if (patch == NULL || nv == NULL || nsize > sizeof(nvm)) {
		printf("bt: stage 3: firmware not in /fw (crbtfw21.tlv %p, crnv21.bin %p): not loaded\n", patch, nv);
		return;
	}
	(void)bt_send_tlv("crbtfw21.tlv", patch, (uint32_t)psize);
	memcpy(nvm, nv, nsize);
	for (uint32_t idx = 4; idx + 12 <= nsize;) {   /* (after the TLV header) tag id, tag length, 8 bytes, data */
		uint32_t tag = nvm[idx] | nvm[idx + 1] << 8, len = nvm[idx + 2] | nvm[idx + 3] << 8;

		if (tag == 17 && len >= 3) {
			printf("bt: stage 3: NVM HCI tag: %02x %02x %02x -> baud byte 0 (115200)\n", nvm[idx + 12],
			    nvm[idx + 13], nvm[idx + 14]);
			nvm[idx + 12 + 2] = 0;
		}
		idx += 4 + 8 + len;
	}
	(void)bt_send_tlv("crnv21.bin", nvm, (uint32_t)nsize);
	delay_for_interval(50, NSEC_PER_MSEC);
	n = bt_uart_read(rx, sizeof(rx), 100);
	bt_show("after the NVM", rx, n);
	printf("bt: stage 3: HCI_Reset: %s\n", bt_uart_write(reset, sizeof(reset)) ? "sent" : "NOT SENT");
	n = bt_uart_read(rx, sizeof(rx), 1000);
	bt_show("HCI_Reset's answer (patched)", rx, n);
	(void)bt_uart_write(version, sizeof(version));
	n = bt_uart_read(rx, sizeof(rx), 500);
	bt_show("the QCA version's answer (patched)", rx, n);
	(void)bt_uart_write(local_version, sizeof(local_version));
	n = bt_uart_read(rx, sizeof(rx), 500);
	bt_show("Read_Local_Version's answer (patched)", rx, n);
}

/*
 * (r5q Test 256) Bluetooth stage 4: the controller to iOS (wifi/BLUETOOTH-IOS-RESEARCH.md). iOS's bluetoothd talks H4
 * to its UART through a kernel-control socket: with BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200 in its
 * job (mkjobs.py JOBENV) it opens PF_SYSTEM/SYSPROTO_CONTROL "com.apple.uart.bluetooth", sets termios with the socket
 * options TIOCGETA/TIOCSETA (kept here and otherwise ignored: the kernel owns the line), then read()s and write()s H4.
 * Its start-up gate only access()es /dev/uart.bluetooth, made here (and /dev/btwake, which takes and drops writes).
 *
 *   - one thread (bt_pump) owns the UART: it writes the queued host packets and reads the controller's bytes, framing
 *     them into whole H4 packets (QCA in-band sleep bytes 0xfd/0xfe/0xfc answered or dropped) for the client;
 *   - the host's stream is framed too, and its vendor commands (OGF 0x3f) NEVER reach the WCN3990: they are Apple's
 *     Broadcom ones (bluetoothd thinks it has a BCM4355C0), and on a QCA chip 0xfc00-0xfc1f are the patch/NVM/EDL
 *     commands. Each is answered here with a Command Complete: bt_vsc=0 all "unknown command" (status 0x01); 1 (the
 *     default) success for the harmless ones (BD_ADDR, baud, sleep mode, PCM settings), 0x01 for the rest; 2 all
 *     success (zeroed return parameters). Every one is said, so the list bluetoothd needs can be learnt.
 */
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>

extern bool redwood_publish_named_service(const char *name, const char *data_key, const void *data, unsigned length);

#define BT_TXQ          8192
#define BT_PKT          2048

static LCK_MTX_DECLARE(bt_lock, &wifi_lock_group);
static kern_ctl_ref bt_kctl;
static volatile uint32_t bt_unit;               /* the connected client's unit; 0 = none */
static uint8_t bt_txq[BT_TXQ];                  /* whole host packets, waiting for the UART */
static volatile uint32_t bt_txq_len;
static uint8_t bt_termios[0x48];                /* struct termios, as the client last set it */
static uint32_t bt_vsc = 1;
static uint8_t bt_hpkt[BT_PKT];                 /* the host's packet being framed */
static uint32_t bt_hlen;
static uint32_t bt_said_h, bt_said_c, bt_vsc_said, bt_conns;
static uint64_t bt_n_cmd, bt_n_acl, bt_n_evt, bt_n_racl, bt_n_vsc, bt_n_drop, bt_n_junk, bt_n_ibs;

/* an H4 packet's whole length so far known from its first bytes; 0 = not yet known; -1 = not a packet type */
static int
bt_h4_len(const uint8_t *p, uint32_t have)
{
	switch (p[0]) {
	case 0x01: return have >= 4 ? 4 + p[3] : 0;                                  /* command */
	case 0x02: return have >= 5 ? 5 + (p[3] | (p[4] << 8)) : 0;                  /* ACL */
	case 0x03: return have >= 4 ? 4 + p[3] : 0;                                  /* SCO */
	case 0x04: return have >= 3 ? 3 + p[2] : 0;                                  /* event */
	case 0x05: return have >= 5 ? 5 + ((p[3] | (p[4] << 8)) & 0x3fff) : 0;       /* ISO */
	default: return -1;
	}
}

static void
bt_to_client(const uint8_t *p, uint32_t n)
{
	uint32_t unit = bt_unit;

	if (unit == 0 || bt_kctl == NULL || ctl_enqueuedata(bt_kctl, unit, (void *)(uintptr_t)p, n, 0) != 0) {
		bt_n_drop++;
	}
}

/*
 * (r5q Test 259) Standard commands bluetoothd's stack insists on and the WCN3990 does not have: answered here with
 * success, and their bits set in the controller's Read_Local_Supported_Commands answer (octet, bit as the spec's
 * table, the same Linux reads as hdev->commands[]). Test 258: "OI_HCICMD_WriteDefaultErroneousDataReporting
 * (status=628) ... HCI initialization failed" -- the stack never sent it: the mask said "not supported" (octet 18 = 0x83).
 */
static const struct {
	uint16_t op;
	uint8_t octet, bit, ret;        /* ret: bytes of zeroed return parameters after the status */
} bt_fake[] = {
	{ 0x0c5a, 18, 2, 1 },           /* Read_Default_Erroneous_Data_Reporting (1 byte: disabled) */
	{ 0x0c5b, 18, 3, 0 },           /* Write_Default_Erroneous_Data_Reporting */
};

/* the host's whole packet: forwarded (queued for bt_pump), or a vendor command answered here */
static void
bt_host_packet(const uint8_t *p, uint32_t n)
{
	if (p[0] == 0x01) {
		uint16_t op = p[1] | (p[2] << 8);

		bt_n_cmd++;
		for (unsigned i = 0; i < sizeof(bt_fake) / sizeof(bt_fake[0]); i++) {
			if (op == bt_fake[i].op) {
				uint8_t ev[16] = { 0x04, 0x0e, (uint8_t)(4 + bt_fake[i].ret), 1, p[1], p[2], 0x00 };

				bt_show("stage 4: standard command the controller lacks, answered OK here", p, n);
				bt_to_client(ev, 7 + bt_fake[i].ret);
				return;
			}
		}
		if ((op >> 10) == 0x3f) {
			static const uint16_t harmless[] = { 0xfc01, 0xfc18, 0xfc1c, 0xfc1e, 0xfc27, 0xfc45, 0xfc6e };
			uint8_t ev[7 + 64] = { 0x04, 0x0e, 4, 1, p[1], p[2], 0x01 };
			uint32_t extra = 0;

			/*
			 * (Test 260) Test 259: with the answer 0x01 and no return parameters, bluetoothd took most of them
			 * ("completed with status 701") but not BD_VSC_OLYMPIC_ANTENNA_PREF_B (0xfce2, sub-command 0x15): "Invalid
			 * data size 0 (status=65535)", then "timedout waiting for controller ... restarting". Broadcom's
			 * meta commands (0xfce2 "Olympic", 0xfd48 ...) carry a sub-command in their first parameter byte, which
			 * the answer may have to repeat. bt_vsc 3: success, the first parameter byte repeated, 8 zero bytes;
			 * 4: as 1, but the first parameter byte repeated in every answer (and 8 zero bytes after a success).
			 */
			if (bt_vsc == 2 || bt_vsc == 3) {
				ev[6] = 0;
			} else if (bt_vsc == 1 || bt_vsc == 4) {
				for (unsigned i = 0; i < sizeof(harmless) / sizeof(harmless[0]); i++) {
					if (op == harmless[i]) {
						ev[6] = 0;
					}
				}
			}
			if ((bt_vsc == 3 || bt_vsc == 4) && n > 4) {
				ev[7 + extra++] = p[4];
			}
			if (bt_vsc == 2 || (ev[6] == 0 && (bt_vsc == 3 || bt_vsc == 4))) {
				extra += 8;                     /* (zeroed return parameters: a guess at their length) */
			}
			ev[2] = (uint8_t)(4 + extra);
			bt_n_vsc++;
			if (bt_vsc_said++ < 120) {
				bt_show(ev[6] == 0 ? "stage 4: vendor command, answered OK here" : "stage 4: vendor command, answered 'unknown' here",
				    p, n);
			}
			bt_to_client(ev, 7 + extra);
			return;
		}
	} else if (p[0] == 0x02) {
		bt_n_acl++;
	}
	if (bt_said_h++ < 150) {
		bt_show("stage 4: host ->", p, n);
	}
	for (int ms = 0; ms < 2000; ms++) {
		lck_mtx_lock(&bt_lock);
		if (bt_txq_len + n <= BT_TXQ) {
			memcpy(bt_txq + bt_txq_len, p, n);
			bt_txq_len += n;
			lck_mtx_unlock(&bt_lock);
			return;
		}
		lck_mtx_unlock(&bt_lock);
		sleep_ms(1);
	}
	bt_n_drop++;
}

static void
bt_host_bytes(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int want;

		if (bt_hlen == 0 && b[i] != 0x01 && b[i] != 0x02 && b[i] != 0x03 && b[i] != 0x05) {
			bt_n_junk++;            /* (not the start of a host packet) */
			continue;
		}
		bt_hpkt[bt_hlen++] = b[i];
		want = bt_h4_len(bt_hpkt, bt_hlen);
		if (want > BT_PKT) {
			bt_n_junk++;
			bt_hlen = 0;
		} else if (want > 0 && bt_hlen == (uint32_t)want) {
			bt_host_packet(bt_hpkt, bt_hlen);
			bt_hlen = 0;
		}
	}
}

static errno_t
bt_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	char name[64] = "";

	proc_selfname(name, sizeof(name));
	*unitinfo = NULL;
	lck_mtx_lock(&bt_lock);
	bt_unit = sac->sc_unit;
	bt_hlen = 0;
	bt_txq_len = 0;
	if (bt_conns++ < 4) {
		bt_said_h = bt_said_c = 0;      /* (each of the first four clients' exchanges logged whole) */
	}
	lck_mtx_unlock(&bt_lock);
	printf("bt: stage 4: %s (pid %d) connected to com.apple.uart.bluetooth (unit %u)\n", name, proc_selfpid(), sac->sc_unit);
	return 0;
}

static errno_t
bt_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	if (bt_unit == unit) {
		bt_unit = 0;
	}
	printf("bt: stage 4: unit %u disconnected (host commands %llu, ACL %llu; events %llu, ACL in %llu; vendor commands "
	    "answered here %llu; dropped %llu)\n", unit, bt_n_cmd, bt_n_acl, bt_n_evt, bt_n_racl, bt_n_vsc, bt_n_drop);
	return 0;
}

static errno_t
bt_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	for (mbuf_t n = m; n != NULL; n = mbuf_next(n)) {
		bt_host_bytes((const uint8_t *)mbuf_data(n), mbuf_len(n));
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	if ((uint32_t)opt == 0x80487414u && data != NULL) {             /* TIOCSETA */
		memcpy(bt_termios, data, len < sizeof(bt_termios) ? len : sizeof(bt_termios));
		printf("bt: stage 4: termios set (speed %llu; the line stays where the kernel put it)\n",
		    len >= 0x48 ? *(unsigned long long *)(bt_termios + 0x40) : 0ULL);
	} else {
		printf("bt: stage 4: socket option %#x set (%zu bytes): ignored\n", (uint32_t)opt, len);
	}
	return 0;
}

static errno_t
bt_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if (data == NULL) {
		return 0;
	}
	if ((uint32_t)opt == 0x40487413u) {                             /* TIOCGETA */
		*len = *len < sizeof(bt_termios) ? *len : sizeof(bt_termios);
		memcpy(data, bt_termios, *len);
	} else {
		printf("bt: stage 4: socket option %#x asked for (%zu bytes): zeros\n", (uint32_t)opt, *len);
		bzero(data, *len);
	}
	return 0;
}

static int
bt_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
bt_dev_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return 0;
}

static int
bt_dev_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint32_t said;

	if (said++ < 10) {
		printf("bt: stage 4: %zd bytes written to /dev/%s: dropped\n", (ssize_t)uio_resid(uio),
		    minor(dev) == 0 ? "uart.bluetooth" : "btwake");
	}
	uio_update(uio, uio_resid(uio));
	return 0;
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_dev_open, .d_close = bt_dev_open, .d_read = bt_dev_read, .d_write = bt_dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * (r5q Test 263) The line faster than 115200 (boot-arg bt_baud=3000000; without it the line stays at 115200). At
 * 32x oversampling the 7.3728 MHz clock tops out at 230400, so the engine's clock first goes to 96 MHz: GPLL0-even
 * 300 MHz x 8/25 (one of Linux's QUP DFS rates), divider 1 at 3 Mbit/s. The controller's side follows hci_qca.c
 * qca_set_speed for the WCN399x: RTS held "not ready" (so nothing comes in mid-change), EDL_SET_BAUDRATE (0xfc48, one
 * byte: QCA's baud index, 14 = 3000000) sent and drained, 10 ms, the host's speed changed, RTS back to the hardware;
 * the controller then sends a command complete at the new speed. Checked with Read_Local_Version; on any failure
 * both sides go back to 115200.
 */
static bool
bt_set_fast(uint32_t baud)
{
	static const uint8_t local_version[] = { 0x01, 0x01, 0x10, 0x00 };
	uint8_t cmd[5] = { 0x01, 0x48, 0xfc, 0x01, 14 }, ans[64];
	uint32_t rcg = BT_GCC_S3_RCG & 0xfff, n;

	if (baud != 3000000 || bt_rcg_page == 0) {
		printf("bt: stage 4: baud %u not handled (3000000 only): the line stays at 115200\n", baud);
		return false;
	}
	wr(bt_se + UART_MANUAL_RFR, (1u << 31) | (1u << 1));           /* RTS: not ready */
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!bt_uart_write(cmd, sizeof(cmd))) {
		printf("bt: stage 4: EDL_SET_BAUDRATE NOT SENT\n");
		bt_uart_flow(true);
		return false;
	}
	delay_for_interval(10, NSEC_PER_MSEC);
	/* the engine's clock to 96 MHz (GPLL0 even 300 MHz x 8/25), then divider 1 */
	wr(bt_rcg_page + rcg + 8, 8);                                   /* M */
	wr(bt_rcg_page + rcg + 12, ~(25u - 8u) & 0xffff);               /* N: ~(n - m) */
	wr(bt_rcg_page + rcg + 16, ~(25u - 8u) & 0xffff);               /* D */
	wr(bt_rcg_page + rcg, rd(bt_rcg_page + rcg) | 1);               /* update */
	for (int us = 0; us < 10000 && (rd(bt_rcg_page + rcg) & 1); us += 10) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	bt_se_hz = 96000000u;
	bt_uart_speed(baud);
	bt_uart_flow(true);                                             /* RTS: by the hardware again */
	n = bt_uart_read(ans, sizeof(ans), 200);
	bt_show("stage 4: at 3 Mbit/s, the controller's answer", ans, n);
	(void)bt_uart_write(local_version, sizeof(local_version));
	n = bt_uart_read(ans, sizeof(ans), 200);
	bt_show("stage 4: Read_Local_Version at 3 Mbit/s", ans, n);
	if (n >= 7 && ans[0] == 0x04 && ans[1] == 0x0e && ans[4] == 0x01 && ans[5] == 0x10) {
		printf("bt: stage 4: THE LINE RUNS AT 3 Mbit/s (engine clock %u Hz, RCG cmd %#x cfg %#x)\n", bt_se_hz,
		    rd(bt_rcg_page + rcg), rd(bt_rcg_page + rcg + 4));
		return true;
	}
	/* back: the clock to 7.3728 MHz and the host to 115200 (the controller may still be at 3 Mbit/s: said) */
	wr(bt_rcg_page + rcg + 8, 384);
	wr(bt_rcg_page + rcg + 12, ~(15625u - 384u) & 0xffff);
	wr(bt_rcg_page + rcg + 16, ~(15625u - 384u) & 0xffff);
	wr(bt_rcg_page + rcg, rd(bt_rcg_page + rcg) | 1);
	delay_for_interval(1, NSEC_PER_MSEC);
	bt_se_hz = 7372800u;
	bt_uart_speed(115200);
	printf("bt: stage 4: 3 Mbit/s FAILED: the host back at 115200 (the controller's speed now unknown)\n");
	return false;
}

static void
bt_stage4(void)
{
	static struct kern_ctl_reg reg;
	static uint8_t rx[512], pkt[BT_PKT], tx[BT_TXQ];
	uint32_t plen = 0, n;
	uint64_t last_count = 0;
	int major;

	PE_parse_boot_argn("bt_vsc", &bt_vsc, sizeof(bt_vsc));
	{
		uint32_t baud = 0;

		if (PE_parse_boot_argn("bt_baud", &baud, sizeof(baud)) && baud > 115200) {
			(void)bt_set_fast(baud);
		}
	}
	/*
	 * (Test 262) The address: the loader's mac-address-bluetooth0 (02:52:44:57:00:02, what MobileGestalt reports).
	 * Into the controller by QCA's EDL_WRITE_BD_ADDR (0xfc14, the address most significant byte first, as Linux
	 * btqca.c qca_set_bdaddr), and onto the "bluetooth" IOService as local-mac-address, where bluetoothd reads it
	 * (without it: "Not a valid MAC address (all 00s)" and exit, Test 256; or a made-up one, wlan.debug.generate-mac).
	 */
	{
		static const uint8_t address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
		uint8_t cmd[10] = { 0x01, 0x14, 0xfc, 6 }, ans[64];
		static const uint8_t read_addr[] = { 0x01, 0x09, 0x10, 0x00 };

		memcpy(cmd + 4, address, 6);
		(void)bt_uart_write(cmd, sizeof(cmd));
		n = bt_uart_read(ans, sizeof(ans), 200);
		bt_show("stage 4: EDL_WRITE_BD_ADDR's answer", ans, n);
		(void)bt_uart_write(read_addr, sizeof(read_addr));
		n = bt_uart_read(ans, sizeof(ans), 200);
		bt_show("stage 4: Read_BD_ADDR now", ans, n);
		printf("bt: stage 4: IOService \"bluetooth\" (local-mac-address 02:52:44:57:00:02) %s\n",
		    redwood_publish_named_service("bluetooth", "local-mac-address", address, sizeof(address)) ?
		    "published" : "NOT published");
	}
	strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;      /* (not privileged: bluetoothd runs as mobile) */
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = bt_ctl_connect;
	reg.ctl_disconnect = bt_ctl_disconnect;
	reg.ctl_send = bt_ctl_send;
	reg.ctl_setopt = bt_ctl_setopt;
	reg.ctl_getopt = bt_ctl_getopt;
	if (ctl_register(&reg, &bt_kctl) != 0) {
		printf("bt: stage 4: the kernel control could not be registered\n");
		return;
	}
	major = cdevsw_add(-1, &bt_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth") == NULL ||
	    devfs_make_node(makedev(major, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btwake") == NULL) {
		printf("bt: stage 4: /dev/uart.bluetooth could not be made (major %d)\n", major);
	}
	printf("bt: stage 4: THE CONTROLLER IS IOS'S: kernel control com.apple.uart.bluetooth, /dev/uart.bluetooth; vendor "
	    "commands answered here (bt_vsc=%u)\n", bt_vsc);
	for (;;) {
		uint32_t q;

		lck_mtx_lock(&bt_lock);
		q = bt_txq_len;
		memcpy(tx, bt_txq, q);
		bt_txq_len = 0;
		lck_mtx_unlock(&bt_lock);
		if (q != 0 && !bt_uart_write(tx, q)) {
			printf("bt: stage 4: %u bytes to the controller NOT SENT\n", q);
		}
		/* the controller's bytes: whole packets to the client */
		n = bt_uart_read(rx, sizeof(rx), 1);
		for (uint32_t i = 0; i < n; i++) {
			int want;

			if (plen == 0) {
				if (rx[i] == 0xfd) {            /* QCA in-band sleep: "awake", answered "ack" */
					static const uint8_t ack = 0xfc;

					bt_n_ibs++;
					(void)bt_uart_write(&ack, 1);
					continue;
				}
				if (rx[i] == 0xfe || rx[i] == 0xfc) {   /* "going to sleep" / "ack" */
					bt_n_ibs++;
					continue;
				}
				if (rx[i] != 0x02 && rx[i] != 0x03 && rx[i] != 0x04 && rx[i] != 0x05) {
					bt_n_junk++;
					continue;
				}
			}
			pkt[plen++] = rx[i];
			want = bt_h4_len(pkt, plen);
			if (want > BT_PKT) {
				bt_n_junk++;
				plen = 0;
			} else if (want > 0 && plen == (uint32_t)want) {
				if (pkt[0] == 0x04) {
					bt_n_evt++;
					/* Read_Local_Supported_Commands' answer: the commands answered here said to be there */
					if (pkt[1] == 0x0e && plen >= 7 + 64 && pkt[4] == 0x02 && pkt[5] == 0x10 && pkt[6] == 0) {
						for (unsigned f = 0; f < sizeof(bt_fake) / sizeof(bt_fake[0]); f++) {
							pkt[7 + bt_fake[f].octet] |= (uint8_t)(1u << bt_fake[f].bit);
						}
					}
				} else {
					bt_n_racl++;
				}
				if (bt_said_c++ < 150) {
					bt_show("stage 4: controller ->", pkt, plen);
				}
				bt_to_client(pkt, plen);
				plen = 0;
			}
		}
		if (q == 0 && n == 0) {
			sleep_ms(1);
		}
		if (bt_n_cmd + bt_n_evt >= last_count + 500) {
			last_count = bt_n_cmd + bt_n_evt;
			printf("bt: stage 4: host commands %llu, ACL %llu; events %llu, ACL in %llu; vendor commands answered here %llu; "
			    "dropped %llu, junk %llu, sleep bytes %llu\n", bt_n_cmd, bt_n_acl, bt_n_evt, bt_n_racl, bt_n_vsc, bt_n_drop,
			    bt_n_junk, bt_n_ibs);
		}
	}
}

/*
 * (r5q Test 352) Cellular, the first look: iOS's CommCenter speaks QMI to its baseband (libQMIParserDynamic, QMI_NAS /
 * QMI_DMS / QMI_BSP), and this phone's modem offers the standard QMI services over QRTR (node 0: WDS 0x1, DMS 0x2, NAS
 * 0x3, WMS 0x5, VOICE 0x9, UIM 0xb ...). Before any bridge: does the modem work here -- a SIM, its state, the modem's
 * operating mode, network registration, signal. Boot-arg cell=1 reads them; cell=2 also sets the modem online (DMS
 * SET_OPERATING_MODE, as Android's HLOS does) when it is in low power, then watches registration for ~45 s. Only states
 * are logged: no IMSI, ICCID, IMEI, phone number or operator code.
 */
static struct redwood_qrtr_port *cell_port;
static uint8_t cell_answer[1024];
static volatile size_t cell_answer_len;
static volatile uint16_t cell_answer_msg;
static uint16_t cell_txn = 1;

static void
cell_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;

	if (length < 7 || m[0] != 2) {          /* responses only (indications are left) */
		return;
	}
	if (length > sizeof(cell_answer)) {
		length = sizeof(cell_answer);
	}
	memcpy(cell_answer, data, length);
	cell_answer_msg = (uint16_t)(m[3] | m[4] << 8);
	__asm__ volatile ("dmb ish" ::: "memory");
	cell_answer_len = length;
}

static bool
cell_where(uint32_t service, uint32_t *node, uint32_t *port)
{
	bool found = false;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && !found; i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == service && qrtr_servers[i].node == 0) {
			*node = qrtr_servers[i].node;
			*port = qrtr_servers[i].port;
			found = true;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return found;
}

/* a request to `service`; the answer's TLVs (after the 7-byte header) and their length, or NULL; *result the QMI result */
static const uint8_t *
cell_ask(uint32_t service, uint16_t msg, const uint8_t *tlvs, size_t tlvs_len, size_t *out_len, uint16_t *result,
    uint16_t *error)
{
	uint8_t out[64];
	uint32_t node, port;
	uint16_t txn = cell_txn++;
	const uint8_t *r;
	uint16_t rl = 0;

	if (!cell_where(service, &node, &port) || tlvs_len > sizeof(out) - 7) {
		return NULL;
	}
	out[0] = 0;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	out[5] = (uint8_t)tlvs_len;
	out[6] = (uint8_t)(tlvs_len >> 8);
	if (tlvs_len != 0) {
		memcpy(out + 7, tlvs, tlvs_len);
	}
	cell_answer_len = 0;
	if (qrtr_send(cell_port, node, port, out, 7 + tlvs_len) != 0) {
		return NULL;
	}
	for (unsigned ms = 0; ms < 3000; ms += 10) {
		if (cell_answer_len != 0 && cell_answer_msg == msg) {
			*out_len = cell_answer_len - 7;
			r = qmi_tlv(cell_answer + 7, *out_len, 0x02, &rl);
			*result = r != NULL && rl >= 4 ? (uint16_t)(r[0] | r[1] << 8) : 0xffff;
			*error = r != NULL && rl >= 4 ? (uint16_t)(r[2] | r[3] << 8) : 0xffff;
			return cell_answer + 7;
		}
		sleep_ms(10);
	}
	return NULL;
}

/*
 * (Test 380) iOS's QMI to the modem: the bridge RedwoodBaseband.cpp's QMUX transport uses. CommCenter's QMI clients
 * (one QMUX client ID per service client) speak through Apple's PCI transport; here each (service, client) gets its
 * own QRTR port, and its QMI messages -- the same bytes in QMUX and in QRTR: flags, transaction, message, length,
 * TLVs -- go to the modem's server of that service as they are. What the modem sends that port (responses,
 * indications) goes back through RedwoodBasebandQmiReceive(). Services the modem does not offer get no port.
 */
extern void RedwoodBasebandQmiReceive(void *client, uint8_t service, uint8_t qmux_client, const void *message,
    size_t length);

struct bb_bridge {
	bool                            used;
	void                            *client;
	uint8_t                         service, qmux_client;
	struct redwood_qrtr_port        *port;
};
static struct bb_bridge bb_bridges[96];
static LCK_MTX_DECLARE(bb_bridge_lock, &wifi_lock_group);

static void
bb_bridge_receive(void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	struct bb_bridge *b = context;

	if (b->used) {
		RedwoodBasebandQmiReceive(b->client, b->service, b->qmux_client, data, length);
	}
}

/* 0 sent; ENOENT the modem has no server of that service; another errno otherwise */
int redwood_bb_qmi_send(void *client, uint8_t service, uint8_t qmux_client, const void *message, size_t length);
int
redwood_bb_qmi_send(void *client, uint8_t service, uint8_t qmux_client, const void *message, size_t length)
{
	struct bb_bridge *b = NULL;
	uint32_t node, port;

	if (!cell_where(service, &node, &port)) {
		return ENOENT;
	}
	lck_mtx_lock(&bb_bridge_lock);
	for (unsigned i = 0; i < sizeof(bb_bridges) / sizeof(bb_bridges[0]) && b == NULL; i++) {
		if (bb_bridges[i].used && bb_bridges[i].client == client && bb_bridges[i].service == service &&
		    bb_bridges[i].qmux_client == qmux_client) {
			b = &bb_bridges[i];
		}
	}
	for (unsigned i = 0; i < sizeof(bb_bridges) / sizeof(bb_bridges[0]) && b == NULL; i++) {
		if (!bb_bridges[i].used) {
			b = &bb_bridges[i];
			b->client = client;
			b->service = service;
			b->qmux_client = qmux_client;
			b->port = qrtr_open(bb_bridge_receive, NULL, b);
			if (b->port == NULL) {
				b = NULL;
				break;
			}
			b->used = true;
			tell("cell: bridge: service 0x%x client %u has QRTR port %u (to %u:%u)\n", service, qmux_client,
			    b->port->number, node, port);
		}
	}
	lck_mtx_unlock(&bb_bridge_lock);
	return b == NULL ? ENOSPC : qrtr_send(b->port, node, port, message, length);
}

/* every port of that connection closed (CommCenter's transport closes): no callback for it after this returns */
void redwood_bb_qmi_close(void *client);
void
redwood_bb_qmi_close(void *client)
{
	for (unsigned i = 0; i < sizeof(bb_bridges) / sizeof(bb_bridges[0]); i++) {
		struct redwood_qrtr_port *port = NULL;

		lck_mtx_lock(&bb_bridge_lock);
		if (bb_bridges[i].used && bb_bridges[i].client == client) {
			port = bb_bridges[i].port;
			bb_bridges[i].used = false;
		}
		lck_mtx_unlock(&bb_bridge_lock);
		if (port != NULL) {
			qrtr_close(port);
		}
	}
}

static void
cell_serving(const char *when)
{
	static const char *const reg[] = { "not registered", "REGISTERED", "searching", "registration denied", "unknown" };
	const uint8_t *a, *v;
	size_t n = 0;
	uint16_t res = 0, err = 0, vl = 0;
	char radios[64] = "";

	a = cell_ask(0x3, 0x24, NULL, 0, &n, &res, &err);
	if (a == NULL || res != 0 || (v = qmi_tlv(a, n, 0x01, &vl)) == NULL || vl < 5) {
		printf("cell: %s: NAS serving system: %s (result %u error %u)\n", when, a ? "no answer TLV" : "NO ANSWER", res, err);
		return;
	}
	for (unsigned i = 0, at = 0; i < v[4] && 5u + i < vl && at < sizeof(radios) - 8; i++) {
		static const char *const rif[] = { "none", "CDMA", "HDR", "AMPS", "GSM", "UMTS", "?6", "?7", "LTE", "TD-SCDMA" };

		at += snprintf(radios + at, sizeof(radios) - at, " %s", v[5 + i] < 10 ? rif[v[5 + i]] : "other");
	}
	printf("cell: %s: NAS serving system: %s, CS %s, PS %s, network type %u, radio:%s\n", when, v[0] < 5 ? reg[v[0]] : "?",
	    v[1] == 1 ? "attached" : "detached", v[2] == 1 ? "attached" : "detached", v[3], radios[0] ? radios : " none");
	a = cell_ask(0x3, 0x20, NULL, 0, &n, &res, &err);
	if (a != NULL && res == 0 && (v = qmi_tlv(a, n, 0x01, &vl)) != NULL && vl >= 2) {
		printf("cell: %s: NAS signal strength %d dBm (radio %u)\n", when, (int8_t)v[0], v[1]);
	} else {
		printf("cell: %s: NAS signal strength: none (result %u error %u)\n", when, res, err);
	}
}

/*
 * (Test 353) The SIM slots: UIM GET_CARD_STATUS (0x2f), TLV 0x10 {four u16 session indexes, u8 cards, per card {state,
 * UPIN state, UPIN retries, UPUK retries, error code, u8 apps, per app {type, state, perso state, perso feature, perso
 * retries, perso unblock retries, u8 AID length, AID, univ PIN, PIN1 state, PIN1 retries, PUK1 retries, PIN2 state,
 * PIN2 retries, PUK2 retries}}}. Test 352 read card 1 "ERROR", 0 applications -- before the modem was online.
 */
static void
cell_sim(const char *when)
{
	static const char *const card[] = { "ABSENT", "PRESENT", "ERROR" };
	static const char *const app[] = { "unknown", "detected", "PIN1 or UPIN required", "PUK1 required", "personalisation",
		                           "PIN1 blocked", "illegal", "READY" };
	static const char *const cerr[] = { "unknown", "power down", "poll error", "no ATR", "voltage mismatch", "parity",
		                            "possibly removed", "technical problem", "null bytes", "SAP connected" };
	const uint8_t *a, *v;
	size_t n = 0;
	uint16_t res = 0, err = 0, vl = 0;

	a = cell_ask(0xb, 0x2f, NULL, 0, &n, &res, &err);
	if (a == NULL || res != 0 || (v = qmi_tlv(a, n, 0x10, &vl)) == NULL || vl < 9) {
		printf("cell: %s: SIM: UIM card status: %s (result %u error %u)\n", when, a ? "no card TLV" : "NO ANSWER", res, err);
		return;
	}
	for (unsigned c = 0, at = 9; c < v[8] && at + 6 <= vl; c++) {
		uint8_t state = v[at], ecode = v[at + 4], apps = v[at + 5];
		const char *first = "";

		at += 6;
		for (unsigned p = 0; p < apps && at + 7 <= vl; p++) {
			uint8_t aid = v[at + 6];

			if (p == 0) {
				first = v[at + 1] < 8 ? app[v[at + 1]] : "?";
			}
			at += 7 + aid + 7;
		}
		printf("cell: %s: SIM slot %u: %s%s%s, %u application(s)%s%s\n", when, c + 1, state < 3 ? card[state] : "?",
		    state == 2 ? ", error " : "", state == 2 ? (ecode < 10 ? cerr[ecode] : "?") : "", apps, apps ? ", the first " : "",
		    first);
	}
}

static void
cell_probe(__unused void *arg, __unused wait_result_t wr_unused)
{
	static const char *const modes[] = { "ONLINE", "low power", "factory test", "offline", "resetting", "shutting down",
		                             "persistent low power", "mode-only low power" };
	uint32_t level = 0, node, port;
	const uint8_t *a, *v;
	size_t n = 0;
	uint16_t res = 0, err = 0, vl = 0;

	(void)PE_parse_boot_argn("cell", &level, sizeof(level));
	for (unsigned s = 0; s < 240 && !(cell_where(0x3, &node, &port) && cell_where(0x2, &node, &port) &&
	    cell_where(0xb, &node, &port)); s++) {
		sleep_ms(500);
	}
	if (!cell_where(0x3, &node, &port) || !cell_where(0x2, &node, &port) || !cell_where(0xb, &node, &port)) {
		printf("cell: the modem's NAS, DMS and UIM services are not all there: no look\n");
		return;
	}
	cell_port = qrtr_open(cell_receive, NULL, NULL);
	if (cell_port == NULL) {
		printf("cell: no QRTR port\n");
		return;
	}
	sleep_ms(5000);                         /* (the modem settles; the SIM is read) */
	cell_sim("before online");
	/* the modem: DMS GET_OPERATING_MODE (0x2d), TLV 0x01 */
	a = cell_ask(0x2, 0x2d, NULL, 0, &n, &res, &err);
	if (a != NULL && res == 0 && (v = qmi_tlv(a, n, 0x01, &vl)) != NULL && vl >= 1) {
		printf("cell: modem operating mode: %s (%u)\n", v[0] < 8 ? modes[v[0]] : "?", v[0]);
		if (level >= 2 && v[0] != 0) {
			static const uint8_t online[] = { 0x01, 0x01, 0x00, 0x00 };     /* TLV 0x01, length 1: 0 = online */

			a = cell_ask(0x2, 0x2e, online, sizeof(online), &n, &res, &err);
			printf("cell: modem set ONLINE: %s (result %u error %u)\n", a ? (res == 0 ? "taken" : "refused") : "NO ANSWER",
			    res, err);
		}
	} else {
		printf("cell: modem operating mode: %s (result %u error %u)\n", a ? "no TLV" : "NO ANSWER", res, err);
	}
	cell_serving("now");
	if (level >= 2) {
		for (unsigned s = 5; s <= 45; s += 5) {
			char when[16];

			sleep_ms(5000);
			snprintf(when, sizeof(when), "+%u s", s);
			cell_serving(when);
			if (s == 10 || s == 30 || s == 45) {
				cell_sim(when);         /* (Test 353) the SIM again, the modem online now */
			}
		}
	}
	qrtr_close(cell_port);
	cell_port = NULL;
}

/*
 * (Test 358) The stubs' answers, settable while iOS runs: kern.redwood.bb_answer = selector << 16 | value sets the
 * first scalar output the stubs return for that selector (Test 357: AppleBaseband selector 3, the baseband state,
 * answered 0, and AppleBasebandManager went no further). Read: the last setting. r5q_sysctl NAME VALUE over r5qd.
 */
#include <sys/sysctl.h>
uint64_t redwood_bb_answer[64];
static int redwood_bb_answer_last;
SYSCTL_DECL(_kern_redwood);

static int
bb_answer_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = redwood_bb_answer_last, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (((uint32_t)value >> 16) >= 64) {
		return EINVAL;
	}
	redwood_bb_answer[(uint32_t)value >> 16] = (uint32_t)value & 0xffff;
	redwood_bb_answer_last = value;
	printf("cell: the baseband stubs now answer selector %u with %u\n", (uint32_t)value >> 16, (uint32_t)value & 0xffff);
	return 0;
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, bb_answer, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, bb_answer_sysctl, "I",
    "the baseband stubs' answer: selector << 16 | value");

/* (Test 355) bbstub=1: iOS's baseband services as listening stubs (iokit/Kernel/RedwoodBaseband.cpp), 5 s in */
static void
bb_publish_thread(__unused void *arg, __unused wait_result_t wr_unused)
{
	extern void RedwoodBasebandPublish(void);

	sleep_ms(5000);
	RedwoodBasebandPublish();
}

static void
bt_stage1(__unused void *arg, __unused wait_result_t wr_unused)
{
	static const char *const supplies[] = { "ldoa7", "ldoc2", "ldoc11" };
	static const uint32_t pins[] = { 43, 44, 45, 46 };
	static const char *const pin_use[] = { "CTS", "RTS", "TX", "RX" };
	uint32_t bt = 0;
	vm_offset_t gcc, tlmm, se, db;

	PE_parse_boot_argn("bt", &bt, sizeof(bt));
	sleep_ms(15000);                        /* (beside the system's start, after Wi-Fi's own early work) */
	/*
	 * (Test 246) Test 245 stopped right after "mapping": one of three large mappings (GCC 0x60000, the east tile's 47
	 * pages, cmd-db again) never returned. Now only the pages read, one at a time, each said first; cmd-db is not
	 * mapped again (the Wi-Fi driver's rails code already says the three supplies' addresses).
	 */
	(void)db;
	printf("bt: stage 1 (Test 246): started; mapping GCC page 0x152000\n");
	gcc = 0;
	{
		vm_offset_t vote = ml_io_map(BT_GCC_PHYS + (BT_GCC_VOTE & ~0xfffu), 0x1000), wrap2;

		printf("bt: mapped %#lx; next GCC page 0x11e000\n", (unsigned long)vote);
		wrap2 = ml_io_map(BT_GCC_PHYS + (BT_GCC_S3_CBCR & ~0xfffu), 0x1000);
		printf("bt: mapped %#lx; next the pins' pages\n", (unsigned long)wrap2);
		if (vote == 0 || wrap2 == 0) {
			goto out;
		}
		printf("bt: the UART's clocks (bt=%u): vote %#x (S3 %u, M_AHB %u, S_AHB %u)\n", bt, rd(vote + (BT_GCC_VOTE & 0xfff)),
		    (rd(vote + (BT_GCC_VOTE & 0xfff)) >> 7) & 1, (rd(vote + (BT_GCC_VOTE & 0xfff)) >> 2) & 1,
		    (rd(vote + (BT_GCC_VOTE & 0xfff)) >> 1) & 1);
		printf("bt: CBCR S3 %#x, M_AHB %#x, S_AHB %#x (bit 31 = off); RCG cmd %#x cfg %#x m %#x n %#x d %#x\n",
		    rd(wrap2 + (BT_GCC_S3_CBCR & 0xfff)), rd(wrap2 + (BT_GCC_M_AHB_CBCR & 0xfff)),
		    rd(wrap2 + (BT_GCC_S_AHB_CBCR & 0xfff)), rd(wrap2 + (BT_GCC_S3_RCG & 0xfff)), rd(wrap2 + (BT_GCC_S3_RCG & 0xfff) + 4),
		    rd(wrap2 + (BT_GCC_S3_RCG & 0xfff) + 8), rd(wrap2 + (BT_GCC_S3_RCG & 0xfff) + 12),
		    rd(wrap2 + (BT_GCC_S3_RCG & 0xfff) + 16));
		/*
		 * (Test 247) Test 246: the first access to GPIO43's page ended the whole system (back to the loader). The
		 * pins are not touched again; bt=3 goes straight on to the serial engine.
		 */
		for (unsigned i = 0; i < 4 && bt < 3; i++) {
			vm_offset_t pin = ml_io_map(BT_TLMM_EAST + 0x1000 * pins[i], 0x1000);
			uint32_t cfg, io;

			if (pin == 0) {
				continue;
			}
			cfg = rd(pin);
			io = rd(pin + 4);
			printf("bt: GPIO%u (%s): cfg %#x (function %u, drive %u, pull %u, output %u), io %#x\n", pins[i], pin_use[i], cfg,
			    (cfg >> 2) & 0xf, (cfg >> 6) & 7, cfg & 3, (cfg >> 9) & 1, io);
		}
		gcc = vote;             /* (below: the vote register through this page) */
		tlmm = wrap2;           /* (below: the CBCRs through this page) */
	}
	if (bt >= 7 && bt <= 9) {
		bt_stage2(gcc, tlmm);           /* (gcc: the vote register's page; tlmm: here the wrapper-2 clocks' page) */
		if (bt >= 8 && bt_se != 0) {
			bt_stage3();            /* (Test 255) the firmware */
		}
		if (bt == 9 && bt_se != 0) {
			bt_stage4();            /* (Test 256) to iOS; does not return */
		}
		goto out;
	}
	if (bt == 6) {
		/*
		 * (Test 253) Before a baud rate can be set: Linux drives the QUP serial-engine clocks on SM8150 by DFS (the
		 * RCG's preset perf levels, picked by the engine's SE_GENI_CLK_SEL). Read only: the wrapper's version
		 * (0xcc0000 + 4: 16x or 32x UART oversampling), the RCG's DFS enable (cmd + 0x14) and its 8 levels (cfg at
		 * cmd + 0x1c + 4l, M at + 0x5c + 4l, N at + 0x9c + 4l), the engine's CLK_SEL (0x7c) and M/S clock configs.
		 */
		vm_offset_t wrapper = ml_io_map(0x00cc0000UL, 0x1000), se3 = ml_io_map(BT_SE_PHYS, 0x1000);
		uint32_t rcg = BT_GCC_S3_RCG & 0xfff;

		wr(gcc + (BT_GCC_VOTE & 0xfff), rd(gcc + (BT_GCC_VOTE & 0xfff)) | (1u << 7) | (1u << 2) | (1u << 1));
		__asm__ volatile ("dsb sy" ::: "memory");
		delay_for_interval(5, NSEC_PER_MSEC);

		printf("bt: clocks: QUP wrapper 2 version %#x; RCG cmd %#x cfg %#x, DFS %#x (bit 0: DFS on)\n",
		    wrapper ? rd(wrapper + 4) : 0, rd(tlmm + rcg), rd(tlmm + rcg + 4), rd(tlmm + rcg + 0x14));
		for (unsigned l = 0; l < 8; l++) {
			uint32_t cfg = rd(tlmm + rcg + 0x1c + 4 * l), m = rd(tlmm + rcg + 0x5c + 4 * l), n = rd(tlmm + rcg + 0x9c + 4 * l);

			printf("bt: clocks: level %u: cfg %#x (source %u, div field %u, mode %u), M %#x, N %#x\n", l, cfg,
			    (cfg >> 8) & 7, cfg & 0x1f, (cfg >> 12) & 3, m, n);
		}
		if (se3 != 0) {
			printf("bt: clocks: SE3 CLK_SEL %#x, M_CLK_CFG %#x, S_CLK_CFG %#x\n", rd(se3 + 0x7c), rd(se3 + 0x48),
			    rd(se3 + 0x4c));
		}
		goto out;
	}
	if (bt == 5) {
		/*
		 * (Test 251) Mapping or reading? Tests 246-248 all died right after "mapping": an early note says a large GCC
		 * mapping "hung the thread silently" (and the system watchdog would then end it). Here: the map, a line, 2 s, a
		 * line, and only then the read -- each said before it is done.
		 */
		vm_offset_t pin47;

		printf("bt: map-or-read: mapping GPIO47's page (0x0352f000)\n");
		pin47 = ml_io_map(BT_TLMM_EAST + 0x1000 * 47, 0x1000);
		printf("bt: map-or-read: MAPPED at %#lx; waiting 2 s before any read\n", (unsigned long)pin47);
		delay_for_interval(2000, NSEC_PER_MSEC);
		printf("bt: map-or-read: still alive 2 s after the mapping; now reading cfg\n");
		delay_for_interval(500, NSEC_PER_MSEC);
		printf("bt: map-or-read: GPIO47 cfg %#x: READ FINE\n", rd(pin47));
		goto out;
	}
	if (bt == 4) {
		/*
		 * (Test 248) What is fenced? Tests 246/247: GPIO43's page and SE13's page each ended the system. Here the
		 * neighbours, each said first: GPIO47 (the same east tile, SE14's pin), then SE12 (0xc88000, the same QUP
		 * wrapper 2, an I2C Android uses) with its clock S2 (vote bit 6, CBCR 0x1e3a4) voted on. If GPIO47 reads,
		 * the fence is per pin; if SE12 reads, the wrapper as a whole is not fenced.
		 */
		vm_offset_t pin47, se12;

		printf("bt: probe: GPIO47 (east tile, next to the Bluetooth pins): mapping\n");
		pin47 = ml_io_map(BT_TLMM_EAST + 0x1000 * 47, 0x1000);
		printf("bt: probe: GPIO47 mapped; reading ...\n");
		delay_for_interval(300, NSEC_PER_MSEC);
		printf("bt: probe: GPIO47 cfg %#x io %#x: READ FINE\n", rd(pin47), rd(pin47 + 4));
		printf("bt: probe: SE12's clock S2 voted on\n");
		wr(gcc + (BT_GCC_VOTE & 0xfff), rd(gcc + (BT_GCC_VOTE & 0xfff)) | (1u << 6));
		__asm__ volatile ("dsb sy" ::: "memory");
		delay_for_interval(5, NSEC_PER_MSEC);
		printf("bt: probe: CBCR S2 %#x\n", rd(tlmm + 0x3a4));
		se12 = ml_io_map(0x00c88000UL, 0x1000);
		printf("bt: probe: SE12 mapped; reading its firmware revision ...\n");
		delay_for_interval(300, NSEC_PER_MSEC);
		printf("bt: probe: SE12 firmware %#x (protocol %u: 3 = I2C): READ FINE\n", rd(se12 + 0x68), (rd(se12 + 0x68) >> 8) & 0xff);
		goto out;
	}
	if (bt < 2) {
		goto out;
	}
	for (unsigned i = 0; i < 3 && bt < 3; i++) {     /* (bt=3: Wi-Fi's rails code votes these already) */
		printf("bt: supply %s: %s\n", supplies[i], redwood_r5q_rpmh_enable(supplies[i]) ? "voted on" : "NOT VOTED");
	}
#define BT_VOTE         (gcc + (BT_GCC_VOTE & 0xfff))           /* (gcc: the vote register's page) */
#define BT_CBCRS        (rd(tlmm + (BT_GCC_S3_CBCR & 0xfff)) | rd(tlmm + (BT_GCC_M_AHB_CBCR & 0xfff)) | \
	                 rd(tlmm + (BT_GCC_S_AHB_CBCR & 0xfff)))   /* (tlmm: here the CBCRs' page) */
	printf("bt: voting the UART's clocks on\n");
	wr(BT_VOTE, rd(BT_VOTE) | (1u << 7) | (1u << 2) | (1u << 1));
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int ms = 0; ms < 50 && (BT_CBCRS >> 31); ms++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("bt: the UART's clocks voted on: vote %#x, CBCR S3 %#x, M_AHB %#x, S_AHB %#x\n", rd(BT_VOTE),
	    rd(tlmm + (BT_GCC_S3_CBCR & 0xfff)), rd(tlmm + (BT_GCC_M_AHB_CBCR & 0xfff)), rd(tlmm + (BT_GCC_S_AHB_CBCR & 0xfff)));
	/* (Test 247) S_AHB read 0x80000002 with its vote on in Test 246: hardware clock gating; S3 and M_AHB decide */
	if ((rd(tlmm + (BT_GCC_S3_CBCR & 0xfff)) | rd(tlmm + (BT_GCC_M_AHB_CBCR & 0xfff))) >> 31) {
		printf("bt: a clock is still off: the engine is not read\n");
		goto out;
	}
	printf("bt: mapping the engine's first page (0xc8c000)\n");
	se = ml_io_map(BT_SE_PHYS, 0x4000);
	if (se == 0) {
		goto out;
	}
	printf("bt: mapped; reading its firmware revision (+0x68) ...\n");
	delay_for_interval(300, NSEC_PER_MSEC);
	printf("bt: firmware revision %#x\n", rd(se + 0x68));
	delay_for_interval(300, NSEC_PER_MSEC);
	printf("bt: the engine (0xc8c000): firmware %#x (protocol %u: 2 = UART), IF_DISABLE %#x (bit 0: no FIFO), status %#x, "
	    "DMA mode %#x, HW params %#x %#x, clock cfg %#x\n", rd(se + 0x68), (rd(se + 0x68) >> 8) & 0xff, rd(se + 0x64),
	    rd(se + 0x40), rd(se + 0x258), rd(se + 0xe24), rd(se + 0xe28), rd(se + 0x48));
out:
	printf("bt: stage 1 done\n");
	thread_terminate(current_thread());
}

void
redwood_wifi_init(void)
{
	uint32_t on = 0, back = 0, done = 0;
	uint64_t size = 0;
	thread_t thread;

	if (redwood_platform_is("y2q")) {
		brcm_init();
		return;
	}
	if (redwood_platform_is("r5q")) {
		if (PE_parse_boot_argn("wifi_survey", &on, sizeof(on)) && on != 0) {
			r5q_wifi_survey();
		}
		on = 0;
		if (PE_parse_boot_argn("wifi_wlfw", &on, sizeof(on)) && on >= 3 &&
		    !(PE_parse_boot_argn("wifi_net", &back, sizeof(back)) && back == 0)) {
			r5q_net_early();                /* (Test 120) en0 now, its link up once joined */
		}
		on = 0;
		if (PE_parse_boot_argn("wifi_modem", &on, sizeof(on)) && on != 0) {
			if (kernel_thread_start(r5q_modem_link, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
		on = 0;
		if (PE_parse_boot_argn("bt", &on, sizeof(on)) && on != 0 &&
		    kernel_thread_start(bt_stage1, NULL, &thread) == KERN_SUCCESS) {      /* (Test 244) Bluetooth */
			thread_deallocate(thread);
		}
		on = 0;
		if (PE_parse_boot_argn("cell", &on, sizeof(on)) && on != 0 &&
		    kernel_thread_start(cell_probe, NULL, &thread) == KERN_SUCCESS) {     /* (Test 352) cellular, a look */
			thread_deallocate(thread);
		}
		on = 0;
		if (PE_parse_boot_argn("bbstub", &on, sizeof(on)) && on != 0 &&
		    kernel_thread_start(bb_publish_thread, NULL, &thread) == KERN_SUCCESS) {   /* (Test 355) */
			thread_deallocate(thread);
		}
		return;
	}
	PE_parse_boot_argn("wifi_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("wifi", &on, sizeof(on)) || on == 0)) {
		return;
	}
	if (last_step == 0 && on != 1) {
		printf("wifi: wifi=%u is not understood: wifi=1 turns the radio on, wifi_step=N goes the first N steps of that\n", on);
		return;
	}
	if (pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
		if (last_step != 0 && PE_parse_boot_argn("wifi_lx", &back, sizeof(back)) && back) {
			/* a check of what is under the driver: it gets as far as finding no firmware to talk to */
			printf("wifi: this is not the phone; with wifi_lx the driver is started all the same, for what it does before "
			    "it needs a radio\n");
			printf("wifi: the driver's start returns %d\n", redwood_lx_start());
		} else {
			printf("wifi: wifi=1 and wifi_step=N are for the phone's radio, and this is not the phone\n");
		}
		return;
	}
	redwood_qmi_set_transport(&qrtr_ops);
	if (last_step == 0) {
		if (image_file("wpss.mdt", &size) == NULL) {
			printf("wifi: wifi=1, but the radio's firmware is not in the small image (fw-wifi/, see the README): no Wi-Fi\n");
			return;
		}
		whole = whole_runs = true;
		quiet = !(PE_parse_boot_argn("wifi_loud", &back, sizeof(back)) && back);
		redwood_lx_net_early();
		pause_ms = watch_seconds = 0;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			whole_runs = false;
			tell("wifi: no thread: the radio is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("wifi: the radio is being brought up beside the system's start (wifi=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	new_from = last_step;
	PE_parse_boot_argn("wifi_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("wifi_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("wifi_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("wifi_watch", &watch_seconds, sizeof(watch_seconds));
	PE_parse_boot_argn("wifi_dirty", &dirty, sizeof(dirty));

	printf("wifi: steps up to %u; those from %u on are new and wait %u ms %s\n", last_step, new_from, pause_ms,
	    fine ? "before every write and call" : "after each announcement");
	if ((radio_space.wanted = redwood_wifi_option("wifi_smmu")) != 0) {
		smmu_look();
		if (radio_space.wanted == 2 && last_step < 15) {
			uint32_t known_from = new_from;

			new_from = 0;                   /* (this is new: it waits as a new step does) */
			radio_space_try();
			new_from = known_from;
		}
	}
	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("wifi: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("wifi: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;                   /* (what follows has been announced) */
	steps_over(done);
	printf("wifi: over after step %u; %s\n", done, last_write);
	back = 0;
	if (PE_parse_boot_argn("wifi_return", &back, sizeof(back)) && back) {
		printf("wifi: back to the loader (wifi_return)\n");
		loader_return();
	}
	printf("wifi: the system goes on starting\n");
}

/*
 * What interrupts would take, instead of looking. The buttons want the same (the PMIC arbiter's
 * line, see the end of redwood_buttons.c), and the radio will: so the general case first.
 *
 * The general case is one shared peripheral interrupt (an "SPI": the interrupt controller's
 * number for it is 32 more than the device tree's) with a handler. osfmk/arm64/gic_v3_irq.c
 * drives the processors' interfaces, takes the timer's interrupt and the two that processors
 * send each other, and masks and counts anything else that fires. What it lacks:
 *   - a table from interrupt number to handler and argument, and a call with which a driver
 *     fills an entry (number, level or edge, handler, argument); gic_irq_handler() calls the
 *     handler where it now masks, between taking the interrupt (ICC_IAR1_EL1) and ending it
 *     (ICC_EOIR1_EL1);
 *   - for that number, in the distributor (it is mapped; only its control register and the
 *     "disable" registers are written today): group 1 like everything here (GICD_IGROUPR), the
 *     trigger (GICD_ICFGR), a priority (GICD_IPRIORITYR), the processor it goes to
 *     (GICD_IROUTER: the boot processor), and enabled (GICD_ISENABLER); and the reverse when
 *     the driver lets go and before the kernel returns to the loader, which must find the
 *     lines as it left them.
 *   Whether the hypervisor passes this kernel's writes to the distributor for these lines is
 *   not known: none has been made. A handler runs with interrupts off on whatever thread was
 *   interrupted; it should quiet its device and wake a thread, no more.
 *
 * Ours, given that:
 *   - The doorbells have one line for everything rung at the application processor: SPI 229,
 *     level, high (dt: qcom,ipcc@408000). What IPCC is seen to do: it keeps one mark per
 *     doorbell (a client and a signal), not a count -- 46 GLINK messages from the DSP were five
 *     rings heard. A doorbell is recorded only if IPCC has been told that it is listened for
 *     (client << 16 | signal written to IPCC_ENABLE): the boot chain leaves the DSP's GLINK
 *     doorbell so and no other, and the DSP's SMP2P rings left no trace until step 11 enabled
 *     them. IPCC_RECEIVED names one recorded doorbell at a time, the same one until that word
 *     is written to IPCC_CLEAR, then the next, then "nobody". So a handler reads IPCC_RECEIVED
 *     until it says nobody, writes each word it gets to IPCC_CLEAR, and passes it on (this is
 *     doorbells() above, called from a loop today): client 0 signal 0 is the always-on
 *     processor's mailbox (every wait in mailbox_wait()), client 24 signal 2 the Wi-Fi DSP's
 *     SMP2P item (the watch of step 11, the wait in dsp_ask_to_stop()), client 24 signal 0 its
 *     GLINK. Since a mark is not a count, whoever is woken looks until there is nothing left:
 *     the link's thread does. With that, link_wait() is the one function to change.
 *   - The DSP's watchdog has a line of its own (SPI 587), raised when its firmware hangs
 *     without managing to say so in the shared memory.
 *   - The radio itself, later, raises 32 more (SPI 768 to 799, edge), by writing their numbers
 *     to a register of the interrupt controller (dt: "msi_addr" of qcom,wcn6750): its copy
 *     engines and the rings of the data path. Those can be looked at in turn as well, at the
 *     price of a thread that never sleeps while the link is up.
 */
