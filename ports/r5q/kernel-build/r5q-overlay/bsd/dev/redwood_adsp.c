/* Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026, from the donor G986N-iOS (commit 946de0fed8e5, a port of HackiOS): Apple Public Source License 2.0, see LICENSE and MODIFICATIONS.md in this directory. */
/*
 * r5q (Galaxy S10 Lite, SM8150; Test 140): this file in an r5q mode too. There the loader starts the DSP (adsp=2:
 * TrustZone PAS 1 from the apnhlos partition into 0x99300000, 39 MB), so steps 2-5 and the start in 7 are skipped and 7
 * only watches it and answers; the shared memory is at 0x86000000; there is no IPCC: the doorbells are bits of APCS IPC
 * 0x17c0000c (live DT: qcom,smp2p-adsp qcom,ipc <apcs 0 10>, qcom,glink/adsp mboxes <apcs 8>), and the DSP's are not
 * listened for (the queues and the item are looked at); GLINK's queue of ours (479, 16 KB) is made here, as Linux's
 * qcom_glink_smem makes it (the DSP made 478 and 480); the rails, the battery manager and the donor's speaker and
 * microphones are not for this phone.
 *
 * The audio DSP of the POCO X5 Pro (redwood), started, and spoken to: step 1 of sound. Every
 * address below belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The SoC's audio (LPASS: its clocks, its I2S and SoundWire, the codec's macros) does not belong
 * to the application processor. It is the audio DSP's -- a Hexagon of its own inside LPASS,
 * Android's "adsp" -- which stays in reset until somebody loads it, exactly as the Wi-Fi's does
 * (redwood_wifi.c): the phone's own signed image, adsp.mdt and adsp.b02 to adsp.b36 from
 * /vendor/firmware_mnt/image of its Android (20 MB: in the system image under /fw, or with ADSP=1 in
 * the small one, see image_file()), goes into the 64 MB reserved for it, TrustZone is
 * asked to authenticate and start it, and from there everything is conversation through the
 * shared memory: SMP2P for "up", "given up", "stopped"; GLINK for channels; QRTR on the channel
 * "IPCRTR" for the services the DSP offers. (Reading an LPASS clock register from here ends the
 * kernel with an external abort: the DSP owns that block, and talks about it only this way.)
 * What audio needs of it later -- the audio services, the codec, the speaker's path -- is for the
 * steps after this one; this file only brings the DSP up and the link with it.
 *
 * The way is the Wi-Fi DSP's, taken over from redwood_wifi.c with the audio DSP's numbers: its
 * comments say why each thing is done so. What is different is said here:
 *   - processor 2 (not 13) in the shared memory; its SMP2P items 443 (its) and 429 (ours), its
 *     last words in item 423; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 3 (LPASS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 1; its memory is 0x86700000, 64 MB (dt: qcom,lpass@3700000,
 *     reserved-memory adsp@86700000), every byte of which the image takes
 *   - the always-on processor is told of it as "adsp"
 *   - the Wi-Fi DSP may be running at the same time (wifi=1): the mailbox to the always-on
 *     processor and the lock for allocating in the shared memory are the two things both use,
 *     each behind a lock both take (redwood_wifi.c); the doorbells are only counted here, and
 *     only the audio DSP's are cleared from here
 * The code is a copy, not a shared library, for now: step 1 is not to change what the Wi-Fi does.
 *
 * Boot-arg adsp_step=N performs steps 1 to N and stops; as with wifi_step, a step says what it is
 * about to touch before it touches it, and a step that does not find what it needs ends the
 * sequence. adsp=1 is all of them, on a thread beside the system's start.
 *
 *   1  reads   the shared memory: our partition with the DSP, its items, whether it has run
 *              since the phone was started (its SMP2P item is there if so), its last words
 *   2  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the Wi-Fi may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a
 *              DSP that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory, and reads it back
 *   6  writes  our SMP2P item (429) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the DSP's rails, writes "adsp: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the DSP, and watches for its first words
 *              (answering in our item what SMP2P has us answer)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written
 *              down for adsp_listen seconds (default 3)
 *  11  talks   APR, on the channel apr_audio_svc that the DSP opens and we answer as well: whether
 *              the audio services are up, and which there are (see step_apr())
 *
 * Once everything is up with adsp=1, the sound's path takes over (redwood_sound.c, boot-arg sound=N):
 * redwood_adsp_apr() is how it speaks to the audio services (any service, port and token; what
 * nobody waits for goes to its handler of events), and it is stopped before the DSP is.
 *
 * What is started is stopped: when the steps are over (unless adsp_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry
 * "master-kernel"), then TrustZone shuts it down, then the always-on processor hears "adsp: off".
 * A DSP that a kernel before this one left running is found by its item and treated the same way
 * before this one is started; one that was shut down without having said it stopped is not
 * started again unless adsp_again=1 (the Wi-Fi DSP reset the phone, twice, when that was tried).
 *
 * More boot-args: adsp_new=K (steps K on have not been seen to work, and wait adsp_pause=MS after
 * each announcement, default 2200: what the loader needs to note the end of the log), adsp_wait=S
 * (how long step 7 watches for "up", default 10), adsp_keep=1, adsp_again=1, adsp_return=1 (back
 * to the loader when the steps are over).
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh, as Linux makes it: the
 * DSP's two rails, LCX and LMX, at level 384 while it boots, given back when it says it has asked
 * for its own (see rails()).
 *
 * Addresses are from the phone's own device tree (qcom,lpass@3700000, qcom,smp2p-adsp,
 * qcom,glink/adsp, qcom,ipcc@408000, power-controller@c300000, reserved-memory), layouts from
 * Linux (see redwood_wifi.c for the files).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the Wi-Fi's DSP and this one both use (redwood_wifi.c). */
extern lck_mtx_t redwood_aop_mailbox_lock;
extern lck_mtx_t redwood_smem_alloc_lock;

static LCK_GRP_DECLARE(adsp_lock_group, "redwood-adsp");

#define STEPS                   11
#define REDWOOD_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in redwood_wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static bool quiet;

/*
 * (r5q, Test 210) the SLPI's lines ("slpi: ...") kept in a 32 KB ring of their own as well, readable as the sysctl
 * kern.redwood.slpi_log: the kernel's log wraps in about ten seconds under SpringBoard's chatter, and the sensors'
 * bring-up is slower than that (wifi/r5q_sysctl.c reads it over r5qd).
 */
#define SLPI_LOG_SIZE           32768u
static char slpi_log[SLPI_LOG_SIZE];
static uint32_t slpi_log_at;
static bool slpi_log_wrapped;

static void
slpi_log_add(const char *line)
{
	for (const char *c = line; *c != 0; c++) {
		slpi_log[slpi_log_at++] = *c;
		if (slpi_log_at == SLPI_LOG_SIZE) {
			slpi_log_at = 0;
			slpi_log_wrapped = true;
		}
	}
}

static int
slpi_log_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = 0;

	if (slpi_log_wrapped) {
		error = SYSCTL_OUT(req, slpi_log + slpi_log_at, SLPI_LOG_SIZE - slpi_log_at);
	}
	if (error == 0) {
		error = SYSCTL_OUT(req, slpi_log, slpi_log_at);
	}
	return error;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, slpi_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, slpi_log_sysctl, "A",
    "the sensors' DSP's lines of the kernel's log (redwood_adsp.c)");

static void
say_line(const char *line, bool aloud)
{
	if (strncmp(line, "slpi:", 5) == 0 && !leaving) {
		lck_mtx_lock(&say_lock);
		slpi_log_add(line);
		lck_mtx_unlock(&say_lock);
	}
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

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

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
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

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in redwood_wifi.c) ------------------------------------------------------- */

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
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adsp: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (redwood_wifi.c, step 1) --------------------------------------------------- */

static bool r5q;                        /* (Test 140) the Galaxy S10 Lite: see the top */
static unsigned long smem_phys = 0x80900000UL;  /* r5q: 0x86000000 */
#define SMEM_PHYS               smem_phys
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_ADSP               2               /* dt: qcom,remote-pid of qcom,smp2p-adsp and qcom,glink/adsp */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-adsp, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         429
#define ITEM_SMP2P_THEIRS       443
#define ITEM_CRASH_REASON       423

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
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

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

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
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };

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

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
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
	printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("adsp: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (redwood_wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_LPASS              3u              /* dt: mboxes of qcom,smp2p-adsp and qcom,glink/adsp */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };
static struct block apcs = { "apcs ipc", 0x17c00000, 0x1000 };    /* (r5q) */

/* a doorbell to the audio DSP: IPCC's signal, or on r5q the APCS IPC bit (SMP2P 10, GLINK 8) */
static void
ring_dsp(uint32_t signal)
{
	if (r5q) {
		need(&apcs);
		wr(apcs.base + 0x0c, 1u << (signal == IPCC_SMP2P ? 10 : 8));
		return;
	}
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | signal);
}

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The audio DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	if (r5q) {
		return;                 /* (no IPCC: the queues and the item are looked at instead) */
	}
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_LPASS) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("adsp: doorbell: +%llu.%03llu ms: signal %u of the audio DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

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
ring_aop(void)
{
	if (r5q) {
		return;
	}
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as redwood_wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("adsp: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (r5q) {
		return true;            /* (the loader told the AOP) */
	}
	if (!leaving) {
		lck_mtx_lock(&redwood_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("adsp: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&redwood_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The audio DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
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

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("adsp: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("adsp: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	if (r5q) {
		return true;            /* (the DSP asks for its own) */
	}
	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("adsp: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("adsp: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		return false;
	}
	printf("adsp: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the audio DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("adsp: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the audio DSP in the shared memory (0x80900000; r5q 0x86000000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	if (!r5q) {
		need(&ipcc);
		printf("adsp: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	}
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             0x86700000u     /* dt: reserved-memory adsp@86700000, memory-region of qcom,lpass@3700000 */
#define ADSP_MEMORY_SIZE        0x04000000u
#define PAS_ADSP                1               /* dt: qcom,pas-id */


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-audio/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* For the sound (redwood_sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
const uint8_t *
redwood_adsp_fw_file(const char *name, uint64_t *size)
{
	return root_file(name, size);
}

void
redwood_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/* A file of fw/ in the small image (ADSP=1 userland/build.sh puts fw-audio/ there), else of /fw on the root (above). */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = redwood_image_file("fw", name, size, NULL);  /* (redwood_image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			return data;
		}
	}
	printf("adsp: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-audio/ and run SYSTEM=1 userland/build.sh (or ADSP=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;
	if (r5q) {
		printf("adsp: %u: (r5q) the image: the loader's work (adsp=2)\n", step);
		return true;
	}

	announce("find the audio DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("adsp.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("adsp: %u: fw/adsp.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < ADSP_MEMORY || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)ADSP_MEMORY + ADSP_MEMORY_SIZE) {
			printf("adsp: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - ADSP_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "adsp.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("adsp: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != ADSP_MEMORY || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("adsp: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - ADSP_MEMORY;
	printf("adsp: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, ADSP_MEMORY_SIZE, ADSP_MEMORY);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;
	if (r5q) {
		printf("adsp: %u: (r5q) the always-on processor: the loader's work (adsp=2)\n", step);
		return true;
	}

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&redwood_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&redwood_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
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
			printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adsp: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
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
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		ring_dsp(IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	ring_dsp(IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	ring_dsp(IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	if (r5q) {
		printf("adsp: %u: (r5q) not shut down through TrustZone from here: the reset does it\n", step);
		dsp_started = false;
		return true;
	}
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	need(&ipcc);
	if (!r5q) {
		wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	}
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;
	if (r5q) {
		printf("adsp: %u: (r5q) TrustZone and the header: the loader's work (adsp=2)\n", step);
		return true;
	}

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("adsp_again")) {
			printf("adsp: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (adsp_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the audio DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TRUSTZONE ACCEPTS THE AUDIO DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, ADSP_MEMORY);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

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
	if (r5q) {
		printf("adsp: %u: (r5q) the image in its memory: the loader's work (adsp=2)\n", step);
		return true;
	}

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the audio DSP's memory (0x86700000, 64 MB: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(ADSP_MEMORY, ADSP_MEMORY_SIZE, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, ADSP_MEMORY);
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
			printf("adsp: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
			return false;
		}
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
	printf("adsp: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (redwood_wifi.c: our_item_make()). */
static bool
item_make(uint32_t partition, uint32_t item, uint32_t data_size)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (data_size + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&redwood_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&redwood_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
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
		printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - data_size);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&redwood_smem_alloc_lock);
	return made;
}

static bool
our_item_make(uint32_t partition)
{
	return item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p));
}

/*
 * (r5q, Test 140) GLINK's queue of ours, item 479 of 16 KB, made as Linux's qcom_glink_smem_register() makes it
 * (qcom_smem_alloc(): the DSP made the descriptor, 478, and its own queue, 480, and waits for ours).
 */
static bool
glink_queue_make(uint32_t partition)
{
	uint32_t size = 0, at = smem_item(479, &size);

	if (at != 0) {
		printf("adsp: %u: GLINK's queue of ours (479) is there: %u bytes at %#lx\n", step, size, SMEM_PHYS + at);
		return true;
	}
	if (!item_make(partition, 479, 16384)) {
		return false;
	}
	at = smem_item(479, &size);
	printf("adsp: %u: GLINK's queue of ours (479) %s: %u bytes at %#lx\n", step, at ? "MADE" : "NOT FOUND AFTER MAKING IT",
	    size, SMEM_PHYS + at);
	return at != 0;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	if (r5q && !glink_queue_make(partition)) {
		return false;
	}
	ring_dsp(IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written && !r5q) {
		printf("adsp: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	if (r5q) {
		announce("watch the audio DSP that the loader started (adsp=2), from its SMP2P item as it is now, and answer it");
		bzero(&last, sizeof(last));
		have = false;
		bzero(&doorbell, sizeof(doorbell));
		doorbell.since = started = now_us();
		dsp_started = true;
		goto watch;
	}
	announce("listen for the audio DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_LPASS; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("adsp: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
watch:
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
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("adsp: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("adsp: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (redwood_wifi.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,adsp_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
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
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_ADSP_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

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

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

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
	ring_dsp(IPCC_GLINK);
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

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
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
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
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
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("adsp: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			ring_dsp(IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (redwood_wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;

/*
 * (r5q, Test 182) The microphones are on the WCD9340 codec, on SLIMbus (live DT slim@171c0000 "qcom,slim-ngd",
 * cell-index 1, tavil_codec). The bus's manager is the DSP's; our part, the NGD, is powered by asking the DSP's
 * QMI service 0x301 (Linux drivers/slimbus/qcom-ngd-ctrl.c): select instance (0x20: instance 0, satellite 1), power
 * (0x21: active 2). Answers come to our port below. Boot-arg mic=1: that, and the controller's registers read.
 */
#define R5Q_SLIM_PORT           0x4130
static volatile uint32_t r5q_slim_result;       /* result | error << 16 of the last answer */
static volatile uint16_t r5q_slim_msg, r5q_slim_seen;
static void r5q_slim_answer(const uint8_t *m, uint32_t size);
static void r5q_mic_start(void);
static void r5q_mic_thread(void *parameter, wait_result_t result);

/* ---- (r5q, Test 201) the sensors' DSP (SLPI): its GLINK edge and QRTR ----------------------------------------- */

/*
 * The loader starts the SLPI (boot-arg slpi=2: PAS 12, live DT qcom,ssc@5c00000). Its edge is the audio DSP's over
 * again with host 3 (live DT glink "dsps": qcom,remote-pid 3, mboxes <apcs 0x18>, interrupts SPI 170): GLINK's
 * descriptor (478) and queues (479 ours, 480 its) in the SMEM partition of hosts 0 and 3, our doorbell APCS IPC
 * 0x17c0000c bit 24 (its SMP2P's is bit 26), its doorbell to us polled every millisecond. One channel is answered,
 * IPCRTR (QRTR; intents <0x800 5 0x2000 3 0x4400 2> as the live DT's qcom,slpi_qrtr), and QRTR spoken only so far as
 * to hear which services the SLPI offers -- the sensors' (Qualcomm SEE, QMI service 400) among them, if it is up.
 * Separate from the audio DSP's link (its own state, lock and thread): nothing of the sound's depends on it.
 */
#define SLPI_HOST               3
#define SLPI_DOORBELL           (1u << 24)

struct sl_packet {
	struct sl_packet *next;
	uint32_t length;
	uint8_t data[];
};

/* (Test 205) two channels answered: IPCRTR (QRTR) and fastrpcglink-apps-dsp (FastRPC, below) */
enum { SL_QRTR, SL_FRPC, SL_CHANNELS };
struct sl_channel {
	const char *name;
	uint32_t ours, theirs;
	uint32_t intents[3][2];
	bool asked, acked, open;
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];
	uint32_t our_intent_count;
	struct sl_packet *first, *last;
};

static struct {
	uint32_t partition, descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken, agreed;
	uint32_t features, said, heard, trace;
	struct sl_channel ch[SL_CHANNELS];
	uint32_t qrtr_node, qrtr_hellos, qrtr_in, qrtr_out;
	struct {
		uint32_t service, instance, node, port;
		bool there;
	} servers[48];
	char others[6][33];
} sl = {
	.ch = {
		[SL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[SL_FRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 2, .intents = { { 0x80, 16 }, { 0x400, 4 }, { 0, 0 } } },
	},
};
static LCK_MTX_DECLARE(sl_lock, &adsp_lock_group);
static void r5q_slpi_start(void);
static void sl_smp2p_show(void);

static uint32_t
sl_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[128];
	uint32_t cacheline = 0, partition_size = 0, partition = smem_partition(SMEM_US, SLPI_HOST, &cacheline, &partition_size);
	unsigned count = smem_items(partition, cacheline, found, 128);

	sl.partition = partition;
	for (unsigned i = 0; i < count && i < 128; i++) {
		if (found[i].item == item) {
			*size = found[i].size;
			return found[i].offset;
		}
	}
	return 0;
}

static void
sl_ring(void)
{
	need(&apcs);
	wr(apcs.base + 0x0c, SLPI_DOORBELL);
}

static uint32_t
sl_waiting(void)
{
	uint32_t head = smem_word(sl.descriptor + GLINK_THEIR_HEAD), tail = smem_word(sl.descriptor + GLINK_THEIR_TAIL);

	if (head >= sl.theirs_size || tail >= sl.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : sl.theirs_size - tail + head;
}

static void
sl_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(sl.descriptor + GLINK_THEIR_TAIL) + offset) % sl.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[sl.theirs + (at + i) % sl.theirs_size];
	}
}

static void
sl_advance(uint32_t count)
{
	smem_poke(sl.descriptor + GLINK_THEIR_TAIL, (smem_word(sl.descriptor + GLINK_THEIR_TAIL) + count) % sl.theirs_size);
}

static uint32_t
sl_room(void)
{
	uint32_t head = smem_word(sl.descriptor + GLINK_OUR_HEAD), tail = smem_word(sl.descriptor + GLINK_OUR_TAIL), room;

	if (head >= sl.ours_size || tail >= sl.ours_size) {
		return 0;
	}
	room = tail <= head ? sl.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
sl_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + sl.ours;
	uint32_t head = smem_word(sl.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!sl.up || sl_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % sl.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(sl.descriptor + GLINK_OUR_HEAD, (head + padded) % sl.ours_size);
	settle();
	sl_ring();
	sl.said++;
	return true;
}

static void
sl_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = sl_put(&message, sizeof(message), more, more_length);

	if (sl.trace > 0 || !sent) {
		sl.trace -= sl.trace > 0;
		tell("slpi: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM, not sent");
	}
}

static struct sl_channel *
sl_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < SL_CHANNELS; c++) {
		if (sl.ch[c].theirs != 0 && sl.ch[c].theirs == theirs) {
			return &sl.ch[c];
		}
	}
	return NULL;
}

static bool
sl_offer(struct sl_channel *ch, uint32_t size)
{
	uint32_t words[2] = { size, ch->our_intent_count + 1 }, i = ch->our_intent_count;

	if (i >= sizeof(ch->our_intents) / sizeof(ch->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (ch->our_intents[i].data == NULL || ch->our_intents[i].size != size) {
		ch->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (ch->our_intents[i].data == NULL) {
		return false;
	}
	ch->our_intents[i].size = size;
	ch->our_intents[i].fill = 0;
	ch->our_intent_count++;
	sl_say(GLINK_INTENT, ch->ours, 1, words, sizeof(words));
	return true;
}

/* everything the SLPI has queued, acted on (as glink_pump()); with sl_lock held */
static void
sl_pump(void)
{
	struct glink_message message;
	struct sl_channel *ch;
	uint32_t words[4];
	char name[33];

	while (sl.up && !sl.broken) {
		uint32_t waiting = sl_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		sl_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			sl_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > sl.theirs_size - 8) {
			tell("slpi: glink: A MESSAGE OF %u BYTES (%s): not believed, nothing more is read\n", length, glink_name(message.command));
			sl.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		sl.heard++;
		if (sl.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE && message.command != GLINK_INTENT &&
		    message.command != GLINK_DONE_REUSE) {
			sl.trace--;
			tell("slpi: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				sl.features &= message.param2;
			}
			sl_advance(length);
			sl_say(GLINK_VERSION_ACK, GLINK_VERSION_1, sl.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			sl_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == sl.features) {
				sl.agreed = true;
			} else if (message.param1 != 0) {
				sl.features &= message.param2;
				sl_say(GLINK_VERSION, GLINK_VERSION_1, sl.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			sl_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			sl_advance(length);
			ch = NULL;
			for (unsigned c = 0; c < SL_CHANNELS; c++) {
				if (strcmp(name, sl.ch[c].name) == 0) {
					ch = &sl.ch[c];
				}
			}
			if (ch == NULL) {
				tell("slpi: glink: the SLPI opens \"%s\" (its %u): not answered\n", name, message.param1);
				for (unsigned i = 0; i < 6; i++) {
					if (sl.others[i][0] == 0) {
						strlcpy(sl.others[i], name, sizeof(sl.others[i]));
						break;
					}
				}
				break;
			}
			ch->theirs = message.param1;
			sl_say(GLINK_OPEN_ACK, ch->theirs, 0, NULL, 0);
			if (!ch->asked) {
				uint32_t size = (uint32_t)strlen(ch->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, ch->name, sizeof(name));
				sl_say(GLINK_OPEN, ch->ours, size, name, (size + 7) & ~7u);
				ch->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			sl_advance(length);
			for (unsigned c = 0; c < SL_CHANNELS; c++) {
				if (message.param1 == sl.ch[c].ours) {
					sl.ch[c].acked = true;
				}
			}
			break;
		case GLINK_INTENT:
			ch = sl_theirs(message.param1);
			for (uint32_t i = 0; ch != NULL && i < message.param2; i++) {
				sl_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(ch->their_intents) / sizeof(ch->their_intents[0]); n++) {
					if (!ch->their_intents[n].there || ch->their_intents[n].id == words[1]) {
						ch->their_intents[n].there = true;
						ch->their_intents[n].in_use = false;
						ch->their_intents[n].size = words[0];
						ch->their_intents[n].id = words[1];
						break;
					}
				}
			}
			sl_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			ch = sl_theirs(message.param1);
			for (unsigned n = 0; ch != NULL && n < sizeof(ch->their_intents) / sizeof(ch->their_intents[0]); n++) {
				if (ch->their_intents[n].there && ch->their_intents[n].id == message.param2) {
					ch->their_intents[n].in_use = false;
					ch->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			sl_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			sl_advance(length);
			if ((ch = sl_theirs(message.param1)) != NULL) {
				bool granted = sl_offer(ch, message.param2);

				sl_say(GLINK_INTENT_REQUEST_ACK, ch->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			sl_advance(length);
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			ch = sl_theirs(message.param1);
			if (ch == NULL || intent >= ch->our_intent_count || words[0] > ch->our_intents[intent].size - ch->our_intents[intent].fill) {
				tell("slpi: glink: < data of %u bytes on %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				sl_advance(length);
				break;
			}
			sl_peek(ch->our_intents[intent].data + ch->our_intents[intent].fill, 16, words[0]);
			ch->our_intents[intent].fill += words[0];
			sl_advance(length);
			if (words[1] == 0) {
				struct sl_packet *packet = kalloc_data(sizeof(*packet) + ch->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = ch->our_intents[intent].fill;
					memcpy(packet->data, ch->our_intents[intent].data, packet->length);
					if (ch->last != NULL) {
						ch->last->next = packet;
					} else {
						ch->first = packet;
					}
					ch->last = packet;
				}
				ch->our_intents[intent].fill = 0;
				sl_say(GLINK_DONE_REUSE, ch->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			sl_advance(length);
			sl_ring();
			break;
		case GLINK_CLOSE:
			sl_advance(length);
			sl_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((ch = sl_theirs(message.param1)) != NULL) {
				tell("slpi: glink: THE SLPI CLOSES \"%s\"\n", ch->name);
				ch->open = false;
				ch->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			sl_advance(length);
			break;
		default:
			tell("slpi: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command, message.param1,
			    message.param2);
			sl.broken = true;
			return;
		}
		for (unsigned c = 0; c < SL_CHANNELS; c++) {
			ch = &sl.ch[c];
			if (ch->theirs != 0 && ch->acked && !ch->open) {
				ch->open = true;
				tell("slpi: glink: \"%s\" IS OPEN both ways (ours %u, the SLPI's %u); our intents told\n", ch->name, ch->ours,
				    ch->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < ch->intents[i][1]; n++) {
						sl_offer(ch, ch->intents[i][0]);
					}
				}
			}
		}
	}
}

/* one message to the SLPI on a channel; waits up to two seconds for an intent of the SLPI's that it fits */
static int
sl_send(struct sl_channel *ch, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)ch->ours, 0 }, length, 0 };
	int found = -1;

	lck_mtx_lock(&sl_lock);
	for (unsigned waited = 0;; waited++) {
		sl_pump();
		if (!sl.up || !ch->open || sl.broken) {
			lck_mtx_unlock(&sl_lock);
			return ENOTCONN;
		}
		found = -1;
		for (unsigned n = 0; n < sizeof(ch->their_intents) / sizeof(ch->their_intents[0]); n++) {
			if (ch->their_intents[n].there && !ch->their_intents[n].in_use && ch->their_intents[n].size >= length &&
			    (found < 0 || ch->their_intents[n].size < ch->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && sl_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (found < 0 && waited == 0) {
			sl_say(GLINK_INTENT_REQUEST, ch->ours, length, NULL, 0);
		}
		if (waited >= 2000) {
			lck_mtx_unlock(&sl_lock);
			return ENOBUFS;
		}
		lck_mtx_unlock(&sl_lock);
		sleep_ms(1);
		lck_mtx_lock(&sl_lock);
	}
	ch->their_intents[found].in_use = true;
	header.message.param2 = ch->their_intents[found].id;
	sl_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&sl_lock);
	return 0;
}

static int
sl_qrtr_out(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	int error;

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
	error = sl_send(&sl.ch[SL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	sl.qrtr_out += error == 0;
	return error;
}

/*
 * (r5q, Test 202) The sensors' client service (Qualcomm SEE, QMI service 400 on the SLPI; boot-arg slpi=3): QMI
 * request 0x20 carries a protobuf sns_client_request_msg in TLV 1 (a 16-bit length, then the bytes); events come back
 * as indication 0x21, TLV 2, a protobuf sns_client_event_msg. Encoded and decoded by hand here (the field numbers as
 * Qualcomm's sns_client.proto, sns_std.proto, sns_suid.proto, sns_std_sensor.proto): first the SUID lookup sensor
 * (SUID ab..ab, msg 512, sns_suid_req{data_type}) answers each data type's sensors (msg 768, sns_suid_event), then
 * the accelerometer is asked for a stream (msg 513, sns_std_sensor_config{sample_rate}) and its samples come as msg
 * 1025 (sns_std_sensor_event{repeated float data}). No floating point in the kernel: floats are taken apart by hand.
 */
#define SEE_PORT                0x4131
#define SEE_SUID_LOOKUP         0xababababababababull
enum { SEE_ACCEL, SEE_GYRO, SEE_MAG, SEE_LIGHT, SEE_PROX, SEE_KINDS };
static const char *const see_kind_name[SEE_KINDS] = { "accel", "gyro", "mag", "ambient_light", "proximity" };
static struct {
	uint32_t node, port;
	uint16_t txn;
	volatile uint32_t answers, events, samples;
	volatile bool have_accel;
	uint64_t accel[2];
	int32_t x, y, z;                        /* the last sample, thousandths of m/s^2 */
	uint32_t status, other_ids[4];
	uint64_t suid[SEE_KINDS][2];            /* (Test 223) every kind's sensor */
	bool have[SEE_KINDS];
} see;

/*
 * (Test 223) What the sensors last said, for iOS: the sysctl kern.redwood.motion (read in backboardd by wifi/inject_bbd.c).
 * Android's axes and units (x right, y up the screen, z out of it; the reaction to gravity: +9.8 on z lying face up),
 * in thousandths; a sample's time in mach_absolute_time.
 */
struct redwood_motion {
	uint32_t version, seq;
	uint32_t count[SEE_KINDS];
	int32_t accel[3];                       /* m/s^2 */
	int32_t gyro[3];                        /* rad/s */
	int32_t mag[3];                         /* uT */
	int32_t lux;
	int32_t prox[2];                        /* near 1 / far 0; the raw reading */
	uint64_t time[SEE_KINDS];
};
static struct redwood_motion motion = { .version = 1 };

static int
motion_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct redwood_motion copy = motion;

	return SYSCTL_OUT(req, &copy, sizeof(copy));
}

SYSCTL_PROC(_kern_redwood, OID_AUTO, motion, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, motion_sysctl, "S",
    "the sensors' last samples (redwood_adsp.c, struct redwood_motion)");

static int32_t
float_milli(uint32_t bits)
{
	int32_t exponent = (int32_t)((bits >> 23) & 0xff) - 127;
	int64_t mantissa = (int64_t)((bits & 0x7fffff) | 0x800000), value;

	if (((bits >> 23) & 0xff) == 0) {
		return 0;
	}
	value = mantissa * 1000;
	value = exponent >= 23 ? (exponent - 23 > 20 ? INT32_MAX : value << (exponent - 23)) :
	    (23 - exponent > 62 ? 0 : value >> (23 - exponent));
	value = value > INT32_MAX ? INT32_MAX : value;
	return (bits & 0x80000000u) ? -(int32_t)value : (int32_t)value;
}

static bool
pb_varint(const uint8_t *b, uint32_t length, uint32_t *at, uint64_t *value)
{
	*value = 0;
	for (unsigned shift = 0; shift < 64 && *at < length; shift += 7) {
		uint8_t byte = b[(*at)++];

		*value |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80)) {
			return true;
		}
	}
	return false;
}

/* the next field of a protobuf message: its number, wire type, and value (varint/fixed) or bytes (length-delimited) */
static bool
pb_next(const uint8_t *b, uint32_t length, uint32_t *at, uint32_t *field, uint32_t *wire, uint64_t *value, const uint8_t **bytes,
    uint32_t *count)
{
	uint64_t key;

	if (*at >= length || !pb_varint(b, length, at, &key)) {
		return false;
	}
	*field = (uint32_t)(key >> 3);
	*wire = (uint32_t)(key & 7);
	*value = 0;
	switch (*wire) {
	case 0:
		return pb_varint(b, length, at, value);
	case 1:
		if (*at + 8 > length) {
			return false;
		}
		memcpy(value, b + *at, 8);
		*at += 8;
		return true;
	case 5:
		if (*at + 4 > length) {
			return false;
		}
		memcpy(value, b + *at, 4);
		*at += 4;
		return true;
	case 2:
		if (!pb_varint(b, length, at, value) || *at + *value > length) {
			return false;
		}
		*bytes = b + *at;
		*count = (uint32_t)*value;
		*at += (uint32_t)*value;
		return true;
	default:
		return false;
	}
}

static void
pb_suid(const uint8_t *b, uint32_t length, uint64_t suid[2])
{
	uint32_t at = 0, field, wire, count = 0;
	uint64_t value;
	const uint8_t *bytes = NULL;

	suid[0] = suid[1] = 0;
	while (pb_next(b, length, &at, &field, &wire, &value, &bytes, &count)) {
		if (field == 1 && wire == 1) {
			suid[0] = value;
		} else if (field == 2 && wire == 1) {
			suid[1] = value;
		}
	}
}

static int
see_send(const uint64_t suid[2], uint32_t msg_id, const uint8_t *payload, uint32_t payload_length)
{
	uint8_t request[256], qmi[300];
	uint32_t at = 0, n;

	if (payload_length > 120) {
		return EINVAL;
	}
	request[at++] = 0x0a;                   /* suid */
	request[at++] = 18;
	request[at++] = 0x09;
	memcpy(request + at, &suid[0], 8);
	at += 8;
	request[at++] = 0x11;
	memcpy(request + at, &suid[1], 8);
	at += 8;
	request[at++] = 0x15;                   /* msg_id */
	memcpy(request + at, &msg_id, 4);
	at += 4;
	request[at++] = 0x1a;                   /* susp_config: the apps processor, no wakeup */
	request[at++] = 4;
	request[at++] = 0x08;
	request[at++] = 1;
	request[at++] = 0x10;
	request[at++] = 1;
	request[at++] = 0x22;                   /* request: { payload } */
	request[at++] = (uint8_t)(payload_length + 2);
	request[at++] = 0x12;
	request[at++] = (uint8_t)payload_length;
	memcpy(request + at, payload, payload_length);
	at += payload_length;
	n = at;
	qmi[0] = 0;                             /* a request */
	see.txn++;
	qmi[1] = (uint8_t)see.txn;
	qmi[2] = (uint8_t)(see.txn >> 8);
	qmi[3] = 0x20;
	qmi[4] = 0;
	qmi[5] = (uint8_t)(3 + 2 + n);
	qmi[6] = (uint8_t)((3 + 2 + n) >> 8);
	qmi[7] = 0x01;                          /* TLV 1: the payload, a 16-bit count and the bytes */
	qmi[8] = (uint8_t)(2 + n);
	qmi[9] = (uint8_t)((2 + n) >> 8);
	qmi[10] = (uint8_t)n;
	qmi[11] = (uint8_t)(n >> 8);
	memcpy(qmi + 12, request, n);
	return sl_qrtr_out(QRTR_DATA, SEE_PORT, see.node, see.port, qmi, 12 + n);
}

static int
see_lookup(const char *data_type)
{
	static const uint64_t lookup[2] = { SEE_SUID_LOOKUP, SEE_SUID_LOOKUP };
	uint8_t payload[40];
	uint32_t length = (uint32_t)strlen(data_type), at = 0;

	payload[at++] = 0x0a;
	payload[at++] = (uint8_t)length;
	memcpy(payload + at, data_type, length);
	at += length;
	payload[at++] = 0x18;                   /* default_only */
	payload[at++] = 1;
	return see_send(lookup, 512, payload, at);
}

static void
see_event(const uint64_t from[2], uint32_t msg_id, const uint8_t *p, uint32_t length)
{
	int kind = -1;
	uint32_t at = 0, field, wire, count = 0, n = 0;
	uint64_t value, suid[2];
	const uint8_t *bytes = NULL;
	char type[32] = "";
	int32_t data[6] = { 0 };

	for (int k = 0; k < SEE_KINDS; k++) {
		if (see.have[k] && see.suid[k][0] == from[0] && see.suid[k][1] == from[1]) {
			kind = k;
		}
	}
	if (kind == SEE_PROX && (msg_id == 1024 || msg_id == 769)) {    /* (Test 230: this phone sends 769) */
		/* (Test 223) sns_proximity_event { proximity_event_type (1 near), raw_adc, status } */
		while (pb_next(p, length, &at, &field, &wire, &value, &bytes, &count)) {
			if (field == 1 && wire == 0) {
				motion.prox[0] = (int32_t)value;
			} else if (field == 2 && wire == 0) {
				motion.prox[1] = (int32_t)value;
			}
		}
		motion.count[SEE_PROX]++;
		motion.time[SEE_PROX] = mach_absolute_time();
		motion.seq++;
		if (motion.count[SEE_PROX] <= 5) {
			tell("slpi: see: proximity: %s (raw %d)\n", motion.prox[0] ? "NEAR" : "far", motion.prox[1]);
		}
		return;
	}
	switch (msg_id) {
	case 768:                               /* sns_suid_event */
		while (pb_next(p, length, &at, &field, &wire, &value, &bytes, &count)) {
			if (field == 1 && wire == 2) {
				memcpy(type, bytes, count < sizeof(type) - 1 ? count : sizeof(type) - 1);
			} else if (field == 2 && wire == 2) {
				pb_suid(bytes, count, suid);
				n++;
				tell("slpi: see: \"%s\" sensor %u: SUID %016llx %016llx\n", type, n, suid[0], suid[1]);
				for (int k = 0; k < SEE_KINDS; k++) {
					if (strcmp(type, see_kind_name[k]) == 0 && !see.have[k]) {
						see.suid[k][0] = suid[0];
						see.suid[k][1] = suid[1];
						see.have[k] = true;
					}
				}
				if (strcmp(type, "accel") == 0 && !see.have_accel) {
					see.accel[0] = suid[0];
					see.accel[1] = suid[1];
					see.have_accel = true;
				}
			}
		}
		if (n == 0) {
			tell("slpi: see: \"%s\": NO SENSOR of that kind\n", type);
		}
		break;
	case 1025:                              /* sns_std_sensor_event */
		while (pb_next(p, length, &at, &field, &wire, &value, &bytes, &count)) {
			if (field == 1 && wire == 5 && n < 6) {
				data[n++] = float_milli((uint32_t)value);
			} else if (field == 1 && wire == 2) {
				for (uint32_t i = 0; i + 4 <= count && n < 6; i += 4) {
					uint32_t bits;

					memcpy(&bits, bytes + i, 4);
					data[n++] = float_milli(bits);
				}
			} else if (field == 2 && wire == 0) {
				see.status = (uint32_t)value;
			}
		}
		if (kind < 0 || kind == SEE_ACCEL) {
			see.samples++;
			see.x = data[0];
			see.y = data[1];
			see.z = data[2];
		}
		if (kind >= 0) {
			int32_t *to = kind == SEE_ACCEL ? motion.accel : kind == SEE_GYRO ? motion.gyro : kind == SEE_MAG ? motion.mag : NULL;

			if (to != NULL) {
				to[0] = data[0];
				to[1] = data[1];
				to[2] = data[2];
			} else if (kind == SEE_LIGHT) {
				motion.lux = data[0];
			} else if (kind == SEE_PROX) {
				motion.prox[0] = data[0] != 0;
				motion.prox[1] = data[1];
			}
			motion.count[kind]++;
			motion.time[kind] = mach_absolute_time();
			motion.seq++;
			if (motion.count[kind] <= 3 || motion.count[kind] % 2000 == 0) {
				tell("slpi: see: %s sample %u: %d %d %d (x1000), status %u\n", see_kind_name[kind], motion.count[kind], data[0], data[1],
				    data[2], see.status);
			}
		}
		break;
	default:
		for (unsigned i = 0; i < 4; i++) {
			if (see.other_ids[i] == msg_id) {
				break;
			}
			if (see.other_ids[i] == 0) {
				uint8_t b[24] = { 0 };

				see.other_ids[i] = msg_id;
				memcpy(b, p, length < sizeof(b) ? length : sizeof(b));
				/* (Test 221) its bytes: 130 is sns_std_error_event { error } (2 not supported, 3 invalid type, 5 invalid value) */
				tell("slpi: see: an event %u (%u bytes): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
				    "%02x %02x\n", msg_id, length, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
				    b[13], b[14], b[15]);
				break;
			}
		}
		break;
	}
}

/* a QMI message from the sensors' service to our port: an answer, or an indication with events */
static void
see_input(const uint8_t *m, uint32_t size)
{
	uint32_t at = 7;
	uint16_t msg;

	if (size < 7) {
		return;
	}
	msg = (uint16_t)(m[3] | m[4] << 8);
	if (m[0] == 2) {                        /* a response */
		see.answers++;
		while (at + 3 <= size) {
			uint16_t len = (uint16_t)(m[at + 1] | m[at + 2] << 8);

			if (m[at] == 0x02 && len >= 4 && (m[at + 3] | m[at + 4] << 8) != 0) {
				tell("slpi: see: request %#x REFUSED: error %#x\n", msg, m[at + 5] | m[at + 6] << 8);
			}
			at += 3u + len;
		}
		return;
	}
	if (m[0] != 4 || (msg != 0x21 && msg != 0x22)) {
		static unsigned odd;

		if (odd++ < 8) {
			tell("slpi: see: a QMI message type %u, id %#x, %u bytes: not taken\n", m[0], msg, size);
		}
		return;
	}
	if (msg == 0x22) {
		static unsigned jumbo;

		if (jumbo++ < 4) {
			tell("slpi: see: a jumbo report (0x22), %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", size,
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16]);
		}
	}
	see.events++;
	while (at + 3 <= size) {
		uint16_t len = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (m[at] == 0x02 && len >= 2 && at + 3 + len <= size) {
			const uint8_t *p = m + at + 5;
			uint32_t plen = (uint32_t)(m[at + 3] | m[at + 4] << 8), pat = 0, field, wire, count = 0;
			uint64_t value;
			const uint8_t *bytes = NULL;

			uint64_t from[2] = { 0, 0 };

			plen = plen > len - 2u ? len - 2u : plen;
			while (pb_next(p, plen, &pat, &field, &wire, &value, &bytes, &count)) {
				if (field == 1 && wire == 2) {  /* (Test 223) whose events these are */
					pb_suid(bytes, count, from);
				}
				if (field == 2 && wire == 2) {  /* an event: { msg_id, timestamp, payload } */
					uint32_t eat = 0, ef, ew, ecount = 0, id = 0, pcount = 0;
					uint64_t ev;
					const uint8_t *eb = NULL, *payload = NULL;

					while (pb_next(bytes, count, &eat, &ef, &ew, &ev, &eb, &ecount)) {
						if (ef == 1 && ew == 5) {
							id = (uint32_t)ev;
						} else if (ef == 3 && ew == 2) {
							payload = eb;
							pcount = ecount;
						}
					}
					if (payload != NULL) {
						see_event(from, id, payload, pcount);
					}
				}
			}
		}
		at += 3u + len;
	}
}

static void
see_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const kinds[] = { "accel", "gyro", "mag", "ambient_light", "proximity", "pressure", "sig_motion",
		                             "registry", "resampler", "remote_proc_state", "timer", "interrupt", "async_com_port",
		                             "device_orient", "data_acquisition_engine" };
	static const uint8_t rate[] = { 0x0d, 0x00, 0x00, 0x20, 0x41 };        /* sns_std_sensor_config { sample_rate = 10.0 } */

	tell("slpi: see: the sensors' service at node %u port %u: asked for its sensors\n", see.node, see.port);
	for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		int error = see_lookup(kinds[i]);

		if (error != 0) {
			tell("slpi: see: lookup of \"%s\" NOT SENT (%d)\n", kinds[i], error);
		}
		sleep_ms(300);
	}
	for (unsigned ms = 0; ms < 3000 && !see.have_accel; ms += 10) {
		sleep_ms(10);
	}
	/* (Test 216) the physical sensors come up only once the registry has been read through FastRPC: asked again */
	for (unsigned round = 0; round < 9 && !see.have_accel; round++) {
		sleep_ms(10000);
		see_lookup("accel");
		for (unsigned ms = 0; ms < 2000 && !see.have_accel; ms += 10) {
			sleep_ms(10);
		}
		if (see.have_accel) {
			see_lookup("gyro");
			see_lookup("mag");
			see_lookup("ambient_light");
			see_lookup("proximity");
			sleep_ms(1000);
		}
	}
	tell("slpi: see: %u answers, %u indications so far; accelerometer %s\n", see.answers, see.events,
	    see.have_accel ? "FOUND" : "NOT FOUND (the sensor registry may be missing: FastRPC)");
	if (!see.have_accel) {
		return;
	}
	(void)rate;
	{
		/* (Test 223) the streams iOS uses: the accelerometer at 25 a second, the compass at 10, light and proximity on change */
		static const uint8_t rate25[] = { 0x0d, 0x00, 0x00, 0xc8, 0x41 }, rate10[] = { 0x0d, 0x00, 0x00, 0x20, 0x41 };

		for (unsigned round = 0; round < 3 && !(see.have[SEE_MAG] && see.have[SEE_LIGHT] && see.have[SEE_PROX]); round++) {
			see_lookup("gyro");
			see_lookup("mag");
			see_lookup("ambient_light");
			see_lookup("proximity");
			sleep_ms(2000);
		}
		tell("slpi: see: accelerometer at 25/s: %s\n", see_send(see.suid[SEE_ACCEL], 513, rate25, sizeof(rate25)) == 0 ? "asked" :
		    "NOT SENT");
		if (see.have[SEE_GYRO]) {               /* (Test 230) for CoreMotion's gyro and device motion */
			tell("slpi: see: gyroscope at 25/s: %s\n", see_send(see.suid[SEE_GYRO], 513, rate25, sizeof(rate25)) == 0 ? "asked" :
			    "NOT SENT");
		}
		if (see.have[SEE_MAG]) {
			tell("slpi: see: compass at 10/s: %s\n", see_send(see.suid[SEE_MAG], 513, rate10, sizeof(rate10)) == 0 ? "asked" :
			    "NOT SENT");
		}
		if (see.have[SEE_LIGHT]) {
			tell("slpi: see: ambient light on change: %s\n", see_send(see.suid[SEE_LIGHT], 514, NULL, 0) == 0 ? "asked" :
			    "NOT SENT");
		}
		if (see.have[SEE_PROX]) {
			tell("slpi: see: proximity on change: %s\n", see_send(see.suid[SEE_PROX], 514, NULL, 0) == 0 ? "asked" : "NOT SENT");
		}
	}
	for (unsigned minute = 0;; minute++) {
		sleep_ms(60000);
		if (minute < 5 || minute % 30 == 0) {
			tell("slpi: see: minute %u: accel %u (%d %d %d), mag %u, light %u (%d lux/1000), proximity %u (%d)\n", minute + 1,
			    motion.count[SEE_ACCEL], motion.accel[0], motion.accel[1], motion.accel[2], motion.count[SEE_MAG],
			    motion.count[SEE_LIGHT], motion.lux, motion.count[SEE_PROX], motion.prox[0]);
		}
	}
}

static void
sl_qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	struct qrtr_control control = { 0 };
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
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
		tell("slpi: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		return;
	}
	sl.qrtr_in++;
	sl.qrtr_node = sl.qrtr_node ? sl.qrtr_node : header.from_node;
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		sl_qrtr_out(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == SEE_PORT && header.type == QRTR_DATA) {
		see_input(data + at, header.size);
		return;
	}
	if (!(header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP)) {
		tell("slpi: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody listens there yet\n", header.size,
		    header.from_node, header.from_port, header.to_port);
		return;
	}
	memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
	switch (control.command) {
	case QRTR_HELLO:
		if (sl.qrtr_hellos++ == 0) {
			struct qrtr_control hello = { .command = QRTR_HELLO };
			int error = sl_qrtr_out(QRTR_HELLO, QRTR_CONTROL, header.from_node, QRTR_CONTROL, &hello, sizeof(hello));

			tell("slpi: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header.from_node, error == 0 ? "SENT" : "NOT SENT");
		}
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(sl.servers) / sizeof(sl.servers[0]); i++) {
			if (!sl.servers[i].there || (sl.servers[i].node == control.words[2] && sl.servers[i].port == control.words[3])) {
				sl.servers[i].there = true;
				sl.servers[i].service = control.words[0];
				sl.servers[i].instance = control.words[1];
				sl.servers[i].node = control.words[2];
				sl.servers[i].port = control.words[3];
				break;
			}
		}
		tell("slpi: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u%s\n", control.words[0], control.words[0],
		    control.words[1] & 0xff, control.words[1] >> 8, control.words[2], control.words[3],
		    control.words[0] == 400 ? ": THE SENSORS' CLIENT SERVICE (SEE)" : "");
		break;
	case QRTR_DEL_SERVER:
		tell("slpi: qrtr: service %u at node %u port %u has gone\n", control.words[0], control.words[2], control.words[3]);
		break;
	case QRTR_NEW_LOOKUP:
		tell("slpi: qrtr: the SLPI looks for service %u (%#x), instance %u: nobody here offers it\n", control.words[0],
		    control.words[0], control.words[1]);
		break;
	case QRTR_BYE:
		tell("slpi: qrtr: BYE from node %u\n", header.from_node);
		break;
	default:
		break;
	}
}

/* (Test 202) slpi=3: the sensors' client (see_thread) in a thread of its own, once service 400 is known */
static void
see_server(uint32_t node, uint32_t port)
{
	uint32_t slpi = 0;
	thread_t thread;

	PE_parse_boot_argn("slpi", &slpi, sizeof(slpi));
	if (slpi < 3) {
		return;
	}
	see.node = node;
	see.port = port;
	if (kernel_thread_start(see_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * (r5q, Test 205) FastRPC to the SLPI, on its channel "fastrpcglink-apps-dsp" (Linux drivers/misc/fastrpc.c; the
 * reverse tunnel as linux-msm hexagonrpcd). The sensors' registry on the SLPI reads its files from the AP through
 * FastRPC: we attach to the sensors' protection domain (INIT handle 1, method 0, pd 2: INIT_ATTACH_SNS), take up the
 * listener (handle 3: init2, then next2 again and again), and the SLPI's calls come back to us as next2's answers:
 * handle 0 is our remotectl (it opens our interfaces by name: apps_std 1, apps_mem 2), handle 1 apps_std (files).
 * A call's arguments are in one buffer of ours that the SLPI reads and writes through the apps SMMU, stream 0x5a1
 * (live DT qcom,msm_fastrpc_compute_cb13, label sdsprpc-smd; matched to bypass here, as the SLIMbus BAM's): below 4 GB
 * (the loader's lowdma_pa), its address given with the session (0x5a1 & 0xf = 1) above bit 32. Stage 1: the SLPI's
 * calls logged; remotectl answered; everything else "unsupported", to see what it asks for.
 */
/* (Test 208) 0: on the SLPI the session number rides in address bits 32-33 and picks the stream (0x5a0 | it) -- the
 * S10 Lite's adsprpc.c "SMMU interconnect HW bug" workaround; with the streams matched to a translation-off bank
 * those bits would reach the bus as part of the address (Test 207: 0x1_dbcaa000, the wrong memory, "bad parameter").
 * So plain physical addresses (stream 0x5a0), all four streams matched. */
#define FRPC_SESSION            0ull
#define FRPC_PD_SENSORS         2u
#define FRPC_CLIENT             0x2a5
#define FRPC_BUF_SIZE           0x4000u
#define FRPC_SCALARS(m, in, out) ((uint32_t)(((m) & 0x1f) << 24 | ((in) & 0xff) << 16 | ((out) & 0xff) << 8))
#define FRPC_EUNSUPPORTED       20
#define FRPC_EBADPARM           14

struct frpc_msg {
	int32_t client_id, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
};

static struct {
	volatile uint8_t *buf;
	uint64_t pa;
	uint32_t seq, calls;
	volatile bool answered;
	volatile uint64_t answer_ctx;
	volatile int32_t answer;
	uint32_t pd;
	bool early;                             /* (Test 212) slpi=5: the SMMU and buffer readied before the SLPI was released */
} fr;

static volatile uint32_t *r5q_dma_area(uint64_t *pa, size_t size);

struct frpc_arg {
	const void *in;                         /* an input's bytes */
	void *out;                              /* where an output goes */
	uint32_t length;
};

static void
frpc_input(const uint8_t *d, uint32_t length)
{
	uint64_t ctx;
	int32_t retval;

	if (length < 12) {
		return;
	}
	memcpy(&ctx, d, 8);
	memcpy(&retval, d + 8, 4);
	fr.answer_ctx = ctx;
	fr.answer = retval;
	__asm__ volatile ("dmb sy" ::: "memory");
	fr.answered = true;
}

static bool
frpc_smmu(void)
{
	/*
	 * (Test 207) as r5q_slim_smmu() and the Wi-Fi's r5q_smmu_look(), which work: the hypervisor refuses S2CR "bypass", so a
	 * new stream match gets a copy of the S2CR of a valid match that translates through a boot bank with translation off
	 * (SCTLR.M 0, CBAR 0x1f000). Test 206 hung somewhere in the first version (an S2CR copied from SMR0 blindly): every
	 * step said first.
	 */
	static const uint32_t sids[3] = { 0x5a1, 0x5a2, 0x5a3 };
	vm_offset_t s;
	uint32_t idr0, idr1, n, numcb, page, numpage, done = 0;

	tell("slpi: frpc: smmu: mapping 0x15000000\n");
	s = ml_io_map_unmappable(0x15000000UL, 0x100000, VM_WIMG_IO);
	if (s == 0) {
		return false;
	}
	idr0 = *(volatile uint32_t *)(s + 0x20);
	idr1 = *(volatile uint32_t *)(s + 0x24);
	n = idr0 & 0xff;
	numcb = idr1 & 0xff;
	page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	numpage = 1u << (((idr1 >> 28) & 7) + 1);
	tell("slpi: frpc: smmu: idr0 %#x idr1 %#x: %u matches, %u banks\n", idr0, idr1, n, numcb);
	for (unsigned k = 0; k < 3; k++) {
		uint32_t sid = sids[k], free_smr = ~0u, have = ~0u, share = ~0u;

		for (uint32_t i = 0; i < n && i < 128; i++) {
			uint32_t smr = *(volatile uint32_t *)(s + 0x800 + 4 * i), s2cr = *(volatile uint32_t *)(s + 0xc00 + 4 * i);
			uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff, cb = s2cr & 0xff;

			if (!(smr & 0x80000000u)) {
				/*
				 * (Test 231) the HIGHEST free one: the Wi-Fi's start (redwood_wifi.c r5q_smmu_look) takes the lowest, and
				 * since the SLPI is released from here early (slpi=5) the two can run at once and take the same SMR --
				 * Test 230's Wi-Fi then sent nothing (its stream match overwritten), and earlier boots were slow to connect.
				 */
				free_smr = i;
				continue;
			}
			if ((sid & ~m) == (id & ~m)) {
				have = i;
			}
			if (share == ~0u && ((s2cr >> 16) & 3) == 0 && cb < numcb &&
			    !(*(volatile uint32_t *)(s + (vm_offset_t)numpage * page + (vm_offset_t)cb * page) & 1) &&
			    *(volatile uint32_t *)(s + page + 4 * cb) == 0x1f000u) {
				share = i;
			}
		}
		tell("slpi: frpc: smmu: SID %#x: matched by %d, free SMR %d, bank to share via SMR %d\n", sid, (int)have, (int)free_smr,
		    (int)share);
		if (have != ~0u) {
			done++;
			continue;
		}
		if (free_smr == ~0u || share == ~0u) {
			continue;
		}
		*(volatile uint32_t *)(s + 0xc00 + 4 * free_smr) = *(volatile uint32_t *)(s + 0xc00 + 4 * share);
		*(volatile uint32_t *)(s + 0x800 + 4 * free_smr) = 0x80000000u | sid;
		__asm__ volatile ("dsb sy" ::: "memory");
		tell("slpi: frpc: smmu: SID %#x: SMR%u %#x, S2CR %#x (as SMR%u's)\n", sid, free_smr,
		    *(volatile uint32_t *)(s + 0x800 + 4 * free_smr), *(volatile uint32_t *)(s + 0xc00 + 4 * free_smr), share);
		done++;
	}
	return done == 3;
}

/*
 * (Test 211) The buffer: one 16 KB page of RAM whose physical address lies between 4 and 8 GB, so that its bits 32-33 are
 * 01 -- the session (0x5a1 & 3) the SLPI's interconnect takes the stream from -- and the matched stream's translation-off
 * bank passes it through unchanged (Tests 208-210 used stream 0x5a0 below 4 GB instead; the phone reset once). Used
 * through an uncached mapping; the cached one cleaned first and never touched again.
 */
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);

static volatile uint8_t *
frpc_buffer(uint64_t *pa)
{
	for (unsigned tries = 0; tries < 256; tries++) {
		void *page = kalloc_data(FRPC_BUF_SIZE, Z_WAITOK | Z_ZERO);
		uint64_t phys;

		if (page == NULL) {
			return NULL;
		}
		phys = kvtophys((vm_offset_t)page);
		if (((vm_offset_t)page & (FRPC_BUF_SIZE - 1)) == 0 && phys >> 32 == 1) {
			volatile uint8_t *uncached;

			flush_dcache64((addr64_t)(uintptr_t)page, FRPC_BUF_SIZE, FALSE);
			uncached = (volatile uint8_t *)ml_io_map_unmappable(phys, FRPC_BUF_SIZE, VM_WIMG_WCOMB);
			tell("slpi: frpc: buffer at %#llx (try %u)\n", phys, tries);
			*pa = phys;
			return uncached;
		}
		/* not in range: kept (never freed), so the next try gets another page */
	}
	tell("slpi: frpc: no 16 KB page of RAM between 4 and 8 GB found\n");
	return NULL;
}

static void
frpc_put64(uint32_t at, uint64_t value)
{
	for (unsigned i = 0; i < 8; i++) {
		fr.buf[at + i] = (uint8_t)(value >> (8 * i));
	}
}

static void
frpc_put32(uint32_t at, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		fr.buf[at + i] = (uint8_t)(value >> (8 * i));
	}
}

static uint64_t
frpc_get64(uint32_t at)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < 8; i++) {
		value |= (uint64_t)fr.buf[at + i] << (8 * i);
	}
	return value;
}

/* one call to the SLPI (fastrpc_internal_invoke): arguments laid out as fastrpc_get_args(), the answer waited for */
static int
frpc_invoke(bool kernel, uint32_t handle, uint32_t sc, struct frpc_arg *args, uint32_t timeout_ms, int32_t *retval)
{
	uint32_t nin = (sc >> 16) & 0xff, nout = (sc >> 8) & 0xff, n = nin + nout;
	uint32_t metalen = (16 + 8 + 16) * n + 8 * 16 + 4 * 64 + 4, at = (metalen + 127) & ~127u, where[16];
	struct frpc_msg msg;
	uint64_t ctx;
	int error;

	if (n > 16 || fr.buf == NULL) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < at; i++) {
		fr.buf[i] = 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t length = args[i].length;
		uint64_t pa;

		at = (at + 127) & ~127u;
		if (at + length > FRPC_BUF_SIZE) {
			return ENOMEM;
		}
		pa = fr.pa + at;
		where[i] = at;
		frpc_put64(16 * i, pa);                                 /* rpra[i]: pv (only its offset in the page counts) */
		frpc_put64(16 * i + 8, length);
		frpc_put32(16 * n + 8 * i, length ? 1 : 0);             /* list[i]: one page run, its index */
		frpc_put32(16 * n + 8 * i + 4, i);
		frpc_put64(24 * n + 16 * i, FRPC_SESSION << 32 | (pa & ~0xfffull));
		frpc_put64(24 * n + 16 * i + 8, length ? ((((pa + length - 1) >> 12) - (pa >> 12)) + 1) << 12 : 0);
		for (uint32_t b = 0; b < length; b++) {
			fr.buf[at + b] = i < nin && args[i].in != NULL ? ((const uint8_t *)args[i].in)[b] : 0;
		}
		at += length;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ctx = (uint64_t)(++fr.seq & 0xfff) << 4 | fr.pd;  /* (Test 213) boot-arg slpi_pd: 2 the sensors domain (default), 0 the root */
	msg.client_id = kernel ? 0 : FRPC_CLIENT;
	msg.tid = FRPC_CLIENT + 1;
	msg.ctx = ctx;
	msg.handle = handle;
	msg.sc = sc;
	msg.addr = FRPC_SESSION << 32 | fr.pa;
	msg.size = (at + 0xfff) & ~0xfffu;
	fr.answered = false;
	fr.calls++;
	error = sl_send(&sl.ch[SL_FRPC], &msg, sizeof(msg));
	if (error != 0) {
		return error;
	}
	for (uint32_t ms = 0; !fr.answered || fr.answer_ctx != ctx; ms++) {
		if (timeout_ms != 0 && ms >= timeout_ms) {
			return ETIMEDOUT;
		}
		sleep_ms(1);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = nin; i < n; i++) {
		uint64_t length = frpc_get64(16 * i + 8);

		length = length > args[i].length ? args[i].length : length;
		for (uint32_t b = 0; b < length && args[i].out != NULL; b++) {
			((uint8_t *)args[i].out)[b] = fr.buf[where[i] + b];
		}
	}
	*retval = fr.answer;
	return 0;
}

/* the SLPI's nested buffers (hexagonrpcd iobuffer.c): a 32-bit length, padding to 8 bytes, the bytes; again */
static uint32_t
frpc_decode(const uint8_t *src, uint32_t length, uint32_t count, const uint8_t **bufs, uint32_t *sizes)
{
	uint32_t at = 0, i;

	for (i = 0; i < count && at + 4 <= length; i++) {
		uint32_t size;

		memcpy(&size, src + at, 4);
		at += 4;
		if (size != 0 && (at & 7)) {
			at = (at + 7) & ~7u;
		}
		if (at + size > length) {
			break;
		}
		bufs[i] = src + at;
		sizes[i] = size;
		at += size;
	}
	return i;
}

static uint32_t
frpc_encode(uint8_t *dst, uint32_t capacity, uint32_t count, const uint8_t *const *bufs, const uint32_t *sizes)
{
	uint32_t at = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (at + 4 > capacity) {
			return at;
		}
		memcpy(dst + at, &sizes[i], 4);
		at += 4;
		if (sizes[i] != 0) {
			while (at & 7) {
				dst[at++] = 0;
			}
			if (at + sizes[i] > capacity) {
				return at;
			}
			if (bufs[i] != NULL) {
				memcpy(dst + at, bufs[i], sizes[i]);
			} else {
				bzero(dst + at, sizes[i]);
			}
			at += sizes[i];
		}
	}
	return at;
}

static void
frpc_show(const char *what, const uint8_t *p, uint32_t size)
{
	char text[97];
	uint32_t k = 0;

	for (uint32_t i = 0; i < size && k < sizeof(text) - 3; i++) {
		uint8_t c = p[i];

		text[k++] = c >= 0x20 && c < 0x7f ? (char)c : c == 0 ? '|' : '.';
	}
	text[k] = 0;
	tell("slpi: frpc:   %s (%u bytes): %s\n", what, size, text);
}

/*
 * (r5q, Test 216) apps_std for the SLPI (hexagonrpcd apps_std.c): the files its sensor registry reads, from a table
 * compiled into the kernel (redwood_sensor_files.h, made by wifi/mk_sensor_files.py from the phone's own
 * /persist/sensors). /mnt/vendor/persist/... is the same as /persist/...; nothing is written (opening for writing
 * is "unsupported", mkdir is let pass). Methods: 2 fflush, 3 fclose, 4 fread, 9 fseek, 19 fopen_with_env, 26
 * opendir, 27 closedir, 28 readdir, 29 mkdir, 31 stat (an extended method: its number the first word).
 */
#include "redwood_sensor_files.h"

/*
 * (Test 219) Files the SLPI writes are kept in memory (lost at a restart: the registry writes them again at its start),
 * over the compiled-in table: Test 218's registry wanted to write sns_reg_version, remove (method 24) and so on, and with
 * writes refused it never came up. Methods (libadsprpc's apps_std numbering): 2 fflush, 3 fclose, 4 fread, 5 fwrite,
 * 9 fseek, 19 fopen_with_env, 22 fileExists, 23 fsync, 24 fremove, 26 opendir, 27 closedir, 28 readdir, 29 mkdir,
 * 30 rmdir, 31 stat, 32 ftrunc, 33 frename (31 and up: extended, the number the first word).
 */
#define SEE_FILES               (sizeof(sensor_files) / sizeof(sensor_files[0]))
#define SEE_WFILES              160     /* (Test 220: the registry may rebuild all ~90 of its files) */
#define SEE_WFILE_SIZE          4096
static struct {
	bool used;
	char path[128];
	uint32_t length;
	uint8_t data[SEE_WFILE_SIZE];
} see_wfiles[SEE_WFILES];
static uint8_t see_hidden[SEE_FILES / 8 + 1];   /* table files removed */
static struct {
	int32_t kind;                           /* -1 free, 0 the table, 1 written */
	uint32_t index, at;
} see_fds[32];
static struct {
	bool used;
	char path[128];
	uint32_t next;
} see_dirs[8];
static uint32_t see_files_served, see_files_missing, see_files_written;

static const char *
see_files_path(const char *path)
{
	/* (Test 217) "." and ".." resolved ("/persist/sensors/registry/registry/../sns_reg_version", Test 216), then the alias */
	static char clean[160];
	size_t at = 0;

	while (*path != 0 && at < sizeof(clean) - 2) {
		const char *end;
		size_t len;

		while (*path == '/') {
			path++;
		}
		if (*path == 0) {
			break;
		}
		end = strchr(path, '/');
		len = end ? (size_t)(end - path) : strlen(path);
		if (len == 1 && path[0] == '.') {
			/* nothing */
		} else if (len == 2 && path[0] == '.' && path[1] == '.') {
			while (at > 0 && clean[at - 1] != '/') {
				at--;
			}
			at = at > 0 ? at - 1 : 0;
		} else if (at + 1 + len < sizeof(clean) - 1) {
			clean[at++] = '/';
			memcpy(clean + at, path, len);
			at += len;
		}
		path += len;
	}
	clean[at] = 0;
	if (at == 0) {
		strlcpy(clean, "/", sizeof(clean));
	}
	return strncmp(clean, "/mnt/vendor/persist/", 20) == 0 ? clean + 11 : clean;
}

/* the path of entry k of the files seen together (the table, then the written ones); NULL: none there now */
static const char *
see_files_entry(uint32_t k)
{
	if (k < SEE_FILES) {
		if (see_hidden[k / 8] & (1u << (k % 8))) {
			return NULL;
		}
		for (uint32_t w = 0; w < SEE_WFILES; w++) {
			if (see_wfiles[w].used && strcmp(see_wfiles[w].path, sensor_files[k].path) == 0) {
				return NULL;            /* (written over: the written one counts) */
			}
		}
		return sensor_files[k].path;
	}
	k -= SEE_FILES;
	return k < SEE_WFILES && see_wfiles[k].used ? see_wfiles[k].path : NULL;
}

static bool
see_files_get(const char *path, int32_t *kind, uint32_t *index)
{
	path = see_files_path(path);
	for (uint32_t w = 0; w < SEE_WFILES; w++) {
		if (see_wfiles[w].used && strcmp(see_wfiles[w].path, path) == 0) {
			*kind = 1;
			*index = w;
			return true;
		}
	}
	for (uint32_t i = 0; i < SEE_FILES; i++) {
		if (!(see_hidden[i / 8] & (1u << (i % 8))) && strcmp(sensor_files[i].path, path) == 0) {
			*kind = 0;
			*index = i;
			return true;
		}
	}
	return false;
}

static const uint8_t *
see_files_bytes(int32_t kind, uint32_t index, uint32_t *length)
{
	if (kind == 1) {
		*length = see_wfiles[index].length;
		return see_wfiles[index].data;
	}
	*length = sensor_files[index].length;
	return sensor_files[index].data;
}

static void
see_files_hide(const char *path)
{
	for (uint32_t i = 0; i < SEE_FILES; i++) {
		if (strcmp(sensor_files[i].path, path) == 0) {
			see_hidden[i / 8] |= (uint8_t)(1u << (i % 8));
		}
	}
	for (uint32_t w = 0; w < SEE_WFILES; w++) {
		if (see_wfiles[w].used && strcmp(see_wfiles[w].path, path) == 0) {
			see_wfiles[w].used = false;
		}
	}
}

/* a written file for path (made, or the existing one); -1: no room */
static int32_t
see_files_writable(const char *path, bool keep)
{
	int32_t kind;
	uint32_t index, length;
	const uint8_t *old;

	path = see_files_path(path);
	if (see_files_get(path, &kind, &index) && kind == 1) {
		if (!keep) {
			see_wfiles[index].length = 0;
		}
		return (int32_t)index;
	}
	for (uint32_t w = 0; w < SEE_WFILES; w++) {
		if (!see_wfiles[w].used) {
			see_wfiles[w].used = true;
			strlcpy(see_wfiles[w].path, path, sizeof(see_wfiles[w].path));
			see_wfiles[w].length = 0;
			if (keep && see_files_get(path, &kind, &index) && kind == 0) {
				old = see_files_bytes(kind, index, &length);
				length = length < SEE_WFILE_SIZE ? length : SEE_WFILE_SIZE;
				memcpy(see_wfiles[w].data, old, length);
				see_wfiles[w].length = length;
			}
			see_files_written++;
			return (int32_t)w;
		}
	}
	return -1;
}

/* a directory: some file lies below it ("/mnt/vendor" holds "persist" too) */
static bool
see_files_is_dir(const char *path)
{
	char dir[164];
	size_t n;

	if (strcmp(path, "/mnt/vendor") == 0 || strcmp(path, "/mnt/vendor/") == 0) {
		return true;
	}
	strlcpy(dir, see_files_path(path), sizeof(dir) - 1);
	n = strlen(dir);
	if (n == 0 || dir[n - 1] != '/') {
		dir[n++] = '/';
		dir[n] = 0;
	}
	for (uint32_t k = 0; k < SEE_FILES + SEE_WFILES; k++) {
		const char *p = see_files_entry(k);

		if (p != NULL && strncmp(p, dir, n) == 0) {
			return true;
		}
	}
	return false;
}

/* the next name directly in a directory; false: none left */
static bool
see_files_next_name(const char *path, uint32_t *next, char *name, size_t size)
{
	char dir[164];
	size_t n;

	if (strcmp(path, "/mnt/vendor") == 0 || strcmp(path, "/mnt/vendor/") == 0) {
		if (*next == 0) {
			*next = 1;
			strlcpy(name, "persist", size);
			return true;
		}
		return false;
	}
	strlcpy(dir, see_files_path(path), sizeof(dir) - 1);
	n = strlen(dir);
	if (n == 0 || dir[n - 1] != '/') {
		dir[n++] = '/';
		dir[n] = 0;
	}
	while (*next < SEE_FILES + SEE_WFILES) {
		uint32_t k = (*next)++;
		const char *p = see_files_entry(k), *rest, *slash;
		size_t len;
		bool seen = false;

		if (p == NULL || strncmp(p, dir, n) != 0) {
			continue;
		}
		rest = p + n;
		slash = strchr(rest, '/');
		len = slash ? (size_t)(slash - rest) : strlen(rest);
		for (uint32_t j = 0; slash && j < k; j++) {     /* a subdirectory: named once */
			const char *q = see_files_entry(j);

			if (q != NULL && strncmp(q, dir, n) == 0 && strncmp(q + n, rest, len) == 0 && q[n + len] == '/') {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		len = len < size - 1 ? len : size - 1;
		memcpy(name, rest, len);
		name[len] = 0;
		return true;
	}
	return false;
}

static void
see_files_put32(uint8_t *at, uint32_t value)
{
	memcpy(at, &value, 4);
}

static void
see_files_put64(uint8_t *at, uint64_t value)
{
	memcpy(at, &value, 8);
}

/* one apps_std call; its first output buffer (prim) and data buffer filled, their sizes said; the answer returned */
static uint32_t
see_files_call(uint32_t method, uint32_t got, const uint8_t **bufs, const uint32_t *sizes, uint32_t word0, uint32_t word1,
    uint32_t word2, uint8_t *prim, uint32_t *prim_size, uint8_t *data, uint32_t *data_size)
{
	static bool first = true;
	int32_t kind;
	uint32_t index, length;
	const uint8_t *bytes;

	if (first) {
		first = false;
		for (unsigned k = 0; k < 32; k++) {
			see_fds[k].kind = -1;
		}
	}
	switch (method) {
	case 2:                                         /* fflush(fd) */
	case 23:                                        /* fsync(fd) */
		return 0;
	case 3:                                         /* fclose(fd) */
		if (word0 < 32) {
			see_fds[word0].kind = -1;
		}
		return 0;
	case 4: {                                       /* fread(fd, size) -> { read, eof }, the bytes */
		uint32_t want = word1, n;

		*prim_size = 8;
		*data_size = want > 6144 ? 6144 : want;
		if (word0 >= 32 || see_fds[word0].kind < 0) {
			return 1;
		}
		bytes = see_files_bytes(see_fds[word0].kind, see_fds[word0].index, &length);
		n = see_fds[word0].at < length ? length - see_fds[word0].at : 0;
		n = n < *data_size ? n : *data_size;
		memcpy(data, bytes + see_fds[word0].at, n);
		bzero(data + n, *data_size - n);
		see_fds[word0].at += n;
		see_files_put32(prim, n);
		see_files_put32(prim + 4, n < want);
		return 0;
	}
	case 5: {                                       /* fwrite(fd, bytes) -> { written, eof } */
		uint32_t n = got >= 2 ? sizes[1] : 0;

		*prim_size = 8;
		if (word0 >= 32 || see_fds[word0].kind != 1) {
			return 1;
		}
		index = see_fds[word0].index;
		if (see_fds[word0].at + n > SEE_WFILE_SIZE) {
			n = see_fds[word0].at < SEE_WFILE_SIZE ? SEE_WFILE_SIZE - see_fds[word0].at : 0;
		}
		memcpy(see_wfiles[index].data + see_fds[word0].at, bufs[1], n);
		see_fds[word0].at += n;
		see_wfiles[index].length = see_fds[word0].at > see_wfiles[index].length ? see_fds[word0].at : see_wfiles[index].length;
		see_files_put32(prim, n);
		return 0;
	}
	case 9: {                                       /* fseek(fd, pos, whence) */
		int64_t pos;

		if (word0 >= 32 || see_fds[word0].kind < 0) {
			return 1;
		}
		see_files_bytes(see_fds[word0].kind, see_fds[word0].index, &length);
		pos = word2 == 0 ? (int64_t)(int32_t)word1 : word2 == 1 ? (int64_t)see_fds[word0].at + (int32_t)word1 :
		    (int64_t)length + (int32_t)word1;
		if (pos < 0 || pos > (see_fds[word0].kind == 1 ? SEE_WFILE_SIZE : length)) {
			return 1;
		}
		see_fds[word0].at = (uint32_t)pos;
		return 0;
	}
	case 19: {                                      /* fopen_with_env(env, delim, name, mode) -> fd */
		const char *name = got >= 4 ? (const char *)bufs[3] : "", *mode = got >= 5 ? (const char *)bufs[4] : "r";
		bool writes = mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+') != NULL;

		*prim_size = 4;
		if (writes) {
			int32_t w = see_files_writable(name, mode[0] != 'w');

			if (w < 0) {
				tell("slpi: files: open %s (%s): no room to write\n", name, mode);
				return 2;
			}
			kind = 1;
			index = (uint32_t)w;
			if (see_files_written <= 40) {
				tell("slpi: files: open %s (%s): written in memory\n", see_files_path(name), mode);
			}
		} else if (!see_files_get(name, &kind, &index)) {
			see_files_missing++;
			tell("slpi: files: open %s: not here\n", name);
			return 1;
		}
		for (unsigned k = 0; k < 32; k++) {
			if (see_fds[k].kind < 0) {
				see_fds[k].kind = kind;
				see_fds[k].index = index;
				see_fds[k].at = mode[0] == 'a' ? see_files_bytes(kind, index, &length), length : 0;
				see_files_put32(prim, k);
				see_files_served++;
				return 0;
			}
		}
		return 2;
	}
	case 22: {                                      /* fileExists(path) -> exists */
		const char *path = got >= 2 ? (const char *)bufs[1] : "";

		*prim_size = 4;
		see_files_put32(prim, see_files_get(path, &kind, &index) || see_files_is_dir(path));
		return 0;
	}
	case 24: {                                      /* fremove(path) */
		const char *path = got >= 2 ? (const char *)bufs[1] : "";

		see_files_hide(see_files_path(path));
		return 0;
	}
	case 26: {                                      /* opendir(path) -> dir */
		const char *path = got >= 2 ? (const char *)bufs[1] : "";

		*prim_size = 8;
		if (!see_files_is_dir(path)) {
			see_files_missing++;
			tell("slpi: files: opendir %s: not here\n", path);
			return 1;
		}
		for (unsigned k = 0; k < 8; k++) {
			if (!see_dirs[k].used) {
				see_dirs[k].used = true;
				see_dirs[k].next = 0;
				strlcpy(see_dirs[k].path, path, sizeof(see_dirs[k].path));
				see_files_put64(prim, k + 1);
				return 0;
			}
		}
		return 2;
	}
	case 27:                                        /* closedir(dir) */
		if (word0 >= 1 && word0 <= 8) {
			see_dirs[word0 - 1].used = false;
		}
		return 0;
	case 28: {                                      /* readdir(dir) -> { inode, name[255], eof } */
		char name[255];

		*prim_size = 264;
		if (word0 < 1 || word0 > 8 || !see_dirs[word0 - 1].used) {
			return 1;
		}
		bzero(name, sizeof(name));
		if (!see_files_next_name(see_dirs[word0 - 1].path, &see_dirs[word0 - 1].next, name, sizeof(name))) {
			see_files_put32(prim + 260, 1);
			return 0;
		}
		memcpy(prim + 4, name, sizeof(name));
		return 0;
	}
	case 29:                                        /* mkdir, rmdir: let pass */
	case 30:
		return 0;
	case 31: {                                      /* stat(path) -> 96 bytes */
		const char *path = got >= 2 ? (const char *)bufs[1] : "";

		*prim_size = 96;
		if (see_files_get(path, &kind, &index)) {
			see_files_bytes(kind, index, &length);
			see_files_put32(prim + 24, 0100644);
			see_files_put32(prim + 28, 1);
			see_files_put64(prim + 40, length);
		} else if (see_files_is_dir(path)) {
			see_files_put32(prim + 24, 040755);
			see_files_put32(prim + 28, 2);
			see_files_put64(prim + 40, 4096);
		} else {
			see_files_missing++;
			tell("slpi: files: stat %s: not here\n", path);
			return 1;
		}
		return 0;
	}
	case 32:                                        /* ftrunc(fd, length) */
		if (word0 >= 32 || see_fds[word0].kind != 1 || word1 > SEE_WFILE_SIZE) {
			return 1;
		}
		see_wfiles[see_fds[word0].index].length = word1;
		return 0;
	case 33: {                                      /* frename(old, new) */
		const char *from = got >= 2 ? (const char *)bufs[1] : "", *to = got >= 3 ? (const char *)bufs[2] : "";
		char old_path[160];
		int32_t w;

		if (!see_files_get(from, &kind, &index)) {
			return 1;
		}
		strlcpy(old_path, see_files_path(from), sizeof(old_path));
		bytes = see_files_bytes(kind, index, &length);
		w = see_files_writable(to, false);
		if (w < 0) {
			return 2;
		}
		length = length < SEE_WFILE_SIZE ? length : SEE_WFILE_SIZE;
		memmove(see_wfiles[w].data, bytes, length);
		see_wfiles[w].length = length;
		see_files_hide(old_path);
		return 0;
	}
	default:
		tell("slpi: files: method %u: not done here\n", method);
		return FRPC_EUNSUPPORTED;
	}
}

static void
frpc_thread(__unused void *parameter, __unused wait_result_t result)
{
	static uint8_t in_prim[16], out_prim[16], enc_out[8192], enc_in[2048];
	static uint8_t reply[2048];
	int32_t retval = -1, client = FRPC_CLIENT;
	uint32_t rctx = 0, answer = 0xffffffffu, enc_out_len = 0;
	int error;

	for (unsigned ms = 0; ms < 20000 && !sl.ch[SL_FRPC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!sl.ch[SL_FRPC].open) {
		tell("slpi: frpc: the channel was not opened by the SLPI: no FastRPC\n");
		return;
	}
	/* (Test 206) Test 205 went silent here, and the Wi-Fi's start with it: wait until the Wi-Fi has done its SMMU */
	if (fr.early) {
		tell("slpi: frpc: the channel is open; at once (the SLPI was released for this)\n");
	} else {
		tell("slpi: frpc: the channel is open; waiting 30 s (the Wi-Fi's start first)\n");
		sleep_ms(30000);
		tell("slpi: frpc: the SMMU's stream matches next\n");
		if (!frpc_smmu()) {
			return;
		}
		fr.buf = frpc_buffer(&fr.pa);
	}
	tell("slpi: frpc: memory %s at %#llx\n", fr.buf ? "mapped" : "NOT mapped", fr.pa);
	if (fr.buf == NULL) {
		tell("slpi: frpc: no memory below 4 GB\n");
		return;
	}
	{
		struct frpc_arg args[1] = { { &client, NULL, 4 } };

		fr.pd = FRPC_PD_SENSORS;
		PE_parse_boot_argn("slpi_pd", &fr.pd, sizeof(fr.pd));
		error = frpc_invoke(true, 1, FRPC_SCALARS(0, 1, 0), args, 5000, &retval);
		tell("slpi: frpc: attach to domain %u (INIT_ATTACH): %s, answer %d\n", fr.pd, error ? "NO ANSWER" : "answered", retval);
		if (error != 0 || retval != 0) {
			return;
		}
	}
	{
		/*
		 * (Test 215) As hexagonrpcd's register_fastrpc_listener(): open the SLPI's "adsp_default_listener" through its
		 * remotectl (handle 0, method 0: in {name length, error capacity} + name; out {handle, error} + error text), then
		 * call its register (method 0, nothing): the SLPI is told a default listener is here for its calls to us.
		 */
		static const char name[] = "adsp_default_listener";
		uint32_t prim_in[2] = { sizeof(name), 256 }, prim_out[2] = { 0, 0 };
		static char dlerr[256];
		struct frpc_arg args[4] = {
			{ prim_in, NULL, 8 }, { name, NULL, sizeof(name) }, { NULL, prim_out, 8 }, { NULL, dlerr, sizeof(dlerr) },
		};

		/* (Test 218) Test 217's first try came too soon ("bad parameter"): tried again for up to 15 s */
		for (unsigned attempt = 0; attempt < 30; attempt++) {
			error = frpc_invoke(false, 0, FRPC_SCALARS(0, 2, 2), args, 5000, &retval);
			if (error != 0 || (retval == 0 && prim_out[0] != 0)) {
				break;
			}
			sleep_ms(500);
		}
		tell("slpi: frpc: open \"%s\": %s, answer %d, handle %u, error %d %.64s\n", name, error ? "NO ANSWER" : "answered",
		    retval, prim_out[0], (int32_t)prim_out[1], dlerr);
		if (error == 0 && retval == 0 && prim_out[0] != 0) {
			error = frpc_invoke(false, prim_out[0], FRPC_SCALARS(0, 0, 0), NULL, 5000, &retval);
			tell("slpi: frpc: adsp_default_listener register: %s, answer %d\n", error ? "NO ANSWER" : "answered", retval);
		}
	}
	error = frpc_invoke(false, 3, FRPC_SCALARS(3, 0, 0), NULL, 5000, &retval);
	tell("slpi: frpc: listener init2: %s, answer %d\n", error ? "NO ANSWER" : "answered", retval);
	if (error != 0 || retval != 0) {
		return;
	}
	for (unsigned call = 0;; call++) {
		const uint8_t *bufs[16];
		uint32_t sizes[16], words[4], handle, sc, method, nin, nout, got, enc_in_len;
		struct frpc_arg args[4] = {
			{ in_prim, NULL, 16 }, { enc_out, NULL, enc_out_len }, { NULL, out_prim, 16 }, { NULL, enc_in, sizeof(enc_in) },
		};

		words[0] = rctx;
		words[1] = answer;
		words[2] = enc_out_len;
		words[3] = sizeof(enc_in);
		memcpy(in_prim, words, 16);
		error = frpc_invoke(false, 3, FRPC_SCALARS(4, 2, 2), args, 0, &retval);
		/*
		 * (Test 237) Once (a restored Test 232 boot) the SLPI refused the answer to call 39 -- a read of the sensor
		 * registry it takes on every other boot, word for word -- with 0x8000040e, and the listener stopping there left
		 * that boot with no sensors. Now: the same answer sent again (twice), then a plain failure for that one call
		 * (no data), and the listener goes on; it stops only after 5 refusals in a row.
		 */
		for (unsigned again = 0; error == 0 && retval != 0 && again < 3; again++) {
			static unsigned refusals;

			tell("slpi: frpc: next2 refused (answer %d) for call %u's answer (%u, %u bytes encoded): %s\n", retval, call - 1,
			    answer, enc_out_len, again < 2 ? "sent again" : "a failure sent instead");
			if (++refusals > 5) {
				break;
			}
			if (again == 2) {
				static const uint8_t *const none[8] = { NULL };
				static const uint32_t zero[8] = { 0 };

				answer = FRPC_EUNSUPPORTED;
				enc_out_len = frpc_encode(enc_out, sizeof(enc_out), 2, none, zero);
				args[1].length = enc_out_len;
			}
			words[1] = answer;
			words[2] = enc_out_len;
			memcpy(in_prim, words, 16);
			error = frpc_invoke(false, 3, FRPC_SCALARS(4, 2, 2), args, 0, &retval);
			if (error == 0 && retval == 0) {
				refusals = 0;
			}
		}
		if (error != 0 || retval != 0) {
			tell("slpi: frpc: next2: %s, answer %d: the listener stops\n", error ? "NOT SENT" : "answered", retval);
			return;
		}
		memcpy(words, out_prim, 16);
		rctx = words[0];
		handle = words[1];
		sc = words[2];
		enc_in_len = words[3] > sizeof(enc_in) ? sizeof(enc_in) : words[3];
		method = (sc >> 24) & 0x1f;
		nin = (sc >> 16) & 0xff;
		nout = (sc >> 8) & 0xff;
		got = frpc_decode(enc_in, enc_in_len, nin, bufs, sizes);
		if (method == 31 && got > 0 && sizes[0] >= 4) {
			memcpy(&method, bufs[0], 4);
		}
		if (call < 40) {
			tell("slpi: frpc: call %u from the SLPI: handle %u, method %u, %u in, %u out (sc %#x)\n", call, handle, method, nin, nout,
			    sc);
			for (uint32_t i = 1; i < got; i++) {
				frpc_show("in", bufs[i], sizes[i]);
			}
		} else if (call % 100 == 0) {
			tell("slpi: frpc: %u calls so far; files served %u, not here %u\n", call, see_files_served, see_files_missing);
		}
		answer = FRPC_EUNSUPPORTED;
		{
			static uint8_t prim_out_buf[288], data_out[6144];
			const uint8_t *outs[8] = { NULL };
			uint32_t out_sizes[8] = { 0 }, word0 = 0, word1 = 0, word2 = 0;
			const char *name = got >= 2 ? (const char *)bufs[1] : "";

			bzero(prim_out_buf, sizeof(prim_out_buf));
			if (got >= 1 && sizes[0] >= 4) {
				memcpy(&word0, bufs[0], 4);
			}
			if (got >= 1 && sizes[0] >= 8) {
				memcpy(&word1, bufs[0] + 4, 4);
			}
			if (got >= 1 && sizes[0] >= 12) {
				memcpy(&word2, bufs[0] + 8, 4);
			}
			if (handle == 0 && method == 0 && got >= 2 && nout >= 2) {
				/* remotectl open(name): our interfaces by name */
				uint32_t prim[2];

				prim[0] = strncmp(name, "apps_std", sizes[1]) == 0 ? 1 : strncmp(name, "apps_mem", sizes[1]) == 0 ? 2 : 0;
				prim[1] = prim[0] ? 0 : (uint32_t)-5;
				answer = prim[1];
				memcpy(prim_out_buf, prim, 8);
				outs[0] = prim_out_buf;
				out_sizes[0] = 8;
				out_sizes[1] = word1 > 256 ? 256 : word1;
			} else if (handle == 0 && method == 1 && nout >= 2) {
				/* remotectl close(handle): nothing to do; no error text */
				answer = 0;
				outs[0] = prim_out_buf;
				out_sizes[0] = 4;
				out_sizes[1] = word1 > 256 ? 256 : word1;
			} else if (handle == 1) {
				answer = see_files_call(method, got, bufs, sizes, word0, word1, word2, prim_out_buf, &out_sizes[0],
				    data_out, &out_sizes[1]);
				outs[0] = prim_out_buf;
				outs[1] = data_out;
			}
			enc_out_len = frpc_encode(enc_out, sizeof(enc_out), nout > 8 ? 8 : nout, outs, out_sizes);
		}
		(void)reply;
	}
}

static void
sl_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	bool told = false;
	uint32_t slpi = 0;

	PE_parse_boot_argn("slpi", &slpi, sizeof(slpi));
	if (slpi >= 4) {
		thread_t thread;

		if (kernel_thread_start(frpc_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	for (;;) {
		sleep_us(1000);
		for (;;) {
			struct sl_packet *packet = NULL;
			unsigned from = SL_CHANNELS;

			lck_mtx_lock(&sl_lock);
			sl_pump();
			for (unsigned c = 0; c < SL_CHANNELS && packet == NULL; c++) {
				if ((packet = sl.ch[c].first) != NULL) {
					sl.ch[c].first = packet->next;
					sl.ch[c].last = sl.ch[c].first == NULL ? NULL : sl.ch[c].last;
					from = c;
				}
			}
			lck_mtx_unlock(&sl_lock);
			if (packet == NULL) {
				break;
			}
			if (from == SL_QRTR) {
				sl_qrtr_input(packet->data, packet->length);
			} else {
				frpc_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		if (!told && now_us() - began > 15000000ull) {
			unsigned count = 0, see = 0;

			told = true;
			sl_smp2p_show();
			for (unsigned i = 0; i < sizeof(sl.servers) / sizeof(sl.servers[0]); i++) {
				count += sl.servers[i].there;
				if (sl.servers[i].there && sl.servers[i].service == 400 && see == 0) {
					see++;
					see_server(sl.servers[i].node, sl.servers[i].port);
				}
			}
			tell("slpi: after 15 s: versions %s, IPCRTR %s, FastRPC %s, GLINK said %u heard %u, QRTR in %u out %u, node %u, %u "
			    "services%s; the SLPI also opened \"%s\" \"%s\"\n", sl.agreed ? "agreed" : "NOT AGREED",
			    sl.ch[SL_QRTR].open ? "OPEN" : "NOT OPEN", sl.ch[SL_FRPC].open ? "OPEN" : "NOT OPEN", sl.said, sl.heard,
			    sl.qrtr_in, sl.qrtr_out, sl.qrtr_node, count, see ? ", THE SENSORS' (400) AMONG THEM" : "", sl.others[0],
			    sl.others[1]);
		}
	}
}

/*
 * (Test 214) Our SMP2P item for the SLPI (live DT qcom,smp2p-dsps: smem <481 430> -- its 481, ours 430; doorbell APCS IPC
 * bit 26), made before it is released, as Linux's qcom_smp2p makes it at probe: "master-kernel" 0 (no stop asked) and
 * "sleepstate" 1 -- bit 0, the apps processor awake (qcom_smp2p_sleepstate.c). Without it the SLPI's sensors framework
 * may take us for asleep and put off what it asks of us (its "remote_proc_state" sensor was missing).
 */
#define SLPI_SMP2P_OURS         430
#define SLPI_SMP2P_THEIRS       481

static void
sl_smp2p_make(void)
{
	uint32_t size = 0, item, partition, cacheline = 0, psize = 0, name[4];

	partition = smem_partition(SMEM_US, SLPI_HOST, &cacheline, &psize);
	item = sl_item(SLPI_SMP2P_OURS, &size);
	if (item == 0) {
		if (partition == 0 || !item_make(partition, SLPI_SMP2P_OURS, sizeof(struct smp2p))) {
			tell("slpi: smp2p: our item (430) could not be made\n");
			return;
		}
		item = sl_item(SLPI_SMP2P_OURS, &size);
	}
	if (item == 0) {
		return;
	}
	for (uint32_t at = 0; at < sizeof(struct smp2p); at += 4) {
		smem_poke(item + at, 0);
	}
	settle();
	smem_poke(item, SMP2P_MAGIC);
	smem_poke(item + SMP2P_AT_PROCESSORS, SMEM_US | SLPI_HOST << 16);
	bzero(name, sizeof(name));
	strlcpy((char *)name, "master-kernel", sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	bzero(name, sizeof(name));
	strlcpy((char *)name, "sleepstate", sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(item + SMP2P_AT_NAME(1) + 4 * word, name[word]);
	}
	smem_poke(item + SMP2P_AT_VALUE(1), 1);                         /* awake */
	smem_poke(item + SMP2P_AT_COUNTS, 16 | 2u << 16);
	smem_poke(item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	need(&apcs);
	wr(apcs.base + 0x0c, 1u << 26);
	tell("slpi: smp2p: our item (430) at %#x: \"master-kernel\" 0, \"sleepstate\" 1 (awake)\n", item);
}

static void
sl_smp2p_show(void)
{
	struct smp2p theirs;
	uint32_t size = 0, item = sl_item(SLPI_SMP2P_THEIRS, &size);

	if (item == 0 || !smem_peek(item, &theirs, sizeof(theirs)) || theirs.magic != SMP2P_MAGIC) {
		tell("slpi: smp2p: the SLPI's item (481) is not there\n");
		return;
	}
	for (unsigned i = 0; i < theirs.valid && i < 16; i++) {
		char name[17];

		memcpy(name, theirs.entry[i].name, 16);
		name[16] = 0;
		tell("slpi: smp2p: the SLPI's \"%s\" = %#x\n", name, theirs.entry[i].value);
	}
}

/* (Test 201) boot-arg slpi=2: the loader has started the SLPI; its GLINK edge found (our queue made) and spoken */
static void
r5q_slpi_start(void)
{
	uint32_t slpi = 0, size = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("slpi", &slpi, sizeof(slpi)) || slpi < 2) {
		return;
	}
	if (slpi >= 5) {
		/*
		 * (Test 212) The loader has the SLPI's image in place, accepted, but held (Test 211: its sensor registry is read
		 * through FastRPC only as it starts, and our listener came 30 s late -- no call ever came). FastRPC's stream
		 * matches and buffer first, then auth_and_reset from here; the link and the listener follow at once.
		 */
		uint64_t answer = ~0ull;
		int32_t result;

		tell("slpi: release: FastRPC's stream matches and buffer first\n");
		{
			/*
			 * (Test 222) the sensors' own supply, live DT qcom,ssc sensor_vdd-supply: PM8150L LDO8 at 1.8 V ("ldoc8", an
			 * RPMh on/off regulator), which Android's PIL votes on as it starts the SLPI (qcom,active-reg-names). Test 221:
			 * without it the accelerometer is there but "not available" (SEE error 6).
			 */
			extern bool redwood_r5q_rpmh_enable(const char *id);

			tell("slpi: release: the sensors' supply (ldoc8, 1.8 V) voted on: %s\n",
			    redwood_r5q_rpmh_enable("ldoc8") ? "done" : "NOT DONE");
			sleep_ms(20);
		}
		sl_smp2p_make();
		if (!frpc_smmu() || (fr.buf = frpc_buffer(&fr.pa)) == NULL) {
			tell("slpi: release: not ready: the SLPI stays held\n");
			return;
		}
		result = scm("to release the SLPI", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, 12, 0, 0, &answer);
		tell("slpi: release: auth_and_reset: TrustZone returns %d, answer %#llx%s\n", result, answer,
		    result == 0 && answer == 0 ? ": THE SLPI IS RUNNING" : ": NOT RELEASED");
		if (result != 0 || answer != 0) {
			return;
		}
		fr.early = true;
	}
	for (unsigned waited = 0; waited < 150; waited++) {
		sl.descriptor = sl_item(ITEM_GLINK_DESCRIPTOR, &size);
		sl.theirs = sl_item(ITEM_GLINK_THEIR_QUEUE, &sl.theirs_size);
		if (sl.descriptor != 0 && sl.theirs != 0) {
			break;
		}
		sleep_ms(100);
	}
	tell("slpi: SMEM partition 0/3 at %#x; GLINK descriptor %#x (%u bytes), its queue %#x (%u bytes)\n", sl.partition,
	    sl.descriptor, size, sl.theirs, sl.theirs_size);
	if (sl.partition == 0 || sl.descriptor == 0 || size != 32 || sl.theirs == 0 || sl.theirs_size < 1024 || (sl.theirs_size & 7)) {
		tell("slpi: the SLPI's GLINK items are not there (is it started? slpi=2 in the loader): no link\n");
		return;
	}
	sl.ours = sl_item(ITEM_GLINK_OUR_QUEUE, &sl.ours_size);
	if (sl.ours == 0) {
		if (!item_make(sl.partition, ITEM_GLINK_OUR_QUEUE, 16384)) {
			tell("slpi: our GLINK queue (479) could not be made\n");
			return;
		}
		sl.ours = sl_item(ITEM_GLINK_OUR_QUEUE, &sl.ours_size);
	}
	tell("slpi: our queue %#x (%u bytes); marks: ours read %#x written %#x, its read %#x written %#x\n", sl.ours, sl.ours_size,
	    smem_word(sl.descriptor + GLINK_OUR_TAIL), smem_word(sl.descriptor + GLINK_OUR_HEAD),
	    smem_word(sl.descriptor + GLINK_THEIR_TAIL), smem_word(sl.descriptor + GLINK_THEIR_HEAD));
	if (sl.ours == 0 || sl.ours_size < 1024 || (sl.ours_size & 7)) {
		return;
	}
	lck_mtx_lock(&sl_lock);
	sl.features = GLINK_INTENT_REUSE;
	sl.trace = 40;
	sl.up = true;
	sl_say(GLINK_VERSION, GLINK_VERSION_1, sl.features, NULL, 0);
	lck_mtx_unlock(&sl_lock);
	if (kernel_thread_start(sl_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static int qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data,
    uint32_t length);

static void
r5q_slim_answer(const uint8_t *m, uint32_t size)
{
	uint32_t at = 7, result = 0xffffffff;

	if (size < 7) {
		return;
	}
	while (at + 3 <= size) {
		uint16_t len = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (m[at] == 0x02 && len >= 4 && at + 3 + 4 <= size) {
			result = (uint32_t)(m[at + 3] | m[at + 4] << 8) | (uint32_t)(m[at + 5] | m[at + 6] << 8) << 16;
		}
		at += 3u + len;
	}
	r5q_slim_result = result;
	r5q_slim_msg = (uint16_t)(m[3] | m[4] << 8);
	r5q_slim_seen++;
}

static bool
r5q_slim_ask(const char *what, uint32_t node, uint32_t port, uint16_t msg, const uint8_t *tlvs, uint16_t len)
{
	static uint16_t txn = 1;
	uint8_t out[64];
	uint16_t seen = r5q_slim_seen;

	out[0] = 0;                             /* request */
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	txn++;
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	out[5] = (uint8_t)len;
	out[6] = (uint8_t)(len >> 8);
	memcpy(out + 7, tlvs, len);
	if (qrtr_out_packet(QRTR_DATA, R5Q_SLIM_PORT, node, port, out, 7u + len) != 0) {
		tell("adsp: slim: %s: NOT SENT\n", what);
		return false;
	}
	for (int ms = 0; ms < 1000 && r5q_slim_seen == seen; ms++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (r5q_slim_seen == seen) {
		tell("adsp: slim: %s (%#x): NO ANSWER\n", what, msg);
		return false;
	}
	tell("adsp: slim: %s (%#x): result %u, error %#x%s\n", what, r5q_slim_msg, r5q_slim_result & 0xffff,
	    r5q_slim_result >> 16, (r5q_slim_result & 0xffff) == 0 ? " (done)" : " (REFUSED)");
	return (r5q_slim_result & 0xffff) == 0;
}

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

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
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
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
		printf("adsp: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("adsp: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
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
		printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.to_port == R5Q_SLIM_PORT) {
		r5q_slim_answer(data + at, header.size);
		return;
	}
	printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- APR: the audio services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's audio services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the audio services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static redwood_apr_events_t apr_events; /* the sound's (redwood_sound.c): what nobody waits for */
static redwood_apr_events_t apr_events_second;  /* the microphones' (redwood_mic.c) */
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		printf("adsp: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		printf("adsp: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		redwood_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct redwood_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (apr_answer.there && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
redwood_adsp_apr_events(redwood_apr_events_t handler)
{
	apr_events = handler;
}

void
redwood_adsp_apr_events_second(redwood_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return redwood_adsp_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_ADSP_APPS ------------------------------------------ */

/*
 * On this SoC the audio DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_ADSP_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With adsp=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to bsd/dev/redwood_battery.c (redwood_battery_report()), and from there to the power source
 * that iOS reads (iokit/Kernel/RedwoodBattery.cpp). battmgr=2: every property once, for the log.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("adsp: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("adsp: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, 0 } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	battmgr.runs = true;
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("adsp: battmgr: the DSP has not opened PMIC_RTR_ADSP_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("adsp: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 12; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("adsp: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			redwood_battery_report(&b);
		}
		if (round < 3 || round % 6 == 0 || !good) {
			tell("adsp: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA)\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr);
			if (round % 30 == 0 && good) {
				tell("adsp: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		for (unsigned ms = 0; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(void)
{
	thread_t thread;

	uint32_t wanted = 1;            /* (on with adsp=1 unless battmgr=0) */

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	if (option("battery")) {
		/* (the loader's way, through Qualcomm's UEFI charger drivers: one way to the battery at a time) */
		tell("adsp: battmgr: not with battery=1: the battery is asked about the loader's way\n");
		return;
	}
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	if (!glink_find()) {
		printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
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
		printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"apr_audio_svc\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("adsp_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's core service over APR whether the audio services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("adsp: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("adsp: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_apr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp_keep, adsp=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("adsp: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
redwood_adsp_stop(void)
{
	battmgr_stop();
	redwood_sound_dsp_down();               /* the amplifiers, the stream and the port first, while the DSP still listens */
	redwood_mic_dsp_down();                 /* and what the microphones' path has asked for */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("adsp: the audio DSP is off: it had been up%s (node %u, %u services; audio services %s), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, audio_up ? "up" : "NOT UP", stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
redwood_adsp_quiesce(void)
{
	redwood_sound_quiesce();                /* the amplifiers off, whatever the DSP's state */
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("adsp: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adsp: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("adsp: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("adsp: THE AUDIO DSP IS UP AND LINKED, its audio services up (APR), %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
		if (r5q) {
			thread_t mic_thread;            /* (Test 182: boot-arg mic=N, the microphones' bus; Test 184: a thread of its
			                                 * own -- a hung access there must not keep the speakers from starting) */

			if (kernel_thread_start(r5q_mic_thread, NULL, &mic_thread) == KERN_SUCCESS) {
				thread_deallocate(mic_thread);
			}
			r5q_slpi_start();               /* (Test 201: boot-arg slpi=2, the sensors' DSP's link) */
		}
		redwood_sound_dsp_up();                 /* (boot-arg sound=N: the speakers' path, redwood_sound.c; r5q: Test 141) */
		if (!r5q) {
			redwood_mic_dsp_up();           /* (boot-arg mic=N: the microphones' path, redwood_mic.c) */
			battmgr_start();                /* (how the battery is, from the DSP; battmgr=0: not asked) */
		}
	} else {
		tell("adsp: THE AUDIO DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/*
 * (r5q, Test 185) Stage 2 (mic=2): the NGD's message queues. The bus's BAM is the DSP's (only our pipes are touched:
 * reading its global registers hung, Test 183); as Linux's bam_dma.c in "controlled remotely" mode a pipe is reset, given
 * a descriptor FIFO (8-byte descriptors: address, size, flags) and enabled (system mode; "direction" for the one that
 * writes our memory), and work is handed over by writing how far the FIFO is filled into its event register; how far
 * the BAM has got is in its SW offsets. The BAM's writes come through the apps SMMU under the SLIMbus stream IDs
 * (live DT iommus 0x1b46/8, 0x1b4d/2, 0x1b50/1): given a stream match through a boot bank with translation off, as the
 * Wi-Fi's (redwood_wifi.c r5q_smmu_look). Then the NGD enabled with both queues, as qcom-ngd-ctrl.c's setup.
 */
#define SLIM_PIPE(n)            (0x1000u * (n))
#define BAM_P_RST               0x13004
#define BAM_P_CTRL              0x13000
#define BAM_P_IRQ_EN            0x13018
#define BAM_P_SW_OFSTS          0x13800
#define BAM_P_EVNT_REG          0x13818
#define BAM_P_DESC_FIFO_ADDR    0x1381c
#define BAM_P_FIFO_SIZES        0x13820
#define BAM_P_EN                (1u << 1)
#define BAM_P_DIRECTION         (1u << 3)
#define BAM_P_SYS_MODE          (1u << 5)
#define BAM_DESC_INT            (1u << 15)
#define SLIM_RX_DESCS           32u
#define SLIM_MSG_LEN            40u

static volatile uint32_t *
r5q_dma_area(uint64_t *pa, size_t size)
{
	/*
	 * (Test 187-189) The BAM takes 32-bit addresses, and iOS has no RAM below 4 GB (kalloc: 256 pages all above; a
	 * contiguous allocation limited to below: none). The loader reserves 64 KB there and says where (lowdma_pa=):
	 * handed out from it in turn.
	 */
	static uint64_t lowdma, used;

	if (lowdma == 0 && !PE_parse_boot_argn("lowdma_pa", &lowdma, sizeof(lowdma))) {
		tell("adsp: slim: no lowdma_pa= from the loader: no memory below 4 GB\n");
		return NULL;
	}
	if (lowdma == 0 || used + size > 0x10000 || lowdma + 0x10000 > 0x100000000ull) {
		tell("adsp: slim: the loader's low memory (%#llx) is used up or not below 4 GB\n", lowdma);
		return NULL;
	}
	*pa = lowdma + used;
	used += size;
	{
		volatile uint32_t *m = (volatile uint32_t *)ml_io_map_unmappable(*pa, size, VM_WIMG_WCOMB);   /* (uncached) */

		for (size_t i = 0; m != NULL && i < size / 4; i++) {
			m[i] = 0;               /* (the loader's memory is kept across runs: what an earlier run left, gone) */
		}
		return m;
	}
}

static bool
r5q_slim_smmu(void)
{
	static const uint32_t sids[3][2] = { { 0x1b46, 0x8 }, { 0x1b4d, 0x2 }, { 0x1b50, 0x1 } };
	vm_offset_t s = ml_io_map_unmappable(0x15000000UL, 0x100000, VM_WIMG_IO);
	uint32_t idr0, idr1, n, numcb, page, numpage, done = 0;

	if (s == 0) {
		return false;
	}
	idr0 = *(volatile uint32_t *)(s + 0x20);
	idr1 = *(volatile uint32_t *)(s + 0x24);
	n = idr0 & 0xff;
	numcb = idr1 & 0xff;
	page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	numpage = 1u << (((idr1 >> 28) & 7) + 1);
	for (unsigned k = 0; k < 3; k++) {
		uint32_t sid = sids[k][0], mask = sids[k][1], free_smr = ~0u, have = ~0u, share = ~0u;

		for (uint32_t i = 0; i < n && i < 128; i++) {
			uint32_t smr = *(volatile uint32_t *)(s + 0x800 + 4 * i), s2cr = *(volatile uint32_t *)(s + 0xc00 + 4 * i);
			uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff, cb = s2cr & 0xff;

			if (!(smr & 0x80000000u)) {
				/*
				 * (Test 238) the highest free one below 80: Wi-Fi takes the lowest free SMRs and FastRPC the
				 * highest (Test 231), and this may run while either does
				 */
				if (i < 80) {
					free_smr = i;
				}
				continue;
			}
			if ((sid & ~m) == (id & ~m)) {
				have = i;
			}
			if (share == ~0u && ((s2cr >> 16) & 3) == 0 && cb < numcb &&
			    !(*(volatile uint32_t *)(s + (vm_offset_t)numpage * page + (vm_offset_t)cb * page) & 1) &&
			    *(volatile uint32_t *)(s + page + 4 * cb) == 0x1f000u) {
				share = i;
			}
		}
		if (have != ~0u) {
			tell("adsp: slim: smmu: SID %#x already matched by SMR%u (%#x, S2CR %#x)\n", sid, have,
			    *(volatile uint32_t *)(s + 0x800 + 4 * have), *(volatile uint32_t *)(s + 0xc00 + 4 * have));
			done++;
			continue;
		}
		if (free_smr == ~0u || share == ~0u) {
			tell("adsp: slim: smmu: SID %#x: no free SMR (%#x) or no bank to share (%#x)\n", sid, free_smr, share);
			continue;
		}
		*(volatile uint32_t *)(s + 0xc00 + 4 * free_smr) = *(volatile uint32_t *)(s + 0xc00 + 4 * share);
		*(volatile uint32_t *)(s + 0x800 + 4 * free_smr) = 0x80000000u | mask << 16 | sid;
		__asm__ volatile ("dsb sy" ::: "memory");
		tell("adsp: slim: smmu: SID %#x mask %#x: SMR%u %#x, S2CR %#x (as SMR%u's)\n", sid, mask, free_smr,
		    *(volatile uint32_t *)(s + 0x800 + 4 * free_smr), *(volatile uint32_t *)(s + 0xc00 + 4 * free_smr), share);
		done++;
	}
	return done == 3;
}

/* one message on the tx pipe (EOT only, as downstream), its outcome: the NGD's interrupt status, cleared before and after */
static uint32_t
r5q_slim_send(vm_offset_t bam, vm_offset_t ngd, volatile uint32_t *tx_fifo, volatile uint8_t *tx_buf, uint64_t tx_buf_pa,
    uint32_t *next, const uint8_t *bytes, uint32_t len)
{
	uint32_t slot = (*next)++, stat;
	volatile uint8_t *b = tx_buf + 16 * slot;

	for (uint32_t i = 0; i < 16; i++) {
		b[i] = i < len ? bytes[i] : 0;
	}
	tx_fifo[2 * slot] = (uint32_t)(tx_buf_pa + 16 * slot);
	tx_fifo[2 * slot + 1] = ((len + 3) & ~3u) | (1u << 14) << 16;
	*(volatile uint32_t *)(ngd + 0x18) = 0xffffffffu;
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_EVNT_REG) = (*next) * 8;
	for (int us = 0; us < 200000; us += 50) {
		if (*(volatile uint32_t *)(ngd + 0x14) & 0xbe000000u) {        /* (not RX_MSG_RCVD) */
			break;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	stat = *(volatile uint32_t *)(ngd + 0x14);
	*(volatile uint32_t *)(ngd + 0x18) = stat;
	return stat;
}

/*
 * (r5q, Test 195, mic=5) Downstream's order exactly (LineageOS sm8150 slim-msm-ngd.c ngd_slim_power_up): the codec on
 * first (its clock, its reset); the NGD's interrupts enabled before it is; the rx queue posted as 4-byte words
 * (msm_slim_post_rx_msgq); the NGD enabled; MASTER_CAPABILITY answered at once with REPORT_SATELLITE, and right
 * after, ADDR_QUERY for the codec; everything that comes back written down for 4 s.
 */
static void
r5q_slim_status(const char *what, vm_offset_t ngd)
{
	uint32_t st = *(volatile uint32_t *)(ngd + 0x4), in = *(volatile uint32_t *)(ngd + 0x14);

	tell("adsp: slim: v6: %s: status %#x (laddr %u, acked mc %#x, nacked mc %#x, error %u), int %#x\n", what, st,
	    (st >> 1) & 1, (st >> 13) & 0x7f, (st >> 20) & 0x7f, (st >> 11) & 3, in);
}

/* (Test 196, mic=6) a message through the NGD's own registers (TX_MSG at +0x30), the tx queue off */
static uint32_t
r5q_slim_send_ahb(vm_offset_t ngd, const uint8_t *bytes, uint32_t len)
{
	uint32_t stat;

	*(volatile uint32_t *)(ngd + 0x18) = 0xffffffffu;
	for (uint32_t i = 0; i < (len + 3) / 4; i++) {
		uint32_t w = 0;

		for (uint32_t k = 0; k < 4 && 4 * i + k < len; k++) {
			w |= (uint32_t)bytes[4 * i + k] << (8 * k);
		}
		*(volatile uint32_t *)(ngd + 0x30 + 4 * i) = w;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int us = 0; us < 200000; us += 50) {
		if (*(volatile uint32_t *)(ngd + 0x14) & 0xbe000000u) {
			break;
		}
		delay_for_interval(50, NSEC_PER_USEC);
	}
	stat = *(volatile uint32_t *)(ngd + 0x14);
	*(volatile uint32_t *)(ngd + 0x18) = stat & ~0x40000000u;
	return stat;
}

/*
 * (Test 239, mic>=7) The PM8150's clock buffers (peripherals 0x50-0x5f of slave 0; LN_BB_CLK2, the codec's MCLK, is one
 * of them): type, subtype, status and enable registers, read only, through the SPMI observer channel -- does our RPMh
 * vote for "lnbclka2" really turn the codec's clock on?
 */
static void
r5q_clk_dump(const char *when)
{
	extern bool redwood_spmi_peek(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count);

	for (uint16_t per = 0x50; per <= 0x5f; per++) {
		uint8_t t[2] = { 0 }, st[8] = { 0 }, en[2] = { 0 };

		if (!redwood_spmi_peek(per, 0x04, t, 2)) {
			continue;
		}
		(void)redwood_spmi_peek(per, 0x08, st, 8);
		(void)redwood_spmi_peek(per, 0x46, en, 2);
		tell("adsp: slim: clk %s: PM8150 %#x00: type %#x subtype %#x, status %02x %02x %02x %02x, 0x46: %02x %02x\n", when, per,
		    t[0], t[1], st[0], st[1], st[2], st[3], en[0], en[1]);
	}
}

/*
 * (Test 249, mic>=7) The codec's supplies, read only: PM8150 S4 (1.8 V: cdc-vdd-buck, buck-sido, tx-h, rx-h, vddpx-1;
 * a "stub" regulator in the DT, i.e. the AOP's, never switched by Android) and PM8150L BOB (3.3 V: cdc-vdd-3v3, RPMh
 * "bobc1"). PM8150's switchers S1..S5 on slave 1 at 0x14/0x17/0x1a/0x1d/0x20 (S4 = 0x1d), BOB on slave 5 at 0xa0:
 * type/subtype (0x04), status 1/2 (0x08: bit 7 "ready"), the voltage set point (0x40/0x41, mV), mode (0x45) and
 * enable (0x46: bit 7). Has the codec's power really been there all along?
 */
static void
r5q_supply_dump(void)
{
	extern bool redwood_spmi_peek(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count);
	static const struct {
		uint16_t id;
		const char *name;
	} regs[] = {
		{ 0x114, "PM8150 S1" }, { 0x117, "PM8150 S2" }, { 0x11a, "PM8150 S3" }, { 0x11d, "PM8150 S4 (the codec's 1.8 V)" },
		{ 0x120, "PM8150 S5" }, { 0x5a0, "PM8150L BOB (the codec's 3.3 V)" },
	};

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t t[2] = { 0 }, st[2] = { 0 }, v[2] = { 0 }, me[2] = { 0 };

		if (!redwood_spmi_peek(regs[i].id, 0x04, t, 2)) {
			tell("adsp: slim: supply %s (%#x): no channel / no answer\n", regs[i].name, regs[i].id);
			continue;
		}
		(void)redwood_spmi_peek(regs[i].id, 0x08, st, 2);
		(void)redwood_spmi_peek(regs[i].id, 0x40, v, 2);
		(void)redwood_spmi_peek(regs[i].id, 0x45, me, 2);
		tell("adsp: slim: supply %s (%#x): type %#x subtype %#x, status %02x %02x (ready %u), set %u mV, mode %02x, "
		    "enable %02x (%s)\n", regs[i].name, regs[i].id, t[0], t[1], st[0], st[1], st[0] >> 7, v[0] | (v[1] << 8), me[0],
		    me[1], (me[1] & 0x80) ? "on" : "off by this register");
	}
}

static void
r5q_slim_v5(vm_offset_t slim, uint32_t mic)
{
	extern bool redwood_r5q_rpmh_enable(const char *id);
	static const uint8_t report[] = { 0xc7, 0x01, 0xff, 0xd9, 0xc5, 0x01, 0x01 };
	static const uint8_t query[] = { 0xaa, 0x0d, 0xff, 0x51, 0x00, 0x01, 0x50, 0x02, 0x17, 0x02, 0x00 };
	vm_offset_t bam = ml_io_map_unmappable(0x17184000UL, 0x2c000, VM_WIMG_IO), ngd = slim + 0x1000;
	volatile uint32_t *rx_fifo, *tx_fifo, *rx_words, *tx_buf, *gpio;
	uint64_t rx_fifo_pa, tx_fifo_pa, rx_words_pa, tx_buf_pa;
	uint32_t words = 0, next = 0, msg[12], have = 0, need = 0, nmsg = 0;
	bool reported = false;

	if (bam == 0 || !r5q_slim_smmu()) {
		return;
	}
	rx_fifo = r5q_dma_area(&rx_fifo_pa, 0x4000);
	tx_fifo = r5q_dma_area(&tx_fifo_pa, 0x4000);
	rx_words = r5q_dma_area(&rx_words_pa, 0x4000);
	tx_buf = r5q_dma_area(&tx_buf_pa, 0x4000);
	gpio = (volatile uint32_t *)ml_io_map(0x03d00000UL + 0x1000 * 143, 0x1000);
	if (rx_fifo == NULL || tx_fifo == NULL || rx_words == NULL || tx_buf == NULL || gpio == NULL) {
		tell("adsp: slim: v5: no memory\n");
		return;
	}
	if (mic >= 7) {
		r5q_supply_dump();
		r5q_clk_dump("before the vote");
	}
	tell("adsp: slim: v5: the codec's clock %s\n", redwood_r5q_rpmh_enable("lnbclka2") ? "on" : "NOT ON");
	if (mic >= 7) {
		delay_for_interval(20, NSEC_PER_MSEC);
		r5q_clk_dump("after the vote");
	}
	gpio[1] = 0;
	gpio[0] = (3u << 6) | 1u | (1u << 9);
	delay_for_interval(20, NSEC_PER_MSEC);
	if (mic < 7) {
		gpio[1] = 2;
	} else {
		tell("adsp: slim: v7: the codec held in reset until the satellite handshake is done (as Android)\n");
	}
	delay_for_interval(100, NSEC_PER_MSEC);
	for (uint32_t pipe = 3; pipe <= 4; pipe++) {
		vm_offset_t pp = bam + SLIM_PIPE(pipe);

		*(volatile uint32_t *)(pp + BAM_P_RST) = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(pp + BAM_P_RST) = 0;
		*(volatile uint32_t *)(pp + BAM_P_DESC_FIFO_ADDR) = (uint32_t)(pipe == 3 ? rx_fifo_pa : tx_fifo_pa);
		*(volatile uint32_t *)(pp + BAM_P_FIFO_SIZES) = 0x4000;
		*(volatile uint32_t *)(pp + BAM_P_IRQ_EN) = 0;
		*(volatile uint32_t *)(pp + BAM_P_CTRL) = BAM_P_EN | BAM_P_SYS_MODE | (pipe == 3 ? BAM_P_DIRECTION : 0);
	}
	for (uint32_t i = 0; i < 256; i++) {
		rx_fifo[2 * i] = (uint32_t)(rx_words_pa + 4 * i);
		rx_fifo[2 * i + 1] = 4u;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_EVNT_REG) = 256 * 8;
	*(volatile uint32_t *)(ngd + 0x10) = 0xbe000000u;      /* NACKED_2, BUF_CONTE, TX_INVAL, IE_VE_CHG, DEV_ERR, TX_SENT */
	*(volatile uint32_t *)(ngd + 0x8) = *(volatile uint32_t *)(ngd + 0x8) | 0x10000;
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(ngd + 0x0) = 0x7;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int ms = 0; ms < 5000; ms++) {
		uint32_t at = (*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS) & 0xffff) / 8;

		while (words < at && words < 256) {
			uint32_t w = rx_words[words++];

			if (have == 0) {
				need = ((w & 0x1f) + 1 + 3) / 4;        /* the length field is the message less one byte */
			}
			if (have < 12) {
				msg[have] = w;
			}
			if (++have < need) {
				continue;
			}
			nmsg++;
			tell("adsp: slim: v5: +%d ms: message %u (%u words): %08x %08x %08x %08x\n", ms, nmsg, need, msg[0],
			    need > 1 ? msg[1] : 0, need > 2 ? msg[2] : 0, need > 3 ? msg[3] : 0);
			if (((msg[0] >> 8) & 0xff) == 0x00 && ((msg[0] >> 5) & 7) == 6 && !reported) {
				uint32_t s1 = r5q_slim_send(bam, ngd, tx_fifo, (volatile uint8_t *)tx_buf, tx_buf_pa, &next, report, 7);
				uint32_t s2;

				reported = true;
				if (mic >= 7) {
					/*
					 * (Test 238, mic=7) Android's order (saved Android boot log: wcd9xxx_slim_probe at 10.889 s,
					 * the codec's logical address 0xcf at 10.960 s): the NGD a satellite first, THEN the codec's
					 * reset (wcd9xxx_reset: low 20 ms, high 20 ms), THEN ADDR_QUERY again and again until the
					 * manager knows the codec (wcd9xxx_slim_get_laddr retries every 1 ms). Here every 10 ms for 3 s.
					 */
					uint8_t q[11];
					uint32_t s = 0, tries;

					tell("adsp: slim: v7: REPORT_SATELLITE: %#x; the codec's reset now\n", s1);
					delay_for_interval(10, NSEC_PER_MSEC);
					gpio[1] = 0;
					delay_for_interval(20, NSEC_PER_MSEC);
					gpio[1] = 2;
					delay_for_interval(20, NSEC_PER_MSEC);
					tell("adsp: slim: v7: GPIO143 cfg %#x io %#x (out of reset)\n", gpio[0], gpio[1]);
					memcpy(q, query, sizeof(q));
					for (tries = 1; tries <= 300; tries++) {
						q[3] = (uint8_t)(0x60 + tries);
						s = r5q_slim_send(bam, ngd, tx_fifo, (volatile uint8_t *)tx_buf, tx_buf_pa, &next, q, 10);
						if ((s & 0x08000000u) == 0) {
							break;
						}
						if (tries % 50 == 0) {
							r5q_slim_status("v7: still refused", ngd);
						}
						delay_for_interval(10, NSEC_PER_MSEC);
					}
					tell("adsp: slim: v7: ADDR_QUERY (codec) %s after %u tries (%u ms): %#x\n",
					    (s & 0x08000000u) ? "STILL REFUSED" : "TAKEN", tries, tries * 10, s);
					r5q_slim_status("v7: after the queries", ngd);
					have = 0;
					continue;
				}
				delay_for_interval(5, NSEC_PER_MSEC);
				s2 = r5q_slim_send(bam, ngd, tx_fifo, (volatile uint8_t *)tx_buf, tx_buf_pa, &next, query, 10);
				tell("adsp: slim: v5: REPORT_SATELLITE: %#x%s; ADDR_QUERY: %#x%s\n", s1, (s1 & 0x08000000u) ? " INVALID" :
				    (s1 & 0x80000000u) ? " sent" : "", s2, (s2 & 0x08000000u) ? " INVALID" : (s2 & 0x80000000u) ? " sent" : "");
				if (mic >= 6) {
					static const uint8_t ifd[] = { 0xaa, 0x0d, 0xff, 0x52, 0x00, 0x00, 0x50, 0x02, 0x17, 0x02, 0x00 };
					uint32_t cfg = *(volatile uint32_t *)(ngd + 0x0);

					r5q_slim_status("after the queued query", ngd);
					delay_for_interval(50, NSEC_PER_MSEC);
					r5q_slim_status("50 ms later", ngd);
					*(volatile uint32_t *)(ngd + 0x0) = cfg & ~4u;          /* the tx queue off: registers */
					__asm__ volatile ("dsb sy" ::: "memory");
					delay_for_interval(1, NSEC_PER_MSEC);
					s2 = r5q_slim_send_ahb(ngd, query, 11);
					tell("adsp: slim: v6: ADDR_QUERY (codec) by registers: %#x\n", s2);
					r5q_slim_status("after it", ngd);
					delay_for_interval(20, NSEC_PER_MSEC);
					s2 = r5q_slim_send_ahb(ngd, ifd, 11);
					tell("adsp: slim: v6: ADDR_QUERY (interface device) by registers: %#x\n", s2);
					r5q_slim_status("after it", ngd);
					delay_for_interval(20, NSEC_PER_MSEC);
					s2 = r5q_slim_send_ahb(ngd, report, 7);
					tell("adsp: slim: v6: REPORT_SATELLITE by registers: %#x\n", s2);
					r5q_slim_status("after it", ngd);
					*(volatile uint32_t *)(ngd + 0x0) = cfg;
					__asm__ volatile ("dsb sy" ::: "memory");
				}
			}
			have = 0;
		}
		if (mic >= 6 && ms % 1000 == 999) {
			tell("adsp: slim: v6: +%d ms: rx offset %#x, rx words %u\n", ms,
			    *(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS), words);
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	tell("adsp: slim: v5: done: NGD cfg %#x status %#x int %#x; %u messages; rx words %u, tx %u\n",
	    *(volatile uint32_t *)(ngd + 0x0), *(volatile uint32_t *)(ngd + 0x4), *(volatile uint32_t *)(ngd + 0x14), nmsg, words,
	    next);
}

static void
r5q_slim_stage2(vm_offset_t slim)
{
	vm_offset_t bam = ml_io_map_unmappable(0x17184000UL, 0x2c000, VM_WIMG_IO), ngd = slim + 0x1000;
	volatile uint32_t *rx_fifo, *tx_fifo, *rx_bufs;
	uint64_t rx_fifo_pa, tx_fifo_pa, rx_bufs_pa;
	uint32_t seen = 0;

	tell("adsp: slim: (Test 186) next: the SMMU's stream matches\n");
	delay_for_interval(3000, NSEC_PER_MSEC);
	if (bam == 0 || !r5q_slim_smmu()) {
		tell("adsp: slim: stage 2 not done (no BAM mapping or no SMMU match)\n");
		return;
	}
	rx_fifo = r5q_dma_area(&rx_fifo_pa, PAGE_SIZE);
	tx_fifo = r5q_dma_area(&tx_fifo_pa, PAGE_SIZE);
	rx_bufs = r5q_dma_area(&rx_bufs_pa, PAGE_SIZE);
	if (rx_fifo == NULL || tx_fifo == NULL || rx_bufs == NULL) {
		tell("adsp: slim: no memory for the queues\n");
		return;
	}
	tell("adsp: slim: (Test 186) next: the BAM's pipes 3 and 4 reset and set\n");
	delay_for_interval(3000, NSEC_PER_MSEC);
	tell("adsp: slim: (Test 187) memory: rx FIFO %#llx, tx FIFO %#llx, rx buffers %#llx\n", rx_fifo_pa, tx_fifo_pa, rx_bufs_pa);
	for (uint32_t pipe = 3; pipe <= 4; pipe++) {
		vm_offset_t pp = bam + SLIM_PIPE(pipe);
		const struct {
			const char *name;
			uint32_t reg, value;
		} writes[] = {
			{ "reset", BAM_P_RST, 1 }, { "reset off", BAM_P_RST, 0 },
			{ "descriptor FIFO", BAM_P_DESC_FIFO_ADDR, (uint32_t)(pipe == 3 ? rx_fifo_pa : tx_fifo_pa) },
			{ "FIFO size", BAM_P_FIFO_SIZES, PAGE_SIZE > 0x8000 ? 0x8000 : PAGE_SIZE },
			{ "interrupts off", BAM_P_IRQ_EN, 0 },
			{ "enable", BAM_P_CTRL, BAM_P_EN | BAM_P_SYS_MODE | (pipe == 3 ? BAM_P_DIRECTION : 0) },
		};

		/* (Test 187) one write at a time, each said first and waited for: Test 185/186 reset the phone in here */
		for (unsigned w = 0; w < sizeof(writes) / sizeof(writes[0]); w++) {
			tell("adsp: slim: BAM pipe %u: next %s (%#x <- %#x)\n", pipe, writes[w].name, writes[w].reg, writes[w].value);
			delay_for_interval(2000, NSEC_PER_MSEC);
			*(volatile uint32_t *)(pp + writes[w].reg) = writes[w].value;
			__asm__ volatile ("dsb sy" ::: "memory");
		}
	}
	tell("adsp: slim: BAM pipes written; next: the rx descriptors handed over\n");
	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uint32_t i = 0; i < SLIM_RX_DESCS; i++) {
		rx_fifo[2 * i] = (uint32_t)(rx_bufs_pa + SLIM_MSG_LEN * i);
		rx_fifo[2 * i + 1] = SLIM_MSG_LEN | BAM_DESC_INT << 16;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_EVNT_REG) = SLIM_RX_DESCS * 8;
	tell("adsp: slim: BAM pipes set: rx 3 (FIFO %#llx, %u buffers of %u bytes at %#llx), tx 4 (FIFO %#llx); ctrl %#x/%#x\n",
	    rx_fifo_pa, SLIM_RX_DESCS, SLIM_MSG_LEN, rx_bufs_pa, tx_fifo_pa,
	    *(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_CTRL), *(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_CTRL));
	tell("adsp: slim: (Test 186) next: the NGD enabled with its queues\n");
	delay_for_interval(3000, NSEC_PER_MSEC);
	/* qcom-ngd-ctrl.c: the queue's timeout, then the NGD with both queues */
	*(volatile uint32_t *)(ngd + 0x8) = 0x10000;
	*(volatile uint32_t *)(ngd + 0x0) = *(volatile uint32_t *)(ngd + 0x0) | 0x7;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int ms = 0; ms < 3000; ms += 10) {
		uint32_t at = (*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS) & 0xffff) / 8;

		while (seen < at && seen < SLIM_RX_DESCS) {
			volatile const uint32_t *m = (volatile const uint32_t *)((volatile const uint8_t *)rx_bufs + SLIM_MSG_LEN * seen);

			tell("adsp: slim: +%d ms: message %u: %08x %08x %08x %08x %08x\n", ms, seen, m[0], m[1], m[2], m[3], m[4]);
			seen++;
		}
		if (*(volatile uint32_t *)(ngd + 0x4) & 2) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	tell("adsp: slim: the NGD: cfg %#x status %#x (logical address %s), int stat %#x; rx offset %#x; %u messages\n",
	    *(volatile uint32_t *)(ngd + 0x0), *(volatile uint32_t *)(ngd + 0x4),
	    (*(volatile uint32_t *)(ngd + 0x4) & 2) ? "YES" : "no", *(volatile uint32_t *)(ngd + 0x14),
	    *(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS), seen);
	{
		/*
		 * (Test 190, mic=3) The manager's MASTER_CAPABILITY answered as qcom-ngd-ctrl.c's master worker does:
		 * REPORT_SATELLITE (mt 6 "source referred user", mc 1) to the manager (logical address 0xff), 8 bytes: the
		 * header word, then the magic 0xc5d9, version 1, protocol 1. Sent from the loader's low memory on pipe 4, one
		 * descriptor (end of transfer and interrupt, as the BAM driver's DMA_PREP_INTERRUPT), then what comes back.
		 */
		uint32_t mic = 0, sent;
		volatile uint32_t *tx_buf;
		uint64_t tx_buf_pa;

		PE_parse_boot_argn("mic", &mic, sizeof(mic));
		if (mic < 3 || (tx_buf = r5q_dma_area(&tx_buf_pa, 0x4000)) == NULL) {
			return;
		}
		tx_buf[0] = 0xd9ff01c7u;                /* rl 7 (8 less one: "HW expects length field to be excluded"), mt 6, mc 1, to 0xff; 0xd9 */
		tx_buf[1] = 0x000101c5u;                /* 0xc5, version 1, protocol 1 */
		tx_fifo[0] = (uint32_t)tx_buf_pa;
		tx_fifo[1] = 8u | (BAM_DESC_INT | (1u << 14)) << 16;
		__asm__ volatile ("dsb sy" ::: "memory");
		tell("adsp: slim: (Test 190) REPORT_SATELLITE to the manager: %08x %08x\n", tx_buf[0], tx_buf[1]);
		*(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_EVNT_REG) = 8;
		for (int ms = 0; ms < 5000; ms += 10) {
			uint32_t at = (*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS) & 0xffff) / 8;

			while (seen < at && seen < SLIM_RX_DESCS) {
				volatile const uint32_t *m = (volatile const uint32_t *)((volatile const uint8_t *)rx_bufs + SLIM_MSG_LEN * seen);

				tell("adsp: slim: +%d ms after it: message %u: %08x %08x %08x %08x %08x\n", ms, seen, m[0], m[1], m[2], m[3],
				    m[4]);
				seen++;
			}
			delay_for_interval(10, NSEC_PER_MSEC);
		}
		sent = *(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_SW_OFSTS);
		tell("adsp: slim: after 5 s: tx offset %#x (%s), NGD status %#x int stat %#x; %u messages in all\n", sent,
		    (sent & 0xffff) == 8 ? "SENT" : "not sent", *(volatile uint32_t *)(ngd + 0x4), *(volatile uint32_t *)(ngd + 0x14),
		    seen);
		if (mic < 4) {
			return;
		}
		/*
		 * (Test 191, mic=4) The WCD9340 out of reset, then asked for on the bus. Its 1.8 V supplies are PM8150 S4 (always
		 * on), its 3.3 V PM8150L BOB (on from the start); its clock LN_BB_CLK2 ("lnbclka2", RPMh VRM: enabled here); its
		 * reset GPIO143 (south tile 0x03d00000, "cdc_reset_active": gpio, 8 mA, pull down, out high). Then
		 * ADDR_QUERY (mt 5 "destination referred user", mc 0xd, to the manager) for its two devices -- 00 01 50 02 17 02
		 * (the codec) and 00 00 50 02 17 02 (its interface) -- every 200 ms for 4 s, the answers (ADDR_REPLY, mc 0xe:
		 * the address back and its logical address) in the rx queue.
		 */
		{
			extern bool redwood_r5q_rpmh_enable(const char *id);
			volatile uint32_t *gpio = (volatile uint32_t *)ml_io_map(0x03d00000UL + 0x1000 * 143, 0x1000);
			static const uint8_t ea[2][6] = { { 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, { 0x00, 0x00, 0x50, 0x02, 0x17, 0x02 } };
			uint32_t tx_at = 16, tid = 1, next_desc = 1;    /* (Test 192: descriptors one after another, buffers 16 apart) */

			tell("adsp: slim: (Test 191) the codec's clock (LN_BB_CLK2): %s\n",
			    redwood_r5q_rpmh_enable("lnbclka2") ? "enabled" : "NOT ENABLED");
			if (gpio == NULL) {
				return;
			}
			tell("adsp: slim: GPIO143 (the codec's reset) as found: cfg %#x io %#x\n", gpio[0], gpio[1]);
			gpio[1] = 0;                                    /* low: in reset */
			gpio[0] = (3u << 6) | 1u | (1u << 9);           /* gpio, 8 mA, pull down, output */
			delay_for_interval(20, NSEC_PER_MSEC);
			gpio[1] = 2;                                    /* high: out of reset */
			delay_for_interval(100, NSEC_PER_MSEC);
			tell("adsp: slim: GPIO143 now: cfg %#x io %#x (the codec out of reset)\n", gpio[0], gpio[1]);
			/*
			 * (Test 194) What the NGD calls an invalid message: variants, each sent alone and its verdict read -- the
			 * query as before (A), with the descriptor flagged "end of transfer" only as downstream's sps_transfer_one
			 * (B), with the exact length 11 (C), REPORT_SATELLITE again as a control (D), after the NGD's interrupts are
			 * enabled as downstream's mask (E), to the codec's interface device (F).
			 */
			{
				static const struct {
					const char *name;
					uint8_t bytes[12], len, flags_int;
					bool irq_en;
				} v[] = {
					{ "A query", { 0xaa, 0x0d, 0xff, 0x41, 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, 12, 1, false },
					{ "B query, EOT only", { 0xaa, 0x0d, 0xff, 0x42, 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, 12, 0, false },
					{ "C query, 11 bytes", { 0xaa, 0x0d, 0xff, 0x43, 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, 11, 0, false },
					{ "D satellite report", { 0xc7, 0x01, 0xff, 0xd9, 0xc5, 0x01, 0x01 }, 8, 0, false },
					{ "E query, interrupts on", { 0xaa, 0x0d, 0xff, 0x44, 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, 12, 0, true },
					{ "F query, interface", { 0xaa, 0x0d, 0xff, 0x45, 0x00, 0x00, 0x50, 0x02, 0x17, 0x02 }, 12, 0, true },
				};

				for (unsigned k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
					volatile uint8_t *b = (volatile uint8_t *)tx_buf + tx_at;
					uint32_t slot = next_desc++, stat;

					for (unsigned i = 0; i < 12; i++) {
						b[i] = v[k].bytes[i];
					}
					if (v[k].irq_en) {
						*(volatile uint32_t *)(ngd + 0x10) = 0xfe000000u | (1u << 24);  /* DEF_NGD_INT_MASK + RECFG */
					}
					tx_fifo[2 * slot] = (uint32_t)(tx_buf_pa + tx_at);
					tx_fifo[2 * slot + 1] = v[k].len | ((v[k].flags_int ? BAM_DESC_INT : 0) | (1u << 14)) << 16;
					tx_at += 16;
					*(volatile uint32_t *)(ngd + 0x18) = *(volatile uint32_t *)(ngd + 0x14);
					__asm__ volatile ("dsb sy" ::: "memory");
					*(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_EVNT_REG) = slot * 8 + 8;
					for (int us = 0; us < 200000; us += 100) {
						if (*(volatile uint32_t *)(ngd + 0x14) & 0xfe000000u) {
							break;
						}
						delay_for_interval(100, NSEC_PER_USEC);
					}
					delay_for_interval(300, NSEC_PER_MSEC);
					stat = *(volatile uint32_t *)(ngd + 0x14);
					tell("adsp: slim: (Test 194) %s: NGD int stat %#x%s%s%s; tx offset %#x\n", v[k].name, stat,
					    (stat & 0x80000000u) ? " sent" : "", (stat & 0x08000000u) ? " INVALID" : "",
					    (stat & 0x02000000u) ? " NACKED" : "", *(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_SW_OFSTS));
					{
						uint32_t at = (*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS) & 0xffff) / 8;

						while (seen < at && seen < SLIM_RX_DESCS) {
							volatile const uint32_t *m = (volatile const uint32_t *)((volatile const uint8_t *)rx_bufs +
							    SLIM_MSG_LEN * seen);

							tell("adsp: slim: (Test 194) message %u: %08x %08x %08x %08x\n", seen, m[0], m[1], m[2], m[3]);
							seen++;
						}
					}
				}
				return;
			}
			for (int round = 0; round < 20; round++) {
				for (unsigned d = 0; d < 2; d++) {
					volatile uint8_t *b = (volatile uint8_t *)tx_buf + tx_at;
					uint32_t slot = next_desc++;

					if (tx_at + 16 > 0x4000) {
						break;
					}
					/* header: rl 10 (11 less one), mt 5, mc 0xd, logical, to 0xff; then tid, then the address */
					b[0] = 0xaa;
					b[1] = 0x0d;
					b[2] = 0xff;
					b[3] = (uint8_t)tid++;
					for (unsigned k = 0; k < 6; k++) {
						b[4 + k] = ea[d][k];
					}
					tx_fifo[2 * slot] = (uint32_t)(tx_buf_pa + tx_at);
					tx_fifo[2 * slot + 1] = 12u | (BAM_DESC_INT | (1u << 14)) << 16;
					tx_at += 16;
					/*
					 * (Test 193) one message at a time, as qcom-ngd-ctrl.c (tx_lock, wait for "sent"): the NGD's interrupt
					 * status cleared before (what an earlier message left) and its outcome read after, then cleared again
					 */
					*(volatile uint32_t *)(ngd + 0x18) = *(volatile uint32_t *)(ngd + 0x14);
					__asm__ volatile ("dsb sy" ::: "memory");
					*(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_EVNT_REG) = slot * 8 + 8;
					for (int us = 0; us < 100000; us += 100) {
						if (*(volatile uint32_t *)(ngd + 0x14) & 0xfe000000u) {
							break;
						}
						delay_for_interval(100, NSEC_PER_USEC);
					}
					if (round < 2 || (*(volatile uint32_t *)(ngd + 0x14) & 0x0e000000u)) {
						tell("adsp: slim: round %d query %u (tid %u): NGD int stat %#x%s\n", round, d, (unsigned)b[3],
						    *(volatile uint32_t *)(ngd + 0x14), (*(volatile uint32_t *)(ngd + 0x14) & 0x08000000u) ?
						    " INVALID" : (*(volatile uint32_t *)(ngd + 0x14) & 0x02000000u) ? " NACKED" : "");
					}
					*(volatile uint32_t *)(ngd + 0x18) = *(volatile uint32_t *)(ngd + 0x14);
				}
				for (int ms = 0; ms < 200; ms += 10) {
					uint32_t at = (*(volatile uint32_t *)(bam + SLIM_PIPE(3) + BAM_P_SW_OFSTS) & 0xffff) / 8;

					while (seen < at && seen < SLIM_RX_DESCS) {
						volatile const uint32_t *m = (volatile const uint32_t *)((volatile const uint8_t *)rx_bufs +
						    SLIM_MSG_LEN * seen);

						tell("adsp: slim: round %d: message %u: %08x %08x %08x %08x\n", round, seen, m[0], m[1], m[2], m[3]);
						seen++;
					}
					delay_for_interval(10, NSEC_PER_MSEC);
				}
			}
			tell("adsp: slim: queries done: tx offset %#x, NGD int stat %#x, %u messages in all\n",
			    *(volatile uint32_t *)(bam + SLIM_PIPE(4) + BAM_P_SW_OFSTS), *(volatile uint32_t *)(ngd + 0x14), seen);
		}
	}
}

static void
r5q_mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	r5q_mic_start();
	thread_terminate(current_thread());
}

/* (r5q, Test 182) stage 1 of the microphones: the SLIMbus controller (NGD) powered, and read */
static void
r5q_mic_start(void)
{
	static const uint8_t select[] = { 0x01, 4, 0, 0, 0, 0, 0, 0x10, 4, 0, 2, 0, 0, 0 };   /* instance 0; the DSP is master */
	static const uint8_t power[] = { 0x01, 4, 0, 2, 0, 0, 0 };                          /* active */
	uint32_t mic = 0, node = 0, port = 0;
	vm_offset_t slim;

	if (!PE_parse_boot_argn("mic", &mic, sizeof(mic)) || mic == 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == 0x301) {
			node = qrtr_servers[i].node;
			port = qrtr_servers[i].port;
		}
	}
	if (port == 0) {
		tell("adsp: slim: the DSP has no SLIMbus service (0x301): no microphones\n");
		return;
	}
	tell("adsp: slim: the DSP's SLIMbus service at node %u port %u (mic=%u)\n", node, port, mic);
	/* (Test 182: answered 0x5a, "incompatible state", the first time: the power asked for all the same) */
	(void)r5q_slim_ask("select instance 0, the DSP the master", node, port, 0x20, select, sizeof(select));
	if (!r5q_slim_ask("power: active", node, port, 0x21, power, sizeof(power))) {
		static const uint8_t framer[] = { 0 };

		(void)r5q_slim_ask("the framer's state", node, port, 0x22, framer, 0);
		return;
	}
	if (mic >= 7) {
		/* (Test 240) the framer's state asked for after power too: is the DSP's framer the bus's active one? */
		static const uint8_t framer[] = { 0 };

		(void)r5q_slim_ask("v7: the framer's state", node, port, 0x22, framer, 0);
	}
	slim = ml_io_map(0x171c0000UL, 0x3000);
	if (slim == 0) {
		return;
	}
	tell("adsp: slim: controller version %#x; the NGD (0x171c1000): cfg %#x status %#x (logical address %s) rx msgq cfg %#x "
	    "int en %#x stat %#x\n", *(volatile uint32_t *)slim >> 16, *(volatile uint32_t *)(slim + 0x1000),
	    *(volatile uint32_t *)(slim + 0x1004), (*(volatile uint32_t *)(slim + 0x1004) & 2) ? "yes" : "no",
	    *(volatile uint32_t *)(slim + 0x1008), *(volatile uint32_t *)(slim + 0x1010), *(volatile uint32_t *)(slim + 0x1014));
	/*
	 * (Test 183) The NGD's message queues are DMA through the bus's BAM (0x17184000, v1.7 registers as Linux's bam_dma.c;
	 * upstream SDM845: controlled remotely, EE 1, the NGD's "rx" pipe 3 and "tx" pipe 4). A look, reading only.
	 */
	{
		vm_offset_t bam = ml_io_map_unmappable(0x17184000UL, 0x2c000, VM_WIMG_IO);

		if (bam != 0) {
			/* (Test 183: reading the BAM's global registers hung this thread: they are the DSP's; only our two pipes now) */
			for (uint32_t pipe = 3; pipe <= 4; pipe++) {
				vm_offset_t pp = bam + 0x1000 * pipe;

				tell("adsp: slim: BAM pipe %u: ctrl %#x halt %#x irq en %#x; sw ofsts %#x evnt %#x desc fifo %#x sizes %#x "
				    "data fifo %#x\n", pipe, *(volatile uint32_t *)(pp + 0x13000), *(volatile uint32_t *)(pp + 0x13008),
				    *(volatile uint32_t *)(pp + 0x13018), *(volatile uint32_t *)(pp + 0x13800),
				    *(volatile uint32_t *)(pp + 0x13818), *(volatile uint32_t *)(pp + 0x1381c),
				    *(volatile uint32_t *)(pp + 0x13820), *(volatile uint32_t *)(pp + 0x13824));
			}
		}
	}
	if (mic >= 5) {
		r5q_slim_v5(slim, mic);
	} else if (mic >= 2) {
		r5q_slim_stage2(slim);
	}
}

/* Called while the system starts (bsd/dev/redwood_input.c), after the Wi-Fi's start has been set going. */
void
redwood_adsp_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("adsp_step", &last_step, sizeof(last_step));
	if (redwood_platform_is("r5q")) {
		if (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 2) {
			return;                 /* (adsp=2: the loader has started it; anything else: not here) */
		}
		r5q = true;
		smem_phys = 0x86000000UL;
		on = 1;
	}
	if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1) && !r5q) {
		return;
	}
	if (!r5q && pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
		printf("adsp: adsp=1 and adsp_step=N are for the phone's audio DSP, and this is not the phone\n");
		return;
	}
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("adsp: the audio DSP is being brought up beside the system's start (%s)\n", r5q ? "r5q, adsp=2" : "adsp=1");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("adsp_keep");
	new_from = last_step;
	PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
	printf("adsp: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("adsp_return")) {
		printf("adsp: back to the loader (adsp_return)\n");
		loader_return();
	}
	printf("adsp: the system goes on starting\n");
}
