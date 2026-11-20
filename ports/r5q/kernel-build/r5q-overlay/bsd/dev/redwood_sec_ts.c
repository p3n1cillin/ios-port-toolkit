/* Modified for the Galaxy S10 Lite (r5q) native iOS port, November 2026, from the donor G986N-iOS (commit 946de0fed8e5, a port of HackiOS): Apple Public Source License 2.0, see LICENSE and MODIFICATIONS.md in this directory. */
/*
 * The touch screen of the Samsung Galaxy S20+ 5G (y2q): a Samsung LSI controller of the kind
 * Samsung's kernels call "sec_ts" (firmware tsp_sec/y79a_y2.bin), at address 0x48 on the I2C bus
 * of the SoC's serial engine 5 of the first QUP wrapper, as a source of touches for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg touchscreen=1, and only on the
 * platform the loader calls "y2q": every address below belongs to this phone.
 *
 * What the chip needs, from the phone's own device tree (i2c@994000/touchscreen@48):
 *   power    its two supplies, LDO2 (1.8 V) and LDO3 (3.0 V) of the S2DOS05 display PMIC, are
 *            "boot on" for Android. Whether they still are after our UEFI is what step 2 reads:
 *            the PMIC is at 0x60 on a bus of two GPIOs (62 SDA, 63 SCL, south tile, pulled up on
 *            the board), driven by hand. The same PMIC makes the panel's supplies, so nothing is
 *            written to it but the register number a read starts with
 *   clocks   the serial engine's clock and the wrapper's four (two AHB ports, core, core 2x),
 *            by vote in the global clock controller
 *   pins     GPIO12 (SDA) and GPIO13 (SCL) to the serial engine ("qup5"), GPIO15 the interrupt
 *            line, an input (low = the chip has events); all three in the TLMM's north tile
 *   I2C      the serial engine ("GENI") in FIFO mode, polled
 *   chip     the protocol of Linux's drivers/input/touchscreen/s6sy761.c, the same family
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * touch_steps=N stops after the first N:
 *   1  read how the clocks and the pins are left, and, if every clock of the serial engine is on,
 *      what the engine says it is                                        (reads only)
 *   2  read the S2DOS05: which of its outputs are on, at what voltage   (its two bus lines driven)
 *   3  switch LDO2 and LDO3 on: the two bits of its enable register, the others written as they
 *      were read; read back, then watch the touch chip's lines come up   (the first write to it)
 *   4  vote for the clocks and give the two bus pins to the serial engine (writes: GCC, TLMM)
 *   5  set the engine up for I2C and read the chip's identity            (the first transfers)
 *   6  read the panel's size, clear the chip's events and switch its sensing on
 *   7  watch the interrupt line, read the events and post the touches    (the default)
 * A finger's report: byte 0 its state (7:6, 1 press, 2 move, 3 release), its number from 1 (5:2)
 * and the kind of event (1:0, 0 = a coordinate); x is byte 1 and the high nibble of byte 3, y
 * byte 2 and the low nibble; byte 7 (5:0) how many events are still waiting.
 *
 * A read of a block whose clock is off can stop the whole SoC on this chip: the serial engine is
 * read only when the clock controller says all of its clocks run.
 *
 * r5q (Galaxy S10 Lite, SM8150; work/ios-r5q, Test 64+): the same chip family (firmware tsp_sec/y771_r5.bin) at 0x48
 * on the serial engine at 0xc80000 (QUP wrapper 2, engine 0: GCC_QUPV3_WRAP2_S0, its pins GPIO55/56 "qup17", SOUTH
 * tile), the interrupt GPIO87 (EAST tile), and its two supplies switched by GPIOs (live DT tsp_ldo_avdd: GPIO131,
 * tsp_ldo_dvdd: GPIO140, active high) instead of a PMIC: steps 2 and 3 read and, if needed, raise those two lines.
 * Its clocks: votes in 0x52014 (bit 0 core, 1 slave AHB, 2 master AHB, 3 core 2x, 4 the engine), branches 0x1e004,
 * 0x1e008, 0x1e144, the engine's source at 0x1e148 (Linux gcc-sm8150.c); TLMM 0x03000000, tiles as downstream.
 *
 * Register layouts: Linux drivers/clk/qcom/gcc-sm8250.c, drivers/pinctrl/qcom/pinctrl-sm8250.c,
 * include/linux/soc/qcom/geni-se.h, drivers/i2c/busses/i2c-qcom-geni.c, and Samsung's
 * include/linux/regulator/s2dos05.h (its SM8250 kernels).
 */
#include <kern/clock.h>
#include <kern/cpu_number.h>          /* (Test 312) the processor in every camera line */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well (as in redwood_buttons.c): without -v the
 * kernel's own messages reach that log only at the end of a run, and "xl log" during a run is to
 * show how far this driver has come.
 */
extern void loader_log_putc(char c);
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static vm_offset_t gcc, tlmm, tlmm_south, se;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/*
 * The bits of a bus driven by hand are timed by spinning on the clock. delay() may put the thread
 * to sleep until a timer wakes it, and then 5 us become most of a millisecond: an event of eight
 * bytes took a fifth of a second to read, and a tap came out as a long press.
 */
static void
spin_us(uint32_t us)
{
	uint64_t interval, deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &interval);
	deadline = mach_absolute_time() + interval;
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- the global clock controller ------------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
/* per platform (set by plat_select): y2q's values are the SM8250 ones this driver was written with */
static uint32_t o_vote = 0x52008, o_m_ahb = 0x17004, o_s_ahb = 0x17008, o_se_cbcr = 0x175fc, o_se_cmd = 0x17600;
static uint32_t o_core = 0x23000, o_core2x = 0x23008;
static uint32_t v_m_ahb = 1u << 6, v_s_ahb = 1u << 7, v_core = 1u << 8, v_core2x = 1u << 9, v_se = 1u << 15;
#define GCC_VOTE                (gcc + o_vote)          /* the application processor's votes */
#define VOTE_QUP0_M_AHB         v_m_ahb
#define VOTE_QUP0_S_AHB         v_s_ahb
#define VOTE_QUP0_CORE          v_core
#define VOTE_QUP0_CORE_2X       v_core2x
#define VOTE_QUP0_SE5           v_se
#define GCC_QUP0_M_AHB_CBCR     (gcc + o_m_ahb)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + o_s_ahb)
#define GCC_QUP0_SE5_CBCR       (gcc + o_se_cbcr)
#define GCC_QUP0_SE5_CMD        (gcc + o_se_cmd)        /* the serial engine's clock source; bit 0: take the new configuration */
#define GCC_QUP0_SE5_CFG        (gcc + o_se_cmd + 0x4)  /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE5_DFSR       (gcc + o_se_cmd + 0x14) /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE5_PERF_CFG(l) (gcc + o_se_cmd + 0x1c + 4 * (l))
#define GCC_QUP0_SE5_PERF_M(l)  (gcc + o_se_cmd + 0x5c + 4 * (l))
#define GCC_QUP0_SE5_PERF_N(l)  (gcc + o_se_cmd + 0x9c + 4 * (l))
#define CLOCK_HW_CONTROL        (1u << 1)               /* in a branch: on when the hardware needs it, "off" when idle */
#define XO_HZ                   19200000
#define GCC_QUP0_CORE_CBCR      (gcc + o_core)          /* (0 on r5q: not known, only voted for) */
#define GCC_QUP0_CORE_2X_CBCR   (gcc + o_core2x)
#define CLOCK_OFF               (1u << 31)

/* ---- the pins ------------------------------------------------------------------------------------- */

#define TLMM_SIZE               0x100000UL
static uint64_t p_tlmm_bus = 0x0f900000UL, p_tlmm_int = 0x0f900000UL, p_se = 0x00994000UL;
static unsigned PIN_SDA = 12, PIN_SCL = 13, PIN_INT = 15, PIN_FUNCTION_QUP5 = 1;
static bool r5q;                        /* the platform: y2q (PMIC supplies) or r5q (GPIO supplies) */
static vm_offset_t tlmm_int;            /* the interrupt pin's tile (the same as the bus pins' on y2q) */
#define TLMM_NORTH_PHYS         p_tlmm_bus
#define GPIO_CFG(n)             ((((n) == PIN_INT) ? tlmm_int : tlmm) + 0x1000UL * (n))  /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              ((((n) == PIN_INT) ? tlmm_int : tlmm) + 0x1000UL * (n) + 4)  /* bit 0 in, bit 1 out */
#define SE_PHYS                 p_se
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)    /* [29:20] high, [19:10] low, [9:0] cycle, in source clocks / divider */
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [15:9] address, [2] no stop after it */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)
#define GENI_PROTOCOL_I2C       3

#define I2C_OP_WRITE            1u
#define I2C_OP_READ             2u
#define I2C_STOP_STRETCH        (1u << 2)       /* keep the bus: a repeated start follows */
#define M_CMD_DONE              (1u << 0)
#define M_CMD_OVERRUN           (1u << 1)
#define M_ILLEGAL_CMD           (1u << 2)
#define M_CMD_FAILURE           (1u << 3)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_GP_NACK               (1u << 10)      /* "GP IRQ 1": nobody acknowledged */
#define M_GP_BUS_PROTO          (1u << 12)
#define M_GP_ARB_LOST           (1u << 13)
#define M_I2C_ERRORS            (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_GP_NACK | M_GP_BUS_PROTO | M_GP_ARB_LOST)
/* 100 kHz from the 19.2 MHz crystal: Linux's geni_i2c_clk_map */
#define I2C_100K_DIVIDER        7
#define I2C_100K_COUNTERS       (10u << 20 | 11u << 10 | 26u)
#define SEC_TS_ADDRESS          0x48

static const char *
pull_name(uint32_t cfg)
{
	static const char *const names[] = { "none", "down", "keeper", "up" };

	return names[cfg & 3];
}

static void
say_pin(const char *what, unsigned pin)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("touch: GPIO%u (%s): config %#x -- function %u, pull %s, %u mA, %s; level %s\n", pin, what, cfg,
	    (cfg >> 2) & 0xf, pull_name(cfg), 2 * (((cfg >> 6) & 7) + 1), (cfg & (1u << 9)) ? "OUTPUT" : "input",
	    (io & 1) ? "high" : "low");
}

static bool
say_clock(const char *what, vm_offset_t cbcr, uint32_t vote_bit, uint32_t votes)
{
	uint32_t value = rd(cbcr);
	bool on = !(value & CLOCK_OFF);

	printf("touch: clock %s: %s (branch %#x), %svoted for by this processor\n", what, on ? "on" : "OFF", value,
	    (votes & vote_bit) ? "" : "not ");
	return on;
}

/* Step 1: what the firmware has left, read only. */
static void
step_survey(void)
{
	uint32_t votes, cfg, param, revision;
	bool all_on = true;

	printf("touch: step 1: read the clocks of serial engine 5 (QUP wrapper 0) and the pins (reads only)\n");
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	tlmm_int = p_tlmm_int == p_tlmm_bus ? tlmm : ml_io_map(p_tlmm_int, TLMM_SIZE);
	votes = rd(GCC_VOTE);
	printf("touch: clock votes of this processor %#x\n", votes);
	all_on &= say_clock("wrapper 0 master AHB", GCC_QUP0_M_AHB_CBCR, VOTE_QUP0_M_AHB, votes);
	all_on &= say_clock("wrapper 0 slave AHB", GCC_QUP0_S_AHB_CBCR, VOTE_QUP0_S_AHB, votes);
	if (o_core != 0) {
		all_on &= say_clock("wrapper 0 core", GCC_QUP0_CORE_CBCR, VOTE_QUP0_CORE, votes);
		all_on &= say_clock("wrapper 0 core 2x", GCC_QUP0_CORE_2X_CBCR, VOTE_QUP0_CORE_2X, votes);
	} else {
		printf("touch: wrapper core clocks: %svoted for (bits %#x/%#x)\n", (votes & (v_core | v_core2x)) == (v_core | v_core2x) ? "" : "not all ",
		    v_core, v_core2x);
	}
	all_on &= say_clock("serial engine 5", GCC_QUP0_SE5_CBCR, VOTE_QUP0_SE5, votes);
	cfg = rd(GCC_QUP0_SE5_CFG);
	printf("touch: serial engine 5's clock source: command %#x, config %#x (source %u, divider %u.%u)\n",
	    rd(GCC_QUP0_SE5_CMD), cfg, (cfg >> 8) & 7, ((cfg & 0x1f) + 1) / 2, ((cfg & 0x1f) + 1) % 2 ? 5 : 0);
	say_pin("SDA", PIN_SDA);
	say_pin("SCL", PIN_SCL);
	say_pin("interrupt", PIN_INT);
	if (!all_on) {
		printf("touch: not every clock of the serial engine runs: its registers are not read\n");
		return;
	}
	se = ml_io_map(SE_PHYS, SE_SIZE);
	revision = rd(GENI_FW_REVISION_RO);
	param = rd(SE_HW_PARAM_0);
	printf("touch: serial engine 5: firmware %#x (protocol %u%s), status %#x, clock config %#x, clock select %#x, "
	    "FIFO interface %s, FIFO depth %u words\n", revision, (revision >> 8) & 0xff,
	    ((revision >> 8) & 0xff) == GENI_PROTOCOL_I2C ? ", I2C" : "", rd(GENI_STATUS), rd(GENI_SER_M_CLK_CFG),
	    rd(GENI_CLK_SEL), (rd(GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "there", (param >> 16) & 0x3f);
}

/* ---- the S2DOS05, on two GPIOs ------------------------------------------------------------------ */

#define TLMM_SOUTH_PHYS         0x0f500000UL
#define BB_CFG(n)               (bb->tlmm + 0x1000UL * (n))
#define BB_IO(n)                (bb->tlmm + 0x1000UL * (n) + 4)
#define PIN_PMIC_SDA            62
#define PIN_PMIC_SCL            63
#define CFG_OUTPUT_ENABLE       (1u << 9)
#define IO_OUT                  (1u << 1)
#define S2DOS05_ADDRESS         0x60
#define S2DOS05_REG_DEV_ID      0x00
#define S2DOS05_REG_EN          0x03    /* bit 0 LDO1, 1 LDO2, 2 LDO3, 3 LDO4, 4 buck */
#define S2DOS05_REG_LDO1_CFG    0x04    /* [6:0] voltage, [7] fast discharge */
#define S2DOS05_READ_COUNT      10      /* 0x00 to 0x09: identity, status, enables, voltages (not 0x11, which a read clears) */
#define HALF_BIT_US             5       /* 100 kHz */
#define STRETCH_US              1000    /* how long a device may hold the clock low */

/*
 * The lines are open drain, as on any I2C bus: a line is pulled low by making its pin an output
 * at 0, and let go by making it an input again -- the board's pull-up makes it high. Nothing else
 * of the pins' configuration changes.
 */
struct bitbang {
	vm_offset_t     tlmm;           /* the TLMM tile of both pins */
	unsigned        sda, scl;
	uint32_t        cfg_sda, cfg_scl;       /* their configuration, as inputs */
	uint8_t         address;
};
static struct bitbang pmic_bus = { .sda = PIN_PMIC_SDA, .scl = PIN_PMIC_SCL, .address = S2DOS05_ADDRESS };
static struct bitbang touch_bus = { .sda = 12, .scl = 13, .address = 0x48 };   /* the pins: set by the platform */
/*
 * The bus the functions below drive. (Test 324) One pointer per side, not one global: the touch thread (touch on the
 * SOUTH tile, the gauge, the cameras' regulator reads) and every other thread (the camera's threads: the sensor on
 * NORTH GPIO31/32, its S2MPB03 on EAST GPIO47/48) drive different pins at the same time. With one global `bb`, a
 * touch poll (sec_ts_read: bb = &touch_bus, many times a second) in the middle of a camera transaction redirected the
 * camera's remaining bit-bang -- and its explicit-pin writes (`BB_CFG(31)` = bb->tlmm + 0x1000 * 31) -- into the
 * SOUTH tile at an offset with no GPIO31 behind it. That race exists since the camera probe left the touch thread
 * (Test 284), and every camera session start and end did those writes: the hangs of Tests 290-321 came exactly there.
 */
static struct bitbang *bb_touch_side = &pmic_bus, *bb_other_side = &pmic_bus;
static thread_t bb_touch_thread;
#define bb      (*(current_thread() == bb_touch_thread ? &bb_touch_side : &bb_other_side))

static void
line(unsigned pin, uint32_t cfg, bool high)
{
	if (high) {
		wr(BB_CFG(pin), cfg & ~CFG_OUTPUT_ENABLE);
	} else {
		wr(BB_IO(pin), 0);
		wr(BB_CFG(pin), cfg | CFG_OUTPUT_ENABLE);
	}
	spin_us(HALF_BIT_US);
}

static bool
level(unsigned pin)
{
	return rd(BB_IO(pin)) & 1;
}

static void sda(bool high) { line(bb->sda, bb->cfg_sda, high); }

/* Let go of the clock and wait for it to be high: a device may hold it low while it gets ready. */
static bool
scl_release(void)
{
	line(bb->scl, bb->cfg_scl, true);
	for (unsigned waited = 0; !level(bb->scl); waited++) {
		if (waited >= STRETCH_US) {
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void scl_low(void) { line(bb->scl, bb->cfg_scl, false); }

static void
bus_start(void)
{
	sda(true);
	scl_release();
	sda(false);
	scl_low();
}

static void
bus_stop(void)
{
	sda(false);
	scl_release();
	sda(true);
}

/*
 * A bus left in the middle of a read -- a run that ended while a transfer was going on, the chip
 * still powered -- has the device holding SDA low for the bit it was sending. As Linux's
 * i2c_generic_scl_recovery(): clock it out, at most nine times, then a stop. True if the bus is
 * idle (both lines high) after that.
 */
static bool
bb_recover(const char *name)
{
	unsigned pulses = 0;

	if (level(bb->sda) && level(bb->scl)) {
		return true;
	}
	if (!level(bb->scl)) {
		printf("touch: %s: the clock line is held low: not driven\n", name);
		return false;
	}
	while (!level(bb->sda) && pulses < 9) {
		scl_low();
		scl_release();
		pulses++;
	}
	bus_stop();
	printf("touch: %s: SDA was held low; %u clock pulses and a stop: SDA %s, SCL %s now\n", name, pulses,
	    level(bb->sda) ? "high" : "low", level(bb->scl) ? "high" : "low");
	return level(bb->sda) && level(bb->scl);
}

/* One byte out; true if the device acknowledged it. */
static bool
bus_put(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		sda((byte >> bit) & 1);
		if (!scl_release()) {
			return false;
		}
		scl_low();
	}
	sda(true);
	if (!scl_release()) {
		return false;
	}
	ack = !level(bb->sda);
	scl_low();
	return ack;
}

static uint8_t
bus_get(bool last)
{
	uint8_t byte = 0;

	sda(true);
	for (int bit = 7; bit >= 0; bit--) {
		scl_release();
		byte |= (uint8_t)(level(bb->sda) << bit);
		scl_low();
	}
	sda(last);      /* acknowledge all but the last */
	scl_release();
	scl_low();
	sda(true);
	return byte;
}

/* Read `count` registers from `reg` on: the register number written, then a repeated start to read. */
static const char *
bb_read(uint8_t reg, uint8_t *bytes, unsigned count)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(reg)) {
		problem = "no acknowledge of the register number";
	} else {
		bus_start();
		if (!bus_put(bb->address << 1 | 1)) {
			problem = "no acknowledge of its address for the read";
		} else {
			for (unsigned i = 0; i < count; i++) {
				bytes[i] = bus_get(i + 1 == count);
			}
		}
	}
	bus_stop();
	return problem;
}

/* Write one register. */
static const char *
bb_write(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(reg)) {
		problem = "no acknowledge of the register number";
	} else if (!bus_put(value)) {
		problem = "no acknowledge of the value";
	}
	bus_stop();
	return problem;
}

/* A command alone: the address, one byte, stop. */
static const char *
bb_command(uint8_t command)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put(command)) {
		problem = "no acknowledge of the command";
	}
	bus_stop();
	return problem;
}

static const char *
pmic_read(uint8_t reg, uint8_t *bytes, unsigned count)
{
	bb = &pmic_bus;
	return bb_read(reg, bytes, count);
}

static const char *
pmic_write(uint8_t reg, uint8_t value)
{
	bb = &pmic_bus;
	return bb_write(reg, value);
}

/* LDO voltage from its configuration: LDO1 and LDO2 from 1.5 V, LDO3 and LDO4 from 2.7 V, 25 mV a step */
static unsigned
ldo_millivolts(unsigned ldo, uint8_t cfg)
{
	return (ldo <= 2 ? 1500 : 2700) + 25 * (cfg & 0x7f);
}

/* Step 2: what the display PMIC says, read only. */
static bool
step_pmic(void)
{
	uint8_t r[S2DOS05_READ_COUNT];
	const char *problem;
	uint32_t function_sda, function_scl;

	printf("touch: step 2: read the S2DOS05 (address %#x on GPIO%u/GPIO%u), registers 0x00 to 0x%02x\n", S2DOS05_ADDRESS,
	    PIN_PMIC_SDA, PIN_PMIC_SCL, S2DOS05_READ_COUNT - 1);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SIZE);
	pmic_bus.tlmm = tlmm_south;
	bb = &pmic_bus;
	pmic_bus.cfg_sda = rd(BB_CFG(PIN_PMIC_SDA));
	pmic_bus.cfg_scl = rd(BB_CFG(PIN_PMIC_SCL));
	function_sda = (pmic_bus.cfg_sda >> 2) & 0xf;
	function_scl = (pmic_bus.cfg_scl >> 2) & 0xf;
	printf("touch: the PMIC's bus: SDA config %#x (function %u, pull %s, %s, level %s), SCL config %#x (function %u, pull %s, "
	    "%s, level %s)\n", pmic_bus.cfg_sda, function_sda, pull_name(pmic_bus.cfg_sda),
	    (pmic_bus.cfg_sda & CFG_OUTPUT_ENABLE) ? "OUTPUT" : "input", level(PIN_PMIC_SDA) ? "high" : "low", pmic_bus.cfg_scl,
	    function_scl, pull_name(pmic_bus.cfg_scl), (pmic_bus.cfg_scl & CFG_OUTPUT_ENABLE) ? "OUTPUT" : "input",
	    level(PIN_PMIC_SCL) ? "high" : "low");
	if (function_sda != 0 || function_scl != 0) {
		printf("touch: those pins are not plain GPIOs: somebody else has them, left alone\n");
		return false;
	}
	/* (as inputs, so that nothing is driven before the bus is seen idle) */
	pmic_bus.cfg_sda &= ~CFG_OUTPUT_ENABLE;
	pmic_bus.cfg_scl &= ~CFG_OUTPUT_ENABLE;
	wr(BB_CFG(PIN_PMIC_SDA), pmic_bus.cfg_sda);
	wr(BB_CFG(PIN_PMIC_SCL), pmic_bus.cfg_scl);
	delay(20);
	if (!bb_recover("the PMIC's bus")) {
		printf("touch: the bus is not idle (SDA %s, SCL %s): nothing more is driven on it\n", level(PIN_PMIC_SDA) ? "high" : "low",
		    level(PIN_PMIC_SCL) ? "high" : "low");
		return false;
	}
	problem = pmic_read(S2DOS05_REG_DEV_ID, r, S2DOS05_READ_COUNT);
	wr(BB_CFG(PIN_PMIC_SDA), pmic_bus.cfg_sda);
	wr(BB_CFG(PIN_PMIC_SCL), pmic_bus.cfg_scl);
	if (problem != NULL) {
		printf("touch: S2DOS05: %s\n", problem);
		return false;
	}
	printf("touch: S2DOS05: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (id, topsys, status, enable, LDO1-4, buck, buck vout)\n",
	    r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]);
	for (unsigned ldo = 1; ldo <= 4; ldo++) {
		uint8_t cfg = r[S2DOS05_REG_LDO1_CFG + ldo - 1];

		printf("touch: S2DOS05 LDO%u: %s, %u mV%s\n", ldo, (r[S2DOS05_REG_EN] & (1u << (ldo - 1))) ? "ON" : "off",
		    ldo_millivolts(ldo, cfg), ldo == 2 ? " (the touch chip's 1.8 V)" : ldo == 3 ? " (the touch chip's 3.0 V)" : "");
	}
	printf("touch: S2DOS05 buck: %s\n", (r[S2DOS05_REG_EN] & (1u << 4)) ? "ON" : "off");
	return true;
}

#define S2DOS05_EN_TOUCH        ((1u << 1) | (1u << 2))  /* LDO2 and LDO3 */

/* The touch chip's three lines, as step 1 reads them: high once its I/O supply is up. */
static void
say_touch_lines(const char *when)
{
	printf("touch: %s: SDA %s, SCL %s, interrupt %s\n", when, (rd(GPIO_IO(PIN_SDA)) & 1) ? "high" : "low",
	    (rd(GPIO_IO(PIN_SCL)) & 1) ? "high" : "low", (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
}

/* Step 3: the touch chip's two supplies on, and nothing else changed. */
static bool
step_power(void)
{
	static const unsigned after_ms[] = { 1, 10, 50, 100, 300 };
	uint8_t en, now[S2DOS05_READ_COUNT];
	const char *problem;
	unsigned waited = 0;

	printf("touch: step 3: switch the S2DOS05's LDO2 and LDO3 on\n");
	problem = pmic_read(S2DOS05_REG_EN, &en, 1);
	if (problem != NULL) {
		printf("touch: S2DOS05: reading the enable register: %s\n", problem);
		return false;
	}
	if ((en & S2DOS05_EN_TOUCH) == S2DOS05_EN_TOUCH) {
		printf("touch: S2DOS05: enable register %#x: both are on already\n", en);
	} else {
		printf("touch: S2DOS05: enable register %#x; next: write %#x to it (LDO2 and LDO3 added, the rest as read)\n", en,
		    en | S2DOS05_EN_TOUCH);
		say_touch_lines("before");
		problem = pmic_write(S2DOS05_REG_EN, en | S2DOS05_EN_TOUCH);
		if (problem != NULL) {
			printf("touch: S2DOS05: writing the enable register: %s\n", problem);
			return false;
		}
	}
	/* (Samsung's driver waits 50 us for an LDO to come up) */
	delay(1000);
	problem = pmic_read(S2DOS05_REG_DEV_ID, now, S2DOS05_READ_COUNT);
	if (problem != NULL) {
		printf("touch: S2DOS05: reading back: %s\n", problem);
		return false;
	}
	printf("touch: S2DOS05 read back: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; LDO2 %s, LDO3 %s, the others %s\n",
	    now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9],
	    (now[S2DOS05_REG_EN] & (1u << 1)) ? "ON" : "off", (now[S2DOS05_REG_EN] & (1u << 2)) ? "ON" : "off",
	    (now[S2DOS05_REG_EN] & ~S2DOS05_EN_TOUCH) == (en & ~S2DOS05_EN_TOUCH) ? "as they were" : "CHANGED");
	for (unsigned i = 0; i < sizeof(after_ms) / sizeof(after_ms[0]); i++) {
		delay((after_ms[i] - waited) * 1000);
		waited = after_ms[i];
		char when[24];
		snprintf(when, sizeof(when), "%u ms after", after_ms[i]);
		say_touch_lines(when);
	}
	return (now[S2DOS05_REG_EN] & S2DOS05_EN_TOUCH) == S2DOS05_EN_TOUCH;
}

/* ---- r5q: the touch chip's supplies are two GPIO-switched regulators (steps 2 and 3) ------------- */

#define R5Q_PIN_AVDD            131     /* tsp_ldo_avdd, 3.3 V, active high (SOUTH tile, as the bus pins) */
#define R5Q_PIN_DVDD            140     /* tsp_ldo_dvdd, 1.8 V, active high */

static bool
step_r5q_supplies(bool raise)
{
	static const unsigned pins[2] = { R5Q_PIN_AVDD, R5Q_PIN_DVDD };
	bool all_high = true;

	printf("touch: step %u: the touch chip's supplies, GPIO%u (3.3 V) and GPIO%u (1.8 V)%s\n", raise ? 3 : 2, R5Q_PIN_AVDD,
	    R5Q_PIN_DVDD, raise ? ": raise any that is low" : " (reads only)");
	for (unsigned i = 0; i < 2; i++) {
		uint32_t cfg = rd(tlmm + 0x1000UL * pins[i]), io = rd(tlmm + 0x1000UL * pins[i] + 4);
		bool high = (cfg & (1u << 9)) ? ((io >> 1) & 1) : (io & 1);

		printf("touch: GPIO%u: config %#x (function %u, %s), level %s\n", pins[i], cfg, (cfg >> 2) & 0xf,
		    (cfg & (1u << 9)) ? "OUTPUT" : "input", high ? "high" : "low");
		if (!high && raise) {
			wr(tlmm + 0x1000UL * pins[i] + 4, 1u << 1);         /* out = 1 first, then the pin an output */
			wr(tlmm + 0x1000UL * pins[i], (cfg & ~0x3fcu) | (1u << 9));
			printf("touch: GPIO%u raised: config %#x, out %#x\n", pins[i], rd(tlmm + 0x1000UL * pins[i]),
			    rd(tlmm + 0x1000UL * pins[i] + 4));
		}
		all_high &= high;
	}
	if (raise && !all_high) {
		delay(300000);  /* the chip boots its firmware: Samsung's driver waits 70-300 ms */
		say_touch_lines("300 ms after the supplies came up");
	}
	return true;
}

/* ---- step 4: the serial engine's clocks and pins --------------------------------------------------- */

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	/* SM8250's parents of these sources: 0 the crystal, 1 GPLL0 (600 MHz), 6 GPLL0 even (300 MHz) */
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static uint32_t se_clock_select;
static bool bitbang;            /* the touch bus driven by hand (the engine has no FIFO interface) */

/* a branch runs: not "off", or under hardware control (which reads as off while nothing asks for it) */
static bool
branch_runs(vm_offset_t cbcr)
{
	uint32_t value = rd(cbcr);

	return !(value & CLOCK_OFF) || (value & CLOCK_HW_CONTROL);
}

static bool
step_clocks_pins(void)
{
	printf("touch: step 4: vote for serial engine 5's clocks and give GPIO12/GPIO13 to it\n");
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB | VOTE_QUP0_CORE | VOTE_QUP0_CORE_2X);
	if (rd(GCC_QUP0_SE5_DFSR) & 1) {
		/* preset rates: the I2C timings below are for the 19.2 MHz crystal, so that one */
		bool found = false;

		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE5_PERF_CFG(level)), rd(GCC_QUP0_SE5_PERF_M(level)),
			    rd(GCC_QUP0_SE5_PERF_N(level)));

			printf("touch: preset rate %u: %u Hz\n", level, hz);
			if (hz == XO_HZ && !found) {
				found = true;
				se_clock_select = level;
			}
		}
		if (!found) {
			printf("touch: no preset rate is the crystal's: the engine is left alone\n");
			return false;
		}
		printf("touch: the engine chooses its rate itself (DFS): preset %u, 19.2 MHz\n", se_clock_select);
	} else {
		wr(GCC_QUP0_SE5_CFG, (rd(GCC_QUP0_SE5_CFG) & ~0x371fu) | 1);   /* the crystal, undivided */
		wr(GCC_QUP0_SE5_CMD, rd(GCC_QUP0_SE5_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE5_CMD) & 1); i++) {
			delay(10);
		}
		if (rd(GCC_QUP0_SE5_CMD) & 1) {
			printf("touch: the engine's clock source does not take its configuration (%#x)\n", rd(GCC_QUP0_SE5_CMD));
			return false;
		}
		se_clock_select = 0;
		printf("touch: the engine's clock source set to the crystal (config %#x)\n", rd(GCC_QUP0_SE5_CFG));
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE5);
	for (int i = 0; !(branch_runs(GCC_QUP0_M_AHB_CBCR) && branch_runs(GCC_QUP0_S_AHB_CBCR) && branch_runs(GCC_QUP0_SE5_CBCR)); i++) {
		if (i >= 1000) {
			printf("touch: the clocks stay off: master AHB %#x, slave AHB %#x, engine %#x\n", rd(GCC_QUP0_M_AHB_CBCR),
			    rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_SE5_CBCR));
			return false;
		}
		delay(10);
	}
	printf("touch: clocks: votes %#x; master AHB %#x, slave AHB %#x, engine %#x\n", rd(GCC_VOTE), rd(GCC_QUP0_M_AHB_CBCR),
	    rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_SE5_CBCR));

	/* the device tree's tsp_i2c_active: function qup5 (y2q) / qup17 (r5q), 2 mA, no pull (the board pulls up); the interrupt an input */
	wr(GPIO_CFG(PIN_SDA), PIN_FUNCTION_QUP5 << 2);
	wr(GPIO_CFG(PIN_SCL), PIN_FUNCTION_QUP5 << 2);
	wr(GPIO_CFG(PIN_INT), 0);
	say_pin("SDA", PIN_SDA);
	say_pin("SCL", PIN_SCL);
	say_pin("interrupt", PIN_INT);

	se = ml_io_map(SE_PHYS, SE_SIZE);
	uint32_t revision = rd(GENI_FW_REVISION_RO);
	printf("touch: serial engine 5: firmware %#x (protocol %u), FIFO interface %s, FIFO depth %u words\n", revision,
	    (revision >> 8) & 0xff, (rd(GENI_IF_DISABLE_RO) & 1) ? "DISABLED" : "there", (rd(SE_HW_PARAM_0) >> 16) & 0x3f);
	if (((revision >> 8) & 0xff) != GENI_PROTOCOL_I2C) {
		printf("touch: the engine's firmware is not I2C's: left alone\n");
		return false;
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		/*
		 * The firmware has given this engine to the GSI DMA engine alone (Linux then talks to it
		 * through GPI DMA). Without that, the bus is driven by hand on the same two pins, as the
		 * PMIC's is: the engine's clock is let go again and the pins become plain GPIOs.
		 */
		printf("touch: the engine's FIFO interface is disabled (GSI DMA only): the bus is driven by hand instead\n");
		wr(GCC_VOTE, rd(GCC_VOTE) & ~VOTE_QUP0_SE5);
		touch_bus.tlmm = tlmm;
		touch_bus.cfg_sda = 0;  /* function 0 (GPIO), no pull, 2 mA, input */
		touch_bus.cfg_scl = 0;
		wr(GPIO_CFG(PIN_SDA), touch_bus.cfg_sda);
		wr(GPIO_CFG(PIN_SCL), touch_bus.cfg_scl);
		delay(20);
		say_pin("SDA", PIN_SDA);
		say_pin("SCL", PIN_SCL);
		bb = &touch_bus;
		if (!bb_recover("the touch bus")) {
			printf("touch: the touch bus is not idle: nothing more is driven on it\n");
			return false;
		}
		bitbang = true;
	}
	return true;
}

/* ---- step 5: I2C through the serial engine, and the chip's identity ------------------------------- */

static uint32_t fifo_depth;

static void
geni_i2c_init(void)
{
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables (the general purpose ones say NACK and bus errors) */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc07fff);
	/* 8 bits, four bytes in a FIFO word, as Linux's geni_se_config_packing(8, 4, true) */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, I2C_100K_DIVIDER << 4 | 1);
	wr(I2C_SCL_COUNTERS, I2C_100K_COUNTERS);
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		delay(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			delay(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* Wait for the command to end; NULL, or what went wrong. */
static const char *
geni_wait(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint64_t deadline;
	uint32_t status = 0;

	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		status = rd(GENI_M_IRQ_STATUS);
		if (rx != NULL) {
			uint32_t fifo = rd(GENI_RX_FIFO_STATUS), words = fifo & 0x01ffffff, bytes = words * 4;

			if ((fifo & 0x80000000u) && ((fifo >> 28) & 7) && ((fifo >> 28) & 7) < 4) {
				bytes -= 4 - ((fifo >> 28) & 7);
			}
			for (uint32_t i = 0; i < words; i++) {
				uint32_t word = rd(GENI_RX_FIFO);

				for (int j = 0; j < 4 && bytes; j++, bytes--) {
					if (*received < length) {
						rx[(*received)++] = (uint8_t)(word >> (8 * j));
					}
				}
			}
		}
		if (status & M_I2C_ERRORS) {
			wr(GENI_M_IRQ_CLEAR, status);
			geni_recover();
			return (status & M_GP_NACK) ? "no acknowledge (NACK)" : (status & M_GP_ARB_LOST) ? "arbitration lost" :
			       (status & M_GP_BUS_PROTO) ? "bus protocol error" : "the engine reports an error";
		}
		if (status & M_CMD_DONE) {
			wr(GENI_M_IRQ_CLEAR, status);
			return NULL;
		}
	}
	geni_recover();
	return "no end of the command in 100 ms";
}

/* Write the command byte, then (repeated start) read `length` bytes. NULL, or what went wrong. */
static const char *
sec_ts_read(uint8_t command, uint8_t *rx, uint32_t length)
{
	const char *problem;
	uint32_t received = 0;

	if (bitbang) {
		bb = &touch_bus;
		return bb_read(command, rx, length);
	}
	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 100) {
			geni_recover();
			break;
		}
		delay(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(I2C_TX_TRANS_LEN, 1);
	wr(GENI_M_CMD0, I2C_OP_WRITE << 27 | SEC_TS_ADDRESS << 9 | I2C_STOP_STRETCH);
	wr(GENI_TX_FIFO, command);
	problem = geni_wait(NULL, 0, NULL);
	if (problem != NULL) {
		return problem;
	}
	wr(I2C_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, I2C_OP_READ << 27 | SEC_TS_ADDRESS << 9);
	problem = geni_wait(rx, length, &received);
	if (problem == NULL && received != length) {
		return "fewer bytes than asked for";
	}
	return problem;
}

#define SEC_TS_DEVICE_ID        0x52    /* 3 bytes */
#define SEC_TS_BOOT_STATUS      0x55    /* 0x10 boot loader, 0x20 application */
#define SEC_TS_READ_ONE_EVENT   0x60    /* 8 bytes */
#define SEC_TS_EVENT_SIZE       8

static bool
step_identity(void)
{
	uint8_t id[3], boot, event[SEC_TS_EVENT_SIZE];
	const char *problem;

	if (bitbang) {
		printf("touch: step 5: read the chip (address %#x) over the bus driven by hand, 100 kHz\n", SEC_TS_ADDRESS);
	} else {
		printf("touch: step 5: set the engine up for I2C at 100 kHz and read the chip (address %#x)\n", SEC_TS_ADDRESS);
		geni_i2c_init();
		printf("touch: engine: clock config %#x, SCL counters %#x, status %#x\n", rd(GENI_SER_M_CLK_CFG), rd(I2C_SCL_COUNTERS),
		    rd(GENI_STATUS));
	}
	problem = sec_ts_read(SEC_TS_DEVICE_ID, id, sizeof(id));
	if (problem != NULL) {
		printf("touch: reading the device identity (%#x): %s\n", SEC_TS_DEVICE_ID, problem);
		return false;
	}
	printf("touch: device identity %02x %02x %02x\n", id[0], id[1], id[2]);
	problem = sec_ts_read(SEC_TS_BOOT_STATUS, &boot, 1);
	if (problem != NULL) {
		printf("touch: reading the boot status (%#x): %s\n", SEC_TS_BOOT_STATUS, problem);
		return false;
	}
	printf("touch: boot status %#x (%s)\n", boot, boot == 0x20 ? "the application runs" : boot == 0x10 ? "BOOT LOADER only" : "?");
	problem = sec_ts_read(SEC_TS_READ_ONE_EVENT, event, sizeof(event));
	if (problem != NULL) {
		printf("touch: reading an event (%#x): %s\n", SEC_TS_READ_ONE_EVENT, problem);
		return false;
	}
	printf("touch: first event %02x %02x %02x %02x %02x %02x %02x %02x; the interrupt line is now %s\n", event[0], event[1],
	    event[2], event[3], event[4], event[5], event[6], event[7], (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
	return boot == 0x20;
}

/* A command byte alone. */
static const char *
sec_ts_command(uint8_t command)
{
	if (bitbang) {
		bb = &touch_bus;
		return bb_command(command);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(I2C_TX_TRANS_LEN, 1);
	wr(GENI_M_CMD0, I2C_OP_WRITE << 27 | SEC_TS_ADDRESS << 9);
	wr(GENI_TX_FIFO, command);
	return geni_wait(NULL, 0, NULL);
}

/* ---- steps 6 and 7: sensing, and the touches ------------------------------------------------------ */

#define SEC_TS_SENSE_ON         0x10
#define SEC_TS_PANEL_INFO       0x23    /* 11 bytes: [0..1] x range, [2..3] y range (big endian) */
#define SEC_TS_READ_ALL_EVENT   0x61    /* the waiting events, 8 bytes each */
#define SEC_TS_CLEAR_EVENTS     0x62
#define SEC_TS_EVENT_COUNT      32
#define SEC_TS_FINGERS          10
#define POLL_US                 8000    /* 125 times a second, as the redwood driver */

static uint32_t max_x, max_y;

static bool
step_sense(void)
{
	uint8_t panel[11];
	const char *problem;

	printf("touch: step 6: read the panel's size, clear the chip's events and switch sensing on\n");
	problem = sec_ts_read(SEC_TS_PANEL_INFO, panel, sizeof(panel));
	if (problem != NULL) {
		printf("touch: reading the panel information (%#x): %s\n", SEC_TS_PANEL_INFO, problem);
		return false;
	}
	max_x = (uint32_t)panel[0] << 8 | panel[1];
	max_y = (uint32_t)panel[2] << 8 | panel[3];
	printf("touch: panel information %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x: x up to %u, y up to %u\n",
	    panel[0], panel[1], panel[2], panel[3], panel[4], panel[5], panel[6], panel[7], panel[8], panel[9], panel[10],
	    max_x, max_y);
	if (max_x < 100 || max_y < 100) {
		max_x = max_y = 4096;   /* the device tree's sec,max_coords */
		printf("touch: ... which does not look like a size: 4096 x 4096 instead (the device tree's)\n");
	}
	problem = sec_ts_command(SEC_TS_CLEAR_EVENTS);
	if (problem == NULL) {
		delay(10000);
		problem = sec_ts_command(SEC_TS_SENSE_ON);
	}
	if (problem != NULL) {
		printf("touch: clearing the events or switching sensing on: %s\n", problem);
		return false;
	}
	delay(10000);
	printf("touch: sensing is on; the interrupt line is %s\n", (rd(GPIO_IO(PIN_INT)) & 1) ? "high" : "low");
	return true;
}

static struct {
	bool down;
	uint32_t x, y;
	uint64_t since;         /* when it landed */
	unsigned moves;
} finger[SEC_TS_FINGERS];
/*
 * boot-arg touch_log=1: a line for each touch and for the first events. Off by default: under -v
 * a line is drawn on the screen, and that takes long enough (the whole framebuffer scrolls) for a
 * tap to be read late and taken for a long press.
 */
static uint32_t touch_log;
static unsigned said_events, said_touches;
static uint64_t touches_down, events_read, read_errors, reads, read_ns, read_longest_ns;

static void
handle_event(const uint8_t *e)
{
	unsigned kind = e[0] & 3, state = e[0] >> 6, id = (e[0] >> 2) & 0xf;
	uint32_t x, y;

	events_read++;
	if (touch_log && said_events < 40) {
		said_events++;
		printf("touch: event %02x %02x %02x %02x %02x %02x %02x %02x\n", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
	}
	if (kind != 0 || id == 0 || id > SEC_TS_FINGERS) {
		return;         /* a status or vendor event, or no finger */
	}
	id--;
	x = (uint32_t)e[1] << 4 | e[3] >> 4;
	y = (uint32_t)e[2] << 4 | (e[3] & 0xf);
	/* to the display's 0..65535, left/top to right/bottom */
	x = x >= max_x ? 65535 : x * 65535 / max_x;
	y = y >= max_y ? 65535 : y * 65535 / max_y;
	if (state == 1 || state == 2) {
		bool landed = !finger[id].down;

		finger[id].down = true;
		finger[id].x = x;
		finger[id].y = y;
		if (landed) {
			touches_down++;
			finger[id].since = mach_absolute_time();
			finger[id].moves = 0;
		} else {
			finger[id].moves++;
		}
		redwood_touch_post(landed ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_MOVE, id, x, y);
		if (touch_log && landed && said_touches < 200) {
			said_touches++;
			printf("touch: finger %u DOWN at %u, %u (of 65535); event %02x %02x %02x %02x %02x %02x %02x %02x\n", id, x, y,
			    e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		}
	} else if (state == 3 && finger[id].down) {
		uint64_t held;

		finger[id].down = false;
		redwood_touch_post(REDWOOD_TOUCH_UP, id, finger[id].x, finger[id].y);
		absolutetime_to_nanoseconds(mach_absolute_time() - finger[id].since, &held);
		if (touch_log && said_touches < 200) {
			said_touches++;
			printf("touch: finger %u up at %u, %u after %llu ms and %u moves; event %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    id, finger[id].x, finger[id].y, held / NSEC_PER_MSEC, finger[id].moves, e[0], e[1], e[2], e[3], e[4], e[5],
			    e[6], e[7]);
		}
	}
}

/* Whether any finger is down, as far as the events have said. */
static bool
any_finger_down(void)
{
	for (unsigned i = 0; i < SEC_TS_FINGERS; i++) {
		if (finger[i].down) {
			return true;
		}
	}
	return false;
}

/*
 * Step 7: whenever the interrupt line is low, read what is waiting -- and while a finger is down,
 * every time whatever the line says. A lift was missed now and then (seen with touch_trace=1:
 * a quick tap's landing, then nothing until the next touch, which came as a move of the same
 * finger), so that iOS held the finger down and made a long press of the tap: the line says so
 * for less than the 8 ms between two looks, it seems. Asked when nothing waits, the chip
 * answers an event of zeroes.
 */
/*
 * r5q: the battery's fuel gauge, Samsung S2MU106 (live DT: i2c@0xc88000 s2mu106-fuelgauge@3B, its pins GPIO57 SDA and
 * GPIO58 SCL "qup19" -- function 2, SOUTH tile like the touch bus; Linux pinctrl-sm8150.c). Its engine is GSI-only like the
 * touch chip's, so the bus is driven by hand here too, from this thread (the one user of the bit-bang functions), every
 * 5 s while no finger is down. Only reads, the registers of Samsung's s2mu106_fuelgauge.c:
 *   0x08 RSOC: 2's complement, 10000 = 1 << 14 (hundredths of a percent); scaled as Android shows it by the DT's
 *        capacity_max 990 (99.0 % raw is 100 % shown)
 *   0x04 RVBAT: mV = value * 1000 >> 13
 *   0x06 RCUR_CC: 2's complement; negative = charging, mA = |value| * 1000 >> 12
 * The temperature is the battery driver's business on Android (a thermistor on the PMIC): 25 C is reported.
 */
#define PIN_GAUGE_SDA           57
#define PIN_GAUGE_SCL           58
#define GAUGE_ADDRESS           0x3b
#define GAUGE_CAPACITY_MAX      990
static struct bitbang gauge_bus = { .sda = PIN_GAUGE_SDA, .scl = PIN_GAUGE_SCL, .address = GAUGE_ADDRESS };
static bool gauge_ready, gauge_off;
static uint32_t gauge_full_mah, gauge_design_mah;
static unsigned gauge_reads, gauge_failed_in_row;

static bool
gauge_setup(void)
{
	uint32_t was_sda, was_scl;

	gauge_bus.tlmm = tlmm;          /* (r5q: the SOUTH tile, mapped for the touch pins GPIO55/56) */
	bb = &gauge_bus;
	was_sda = rd(BB_CFG(PIN_GAUGE_SDA));
	was_scl = rd(BB_CFG(PIN_GAUGE_SCL));
	printf("battery: gauge pins GPIO%u config %#x (function %u), GPIO%u config %#x (function %u)\n", PIN_GAUGE_SDA, was_sda,
	    (was_sda >> 2) & 0xf, PIN_GAUGE_SCL, was_scl, (was_scl >> 2) & 0xf);
	gauge_bus.cfg_sda = 0;          /* function 0 (GPIO), no pull, 2 mA, input: the board pulls the lines up */
	gauge_bus.cfg_scl = 0;
	wr(BB_CFG(PIN_GAUGE_SDA), 0);
	wr(BB_CFG(PIN_GAUGE_SCL), 0);
	spin_us(50);
	if (!bb_recover("the battery gauge's bus")) {
		printf("battery: the gauge's bus stays low: pins put back, no battery level\n");
		wr(BB_CFG(PIN_GAUGE_SDA), was_sda);
		wr(BB_CFG(PIN_GAUGE_SCL), was_scl);
		return false;
	}
	return true;
}

/*
 * r5q (Test 104): the S2MU106 charger's status for iOS, read over QUP0 SE4 in FIFO mode -- the engine as the loader left
 * it (Test 102: clocks voted, GPIO51/52 on qup4, packing, 100 kHz; the charger's watchdog off). Reads only:
 * CHG_STATUS0 (0x0e) bits 2:0 the input (3 or 5: good power, Samsung's s2mu106_get_charging_health), CHG_STATUS1
 * (0x0f) bit 7 discharging, bits 1:0 top-off/done (full). Returns false if the engine or the charger does not answer.
 */
#define R5Q_SE4_PHYS            0x00890000UL
static vm_offset_t chg_se;
static bool chg_se_off;
static const char *chg_xfer_at(uint32_t addr, uint32_t op, uint8_t *data, uint32_t n, bool stretch);

/*
 * (Test 143) SE4 is shared with the vibration motor's driver (redwood_haptic.c: the S2MU106's haptic block, 0x3a): a
 * register's read is two commands (its number with no stop, then the bytes), which nothing may come between. Held by
 * gauge_poll()/charger_dump()'s caller and by the two functions below.
 */
static LCK_GRP_DECLARE(r5q_pmic_group, "r5q-pmic-bus");
static LCK_MTX_DECLARE(r5q_pmic_lock, &r5q_pmic_group);

/* the engine as the loader left it, mapped and checked once; false if it is not (then never again) */
static bool
chg_se_ready(void)
{
	if (chg_se_off) {
		return false;
	}
	if (chg_se == 0) {
		chg_se = ml_io_map(R5Q_SE4_PHYS, 0x4000);
		if (((rd(chg_se + 0x68) >> 8) & 0xff) != 3 || (rd(chg_se + 0x48) & 0xff1) != 0x71) {
			printf("charger: SE4 is not set up for I2C (fw %#x, clk %#x)\n", rd(chg_se + 0x68), rd(chg_se + 0x48));
			chg_se_off = true;
			return false;
		}
	}
	return true;
}

bool redwood_r5q_pmic_read(uint8_t address, uint8_t reg, uint8_t *value);
bool redwood_r5q_pmic_write(uint8_t address, uint8_t reg, uint8_t value);

bool
redwood_r5q_pmic_read(uint8_t address, uint8_t reg, uint8_t *value)
{
	const char *why = "the bus is not set up";

	lck_mtx_lock(&r5q_pmic_lock);
	if (chg_se_ready()) {
		why = chg_xfer_at(address, 1, &reg, 1, true);
		if (why == NULL) {
			why = chg_xfer_at(address, 2, value, 1, false);
		}
	}
	lck_mtx_unlock(&r5q_pmic_lock);
	return why == NULL;
}

bool
redwood_r5q_pmic_write(uint8_t address, uint8_t reg, uint8_t value)
{
	uint8_t b[2] = { reg, value };
	const char *why = "the bus is not set up";

	lck_mtx_lock(&r5q_pmic_lock);
	if (chg_se_ready()) {
		why = chg_xfer_at(address, 1, b, 2, false);
	}
	lck_mtx_unlock(&r5q_pmic_lock);
	return why == NULL;
}

static const char *
chg_xfer(uint32_t op, uint8_t *data, uint32_t n, bool stretch)
{
	return chg_xfer_at(0x3d, op, data, n, stretch);
}

static const char *
chg_xfer_at(uint32_t addr, uint32_t op, uint8_t *data, uint32_t n, bool stretch)
{
	uint32_t st = 0, got = 0;

	wr(chg_se + 0x618, 0xffffffffu);
	wr(chg_se + (op == 1 ? 0x26c : 0x270), n);
	wr(chg_se + 0x600, (op << 27) | (addr << 9) | (stretch ? 4u : 0u));
	if (op == 1) {
		uint32_t w = 0;

		for (uint32_t k = 0; k < n && k < 4; k++) {
			w |= (uint32_t)data[k] << (8 * k);
		}
		wr(chg_se + 0x700, w);
		wr(chg_se + 0x80c, 0);
	}
	for (int t = 0; t < 5000; t++) {                /* 50 ms at most */
		st = rd(chg_se + 0x610);
		if (op == 2) {
			uint32_t fs = rd(chg_se + 0x804) & 0x1ffffff;

			for (uint32_t w = 0; w < fs && got < n; w++) {
				uint32_t v = rd(chg_se + 0x780);

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
	wr(chg_se + 0x618, st);
	wr(chg_se + 0x80c, 1);
	if (!(st & 1)) {
		wr(chg_se + 0x604, 1u << 1);
		return "timeout";
	}
	return (st & (1u << 10)) ? "NACK" : (st & 0x2e) ? "failed" : (op == 2 && got < n) ? "short" : NULL;
}

/*
 * r5q (Test 106): the input limit from what is on the cable, as Samsung's battery driver sets it per cable type: the
 * S2MU106's USB detection (MUIC, I2C 0x3e) DEVICE_TYP1 0x47 (bit 6 DCP, bit 5 CDP, bit 2 USB) and CHG_TYP 0x4d (bit 0
 * DCP, bit 1 CDP, bit 2 USB): a wall charger 1800 mA, a charging port 1500 mA, a computer's port 900 mA; written to
 * CHG_CTRL1 0x19 ((mA - 50) / 25) when it differs. The charge current stays the loader's 1500 mA.
 */
static void
chg_tune(const uint8_t st[2])
{
	static uint8_t last_code;
	uint8_t typ[7], ctrl1 = 0x19, code, now[1];
	const char *kind;

	if ((st[1] >> 4) == 1) {                /* WD_SUSPEND again (the loader's set-up undone?): kick it, keep it off */
		uint8_t kick[2] = { 0x24, 0x1b }, off[2] = { 0x24, 0x0a };

		if (chg_xfer(1, kick, 2, false) == NULL && chg_xfer(1, off, 2, false) == NULL) {
			printf("charger: watchdog suspend seen: cleared, watchdog off\n");
		}
	}
	if ((st[0] & 7) != 3 && (st[0] & 7) != 5) {
		last_code = 0;                  /* (no power: look again when it comes) */
		return;
	}
	typ[0] = 0x47;
	if (chg_xfer_at(0x3e, 1, typ, 1, true) != NULL || chg_xfer_at(0x3e, 2, typ, 7, false) != NULL) {
		return;
	}
	if ((typ[0] & 0x40) || (typ[6] & 0x01)) {
		code = 0x46;
		kind = "a wall charger (DCP): 1800 mA";
	} else if ((typ[0] & 0x20) || (typ[6] & 0x02)) {
		code = 0x3a;
		kind = "a charging port (CDP): 1500 mA";
	} else if ((typ[0] & 0x04) || (typ[6] & 0x04)) {
		code = 0x22;
		kind = "a computer's port (USB): 900 mA";
	} else {
		return;                         /* (not known yet: as it is) */
	}
	if (code == last_code) {
		return;
	}
	if (chg_xfer(1, &ctrl1, 1, true) != NULL || chg_xfer(2, now, 1, false) != NULL) {
		return;
	}
	if ((now[0] & 0x7f) != code) {
		uint8_t w[2] = { 0x19, (uint8_t)((now[0] & 0x80) | code) };

		if (chg_xfer(1, w, 2, false) != NULL) {
			return;
		}
	}
	last_code = code;
	printf("charger: on %s (MUIC type1 %#x chg_typ %#x)\n", kind, typ[0], typ[6]);
}

static bool
chg_status(uint8_t st[2])
{
	static unsigned failures;
	const char *why;
	uint8_t reg = 0x0e;

	if (!chg_se_ready()) {           /* (the loader's set-up must be there: the I2C protocol, FIFO mode, its divider) */
		return false;
	}
	why = chg_xfer(1, &reg, 1, true);
	if (why == NULL) {
		why = chg_xfer(2, st, 2, false);
	}
	if (why != NULL) {
		printf("charger: status read: %s%s\n", why, ++failures >= 5 ? "; no more tries" : "");
		chg_se_off = failures >= 5;
		return false;
	}
	failures = 0;
	chg_tune(st);
	return true;
}

static void
gauge_poll(void)
{
	uint8_t soc[2], vbat[2], cur[2];
	const char *problem;
	struct bitbang *was = bb;
	struct redwood_battery_state b;
	uint32_t raw;
	int32_t hundredths, mv, ma;

	if (gauge_off) {
		return;
	}
	if (!gauge_ready) {
		gauge_ready = gauge_setup();
		if (!gauge_ready) {
			gauge_off = true;
			bb = was;
			return;
		}
	}
	bb = &gauge_bus;
	problem = bb_read(0x08, soc, 2);
	if (problem == NULL) {
		problem = bb_read(0x04, vbat, 2);
	}
	if (problem == NULL) {
		problem = bb_read(0x06, cur, 2);
	}
	if (problem == NULL && redwood_platform_is("r5q") && (gauge_reads % 12) == 0) {
		/* (Test 106) RBATCAP 0x0e (learned, mAh << 2) and BATCAP 0x10 (as made, mAh), Samsung's s2mu106_get_batcap_ocv */
		uint8_t cap[4];

		if (bb_read(0x0e, cap, 4) == NULL) {
			gauge_full_mah = (uint32_t)(cap[0] | cap[1] << 8) >> 2;
			gauge_design_mah = (uint32_t)(cap[2] | cap[3] << 8);    /* (plain mAh: s2mu106_get_soh) */
		}
	}
	bb = was;
	gauge_reads++;
	if (problem != NULL) {
		printf("battery: gauge read %u: %s\n", gauge_reads, problem);
		if (++gauge_failed_in_row >= 5) {
			printf("battery: five failed reads in a row: no battery level from now on\n");
			gauge_off = true;
		}
		return;
	}
	gauge_failed_in_row = 0;
	raw = (uint32_t) soc[1] << 8 | soc[0];
	hundredths = (raw & 0x8000) ? 0 : (int32_t) (((raw & 0x7fff) * 10000u) >> 14);
	hundredths = hundredths * 1000 / GAUGE_CAPACITY_MAX;
	hundredths = hundredths < 0 ? 0 : hundredths > 10000 ? 10000 : hundredths;
	mv = (int32_t) ((((uint32_t) vbat[1] << 8 | vbat[0]) * 1000u) >> 13);
	raw = (uint32_t) cur[1] << 8 | cur[0];
	ma = (raw & 0x8000) ? (int32_t) (((((~raw) & 0xffff) + 1) * 1000u) >> 12) : -(int32_t) (((raw & 0x7fff) * 1000u) >> 12);
	if (gauge_reads <= 2 || gauge_reads % 12 == 0) {
		printf("battery: gauge: SOC %d.%02d %% (raw %02x%02x), %d mV (raw %02x%02x), %d mA (raw %02x%02x)\n",
		    hundredths / 100, hundredths % 100, soc[1], soc[0], mv, vbat[1], vbat[0], ma, cur[1], cur[0]);
	}
	if (mv < 2800 || mv > 4600) {
		printf("battery: gauge: %d mV is not a battery's: not reported\n", mv);
		return;
	}
	bzero(&b, sizeof(b));
	b.size = sizeof(b);
	b.percent_hundredths = hundredths;
	b.voltage_uv = mv * 1000;
	b.current_ua = ma * 1000;
	b.temperature_hundredths = 2500;
	b.present = 1;
	if (gauge_full_mah >= 1000 && gauge_full_mah <= 10000) {
		b.full_uah = (int32_t)gauge_full_mah * 1000;
		b.charge_uah = (int32_t)((int64_t)b.full_uah * hundredths / 10000);
	}
	if (gauge_design_mah >= 1000 && gauge_design_mah <= 10000) {
		b.design_uah = (int32_t)gauge_design_mah * 1000;
	}
	if (gauge_reads % 12 == 0 && gauge_full_mah != 0) {
		printf("battery: capacity %u mAh learned, %u mAh as made\n", gauge_full_mah, gauge_design_mah);
	}
	b.external = ma > 20;
	b.status = ma > 20 ? 1 : (hundredths >= 10000 ? 4 : 2);
	{
		uint8_t st[2];                  /* (Test 104: the charger's own word, when it answers) */
		static int said = -1;

		if (redwood_platform_is("r5q") && chg_status(st)) {
			uint32_t in = st[0] & 7;
			bool power = in == 3 || in == 5, full = (st[1] & 3) != 0;

			b.external = power;
			b.status = !power || (st[1] & 0x80) ? 2 : full ? 4 : 1;
			if (said != (int)(st[0] | st[1] << 8)) {
				said = st[0] | st[1] << 8;
				printf("charger: status0 %#x status1 %#x: %s\n", st[0], st[1], !power ? "no input power" :
				    full ? "on power, full" : "on power, charging");
			}
		}
	}
	b.usb_type = b.external ? 2 : 0;
	redwood_battery_report(&b);
}

/*
 * r5q (Test 87): the charger. Not the PM8150B (its charger peripherals do not answer the arbiter): a Samsung S2MU106
 * (live DT: s2mu106@3D on i2c@890000, QUP SE4, GPIO51 SDA / GPIO52 SCL "qup4"; its fuel gauge is the 0x3b above, its
 * USB-PD 0x3c). Charging stops when nobody drives it, as on a Samsung phone whose kernel has no s2mu106 driver: this
 * reads its registers (0x00..0x3f) once a minute, READS ONLY, the pins as the gauge's are (GPIO, the board's
 * pull-ups) and put back after each read. Boot-arg charger_dump=1.
 */
#define PIN_CHG_SDA             51
#define PIN_CHG_SCL             52
static struct bitbang charger_bus = { .sda = PIN_CHG_SDA, .scl = PIN_CHG_SCL, .address = 0x3d };
static bool charger_dump_on, charger_dump_off;
static unsigned charger_dumps;

/*
 * (r5q Test 270, boot-arg campmic=1) The cameras' power, read only: two Samsung S2MPB03 regulator chips at 0x56 on
 * bit-banged I2C (DT i2c@1: GPIO94 SDA / 95 SCL, NORTH tile; i2c@2: GPIO47 / 48, EAST tile). Their LDOs feed the
 * camera sensors (the DT's 1.1/1.8/2.8 V supplies of the camera nodes). Registers 0x00 (the chip's ID) and 0x02-0x2f
 * are read once, ~30 s after boot, in the battery's thread under r5q_pmic_lock; 0x01 (an interrupt latch, which a
 * read may clear) is not. Nothing is written to the chips; the pins are set as the DT's (GPIO, no pull, 2 mA) for the
 * read and put back as found.
 */
static bool campmic_on, campmic_done;

static void
campmic_bus(const char *name, uint64_t tile_phys, unsigned sda_pin, unsigned scl_pin)
{
	struct bitbang bus = { .sda = sda_pin, .scl = scl_pin, .address = 0x56 }, *was = bb;
	uint32_t was_sda, was_scl;
	uint8_t id = 0, regs[0x30];
	const char *problem = NULL;
	char line[200];

	bus.tlmm = ml_io_map(tile_phys, 0x1000UL * (scl_pin > sda_pin ? scl_pin : sda_pin) + 0x1000);
	if (bus.tlmm == 0) {
		printf("camera power: %s: no mapping\n", name);
		return;
	}
	bb = &bus;
	was_sda = rd(BB_CFG(sda_pin));
	was_scl = rd(BB_CFG(scl_pin));
	printf("camera power: %s: GPIO%u cfg %#x io %#x, GPIO%u cfg %#x io %#x (as found)\n", name, sda_pin, was_sda,
	    rd(BB_IO(sda_pin)), scl_pin, was_scl, rd(BB_IO(scl_pin)));
	bus.cfg_sda = 0;
	bus.cfg_scl = 0;
	wr(BB_CFG(sda_pin), 0);
	wr(BB_CFG(scl_pin), 0);
	spin_us(50);
	if (!bb_recover(name)) {
		problem = "the bus stays low";
	}
	if (problem == NULL) {
		problem = bb_read(0x00, &id, 1);
	}
	for (unsigned r = 0x02; problem == NULL && r < sizeof(regs); r += 2) {
		problem = bb_read((uint8_t)r, regs + r, 2);
	}
	wr(BB_CFG(sda_pin), was_sda);
	wr(BB_CFG(scl_pin), was_scl);
	bb = was;
	if (problem != NULL) {
		printf("camera power: %s: S2MPB03 at 0x56: %s\n", name, problem);
		return;
	}
	printf("camera power: %s: S2MPB03 at 0x56 answers: ID %#04x\n", name, id);
	for (unsigned r = 0x00; r < sizeof(regs); r += 16) {
		int at = snprintf(line, sizeof(line), "camera power: %s %02x:", name, r);

		for (unsigned i = 0; i < 16; i++) {
			at += (r + i < 2) ? snprintf(line + at, sizeof(line) - at, " --") :
			    snprintf(line + at, sizeof(line) - at, " %02x", regs[r + i]);
		}
		printf("%s\n", line);
	}
}

static void
campmic_peek(void)
{
	if (!campmic_on || campmic_done) {
		return;
	}
	campmic_done = true;
	campmic_bus("S2MPB03 #1 (i2c@1)", 0x03900000UL, 94, 95);
	campmic_bus("S2MPB03 #2 (i2c@2)", 0x03500000UL, 47, 48);
}

/*
 * (r5q Test 271, boot-arg camsensor=1) One camera sensor powered, its chip ID read, everything put back: DT
 * qcom,cam-sensor@2 (slave 0x5A on CCI 1 master 0 = GPIO31 SDA / 32 SCL, NORTH; CSIPHY 2), chosen because its MCLK is
 * 19.2 MHz, TCXO itself, no camera PLL. Its supplies: vio PM8150 L14 1.8 V (RPMh "ldoa14", a proxy-enabled supply,
 * probably on already), vana S2MPB03 #2 LDO6 2.8 V (reg 0x08: 1.8 V + n x 25 mV, n 0x28), vdig S2MPB03 #2 LDO2 1.05 V
 * (reg 0x04: 0.7 V + n x 12.5 mV, n 0x1c); MCLK1 = camcc (0xad00000) RCG 0x5024 from TCXO (source 0, divider 1),
 * branch 0x503c, on GPIO14 (NORTH, function 1 cam_mclk, 4 mA); reset GPIO22 (EAST, GPIO, high = out of reset).
 * The sensor's I2C is bit-banged on its CCI pins as GPIOs (pull-up as the DT), not through the CCI. The chip ID:
 * 16-bit register 0x0000, 2 bytes (Samsung's sensors). Then: reset low, MCLK off, both LDOs back as found (off).
 */
/*
 * (r5q Test 277) The camera lines also into the loader's log ring, which outlives a hang and the watchdog's reset
 * (Test 276 hung with nothing of the camera in the console's log), as redwood_camera.c's say() does.
 */
extern void loader_log_putc(char c);

static bool rxlog_fast;         /* (Test 290) no pause after the step lines: a session for iOS waits on them */
static void rxlog(const char *format, ...) __printflike(1, 2);

static void
rxlog(const char *format, ...)
{
	char line[360];
	va_list list;
	uint64_t now_ns = 0;
	int at;

	/* (Test 312) every camera line stamped: microseconds since boot and the processor, to time a hang to its step */
	absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
	at = snprintf(line, sizeof(line), "[%llu.%06llu c%d] ", now_ns / 1000000000ull, (now_ns / 1000ull) % 1000000ull,
	    cpu_number());
	va_start(list, format);
	vsnprintf(line + at, sizeof(line) - at, format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	/*
	 * (Test 300, cam_steplog_ms=N) every camera line followed by N ms: the PC fetches the log every ~2 s, and a hang that
	 * stops the whole phone (the loader's USB core too) loses whatever was not fetched yet
	 */
	static uint32_t steplog_ms = ~0u;

	if (steplog_ms == ~0u && !PE_parse_boot_argn("cam_steplog_ms", &steplog_ms, sizeof(steplog_ms))) {
		steplog_ms = 0;
	}
	if (steplog_ms != 0 && steplog_ms != ~0u) {
		delay_for_interval(steplog_ms, NSEC_PER_MSEC);
		return;
	}
	if (strncmp(line + at, "camera rx: step", 15) == 0 && !rxlog_fast) {
		delay_for_interval(300, NSEC_PER_MSEC);         /* (Test 278: the line out first; 300 ms again in Test 284, the probe in its own thread) */
	}
}

static bool camsensor_on, camsensor_done, camrx_on;
/* (Test 290) a session for the camera driver (redwood_camera.c): each done frame handed over, until it says stop */
static bool (*rx_session_cb)(const uint8_t *frame, uint32_t bytes, void *ctx);
static void *rx_session_ctx;
static uint32_t camrx_level;    /* (Test 278) how far cam_rx_up() goes: 1 rails/GCC/PLL0, 2 RCGs/branches, 3 CPAS, 4 CSI-PHY, 5 CSID */
extern bool redwood_r5q_rpmh_enable(const char *id);
extern bool redwood_spmi_peek(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count);

/* 16-bit register address, 1 or 2 data bytes (most significant first) */
static const char *
bb_write16(uint16_t reg, uint16_t value, unsigned data_bytes)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put((uint8_t)(reg >> 8)) || !bus_put((uint8_t)reg)) {
		problem = "no acknowledge of the register number";
	} else if ((data_bytes == 2 && !bus_put((uint8_t)(value >> 8))) || !bus_put((uint8_t)value)) {
		problem = "no acknowledge of the value";
	}
	bus_stop();
	return problem;
}

/*
 * (r5q Test 275) One of the sensor's tables (RWCTAB02, pipeline/camsensor_v14.py, in the root's /fw) written over the
 * bit-banged bus; false at the first write not acknowledged.
 */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static bool
cam_table(const uint8_t *tab, uint64_t size, const char *want)
{
	const uint8_t *at = tab + 8 + 32, *end = tab + size;
	uint32_t modes = *(const uint32_t *)(tab + 8 + 28), tables;

	at += 48 * modes;
	for (int p = 0; p < 2 && at + 12 <= end; p++) {
		at += 12 + *(const uint32_t *)(at + 8) * 12;
	}
	if (at + 4 > end) {
		return false;
	}
	tables = *(const uint32_t *)at;
	at += 4;
	for (uint32_t t = 0; t < tables && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);
		const uint8_t *e = at + 12;

		if (strncmp((const char *)at, want, 8) == 0) {
			for (uint32_t i = 0; i < n && e + 12 <= end; i++, e += 12) {
				uint16_t a = *(const uint16_t *)e, v = *(const uint16_t *)(e + 2);
				uint32_t delay = *(const uint32_t *)(e + 8);
				const char *problem = bb_write16(a, v, e[5] == 2 ? 2 : 1);

				if (problem != NULL) {
					rxlog("camera sensor: table %s write %u (%#x = %#x): %s\n", want, i, a, v, problem);
					return false;
				}
				if (delay != 0) {
					spin_us(delay > 50000 ? 50000 : delay);
				}
			}
			rxlog("camera sensor: table %s: %u writes taken\n", want, n);
			return true;
		}
		at = e + 12 * n;
	}
	rxlog("camera sensor: no table %s\n", want);
	return false;
}

/*
 * (r5q Test 276) The camera's receive path, as far as the CSID (wifi/CAMERA-RX-SPEC.md (a)-(d)): rails, GCC's camera
 * AXI clocks, camcc PLL0 (Trion, 1200 MHz), the RCGs and branches, CSI-PHY 2 (v1.1, 4-lane D-PHY, settle 14), and
 * CSID-lite 0's RX (PHY 2, lanes 0x3210) and RDI0 (RAW10, dt 0x2b). Nothing is written to memory: the CSID's own
 * counters and status say whether frames arrive. cam_rx_down() undoes it.
 */
extern int redwood_r5q_rpmh_level(const char *id, unsigned vlvl);
static vm_offset_t rx_cc, rx_gcc, rx_phy, rx_csid, rx_cpas;
static bool rx_pll_ours;
/*
 * (Test 317) The apps SMMU's lock (redwood_wifi.c), shared with Wi-Fi, the sound and the donor camera code. Wi-Fi writes
 * TLBIASID + TLBSYNC to the apps SMMU whenever it maps memory for the radio; a TLB sync goes to every TBU, the camera's
 * too. Tests 311-316 hung at the camera's power and SMMU transitions with Wi-Fi joined; the one run that survived (313,
 * t295) had no Wi-Fi. The camera takes this lock around its GDSC power-up and collapse, the memory stage's bring-up
 * (camera NoC, AXI, the SMMU match) and its teardown, so none of them overlaps a TLB sync.
 */
extern lck_mtx_t redwood_smmu_lock;
/* (Test 334) the sensor's CSI-PHY (2: the rear S5K3L6; 3: the front IMX616) and its settle count */
static uint32_t cam_phy_index = 2, cam_phy_settle = 14;
static uint32_t rx_reached;      /* (Test 278) the last stage cam_rx_up() finished: cam_rx_down() undoes only those */

/*
 * (Test 334) The CSI-PHY's own clocks follow its index, 0x24 apart in camcc (SM8150: csiNphytimer RCG 0x6004, its
 * branch 0x601c, csiphyN 0x6020 for N = 0): the code names PHY 2's (0x604c-0x606b) and CC()/CCA() move them.
 */
static inline uint32_t
cam_phy_clk(uint32_t o)
{
	return (o >= 0x604c && o < 0x6070) ? o + 0x24u * cam_phy_index - 0x48u : o;     /* its RCG (cmd, cfg) and branches */
}
#define CC(o)   (*(volatile uint32_t *)(rx_cc + cam_phy_clk(o)))
#define CCA(o)  ((volatile uint32_t *)(rx_cc + cam_phy_clk(o)))
#define PHY(o)  (*(volatile uint32_t *)(rx_phy + (o)))
#define CSID(o) (*(volatile uint32_t *)(rx_csid + (o)))

static bool
rx_wait(volatile uint32_t *reg, uint32_t mask, uint32_t want, unsigned us_max)
{
	for (unsigned us = 0; us < us_max; us += 10) {
		if ((*reg & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return (*reg & mask) == want;
}

static bool cam_rx_memory_up(void);
static void cam_rx_memory_poll(void);
static void cam_rx_memory_down(void);

static bool
cam_rx_up(void)
{
	static const uint32_t rcgs[][2] = {
		/* (Test 282) Tests 280-281: enabling ife_lite_0_csid_clk (0xc038) at 400 MHz (PLL0 odd) hung the system; the CSID
		 * and IFE-lite from TCXO (19.2 MHz, cfg 1) instead: enough for registers and counters */
		{ 0x7058, 0x20e }, { 0xa064, 0x301 }, { 0x604c, 0x203 }, { 0xc020, 0x001 }, { 0xc004, 0x001 },
	};
	/* (Test 280) Test 279 hung right after camnoc_axi (RCG 0xc170, branch 0xc18c) came on, at the next camcc access:
	 * left out until the IFE writes memory (the CSID alone does not need the camera NoC) */
	static const uint32_t branches[] = { 0xc168, 0x6068, 0x6064, 0xc038, 0xc040, 0xc01c };
	static const uint32_t lanes[] = { 0x000, 0x700, 0x200, 0x400, 0x600 };     /* DL0, CLK, DL1, DL2, DL3 */
	static const uint8_t irq_masks[] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t settle = cam_phy_settle, mode, opmode, nopll = 0, pll600 = 0;

	/* (Test 285) Tests 279-284 hung ~3 s after PLL0 locked, whatever the step or thread: cam_nopll=1 keeps PLL0 off */
	(void)PE_parse_boot_argn("cam_nopll", &nopll, sizeof(nopll));
	(void)PE_parse_boot_argn("cam_pll600", &pll600, sizeof(pll600));
	if (rx_session_cb != NULL || cam_phy_index != 2) {
		/* (Test 290) a session: the proven setting (Tests 287-289); (Test 334) and the front sensor's probe, which hung
		 * without it, as Tests 279-286 did (slow_ahb on PLL0) */
		pll600 = 1;
	}

	(void)PE_parse_boot_argn("cam_settle", &settle, sizeof(settle));
	rx_cc = ml_io_map(0x0ad00000UL, 0x10000);
	rx_gcc = ml_io_map(0x0010b000UL, 0x1000);
	rx_phy = ml_io_map(0x0ac65000UL + 0x1000UL * cam_phy_index, 0x1000);     /* (Test 334) CSI-PHY 0-3 */
	rx_csid = ml_io_map(0x0acc8000UL, 0x1000);
	rx_cpas = ml_io_map(0x0ac40000UL, 0x1000);
	if (rx_cc == 0 || rx_gcc == 0 || rx_phy == 0 || rx_csid == 0 || rx_cpas == 0) {
		rxlog("camera rx: no mapping\n");
		return false;
	}
	/* rails: MMCX >= SVS (IFE-lite at 400 MHz), MX >= SVS_L1 (PLL0 at 1200 MHz), ldoc3 (the CSI-PHY's 1.2 V) */
	rxlog("camera rx: rails: mmcx %d, mx %d, ldoc3 %s\n", redwood_r5q_rpmh_level("mmcx.lvl", 128),
	    redwood_r5q_rpmh_level("mx.lvl", 192), redwood_r5q_rpmh_enable("ldoc3") ? "on" : "NOT ON");
	/* GCC: the camera's AXI (hf, sf) */
	rxlog("camera rx: step: GCC AXI\n");
	/* (Test 281) Tests 279-280 hung about 3 s after these came on, at different steps: the camera AXI path without the
	 * camera NoC. Not needed for the CSID (register access only): only from stage 6 (the IFE writing memory). */
	if (camrx_level >= 6) {
		*(volatile uint32_t *)(rx_gcc + 0x30) |= 1;
		*(volatile uint32_t *)(rx_gcc + 0x34) |= 1;
	}
	spin_us(100);
	rxlog("camera rx: GCC camera hf_axi %#x sf_axi %#x xo %#x\n", *(volatile uint32_t *)(rx_gcc + 0x30),
	    *(volatile uint32_t *)(rx_gcc + 0x34), *(volatile uint32_t *)(rx_gcc + 0x44));
	/* PLL0 (Trion): not ours if running or in voting mode */
	rxlog("camera rx: step: PLL0 read\n");
	mode = CC(0x0);
	opmode = CC(0x38);
	if (nopll) {
		rxlog("camera rx: cam_nopll=1: PLL0 left off; every RCG from TCXO (19.2 MHz)\n");
	} else if (((mode & 1) && (opmode & 1)) || (mode & (1u << 20))) {
		rxlog("camera rx: PLL0 already running or voted (mode %#x opmode %#x): left as it is\n", mode, opmode);
	} else {
		/*
		 * (Test 286) Test 285: no hang with PLL0 off; Tests 279-284 hung ~3 s after it locked at 1200 MHz. cam_pll600=1:
		 * 600 MHz (L 0x1f, alpha 0x4000: 19.2 x 31.25), which needs MX at SVS only; even 300, odd 200 MHz.
		 */
		CC(0x04) = pll600 ? 0x1f : 0x3e;
		CC(0x08) = 0x44;
		CC(0x40) = pll600 ? 0x4000 : 0x8000;
		CC(0x18) = 0x20485699;
		CC(0x1c) = 0x2267;
		CC(0x20) = 0x24;
		CC(0x0c) = 0x3100;
		CC(0x10) = 0x805;
		CC(0x14) = 0xd0;
		CC(0x2c) = 0x20;
		CC(0x0) |= 1u << 23;
		CC(0x0) &= ~1u;
		CC(0x38) = 0;
		CC(0x0) |= 1u << 2;
		CC(0x38) = 1;
		if (!rx_wait((volatile uint32_t *)(rx_cc + 0x0), 1u << 31, 1u << 31, 2000)) {
			rxlog("camera rx: PLL0 does not lock (mode %#x status %#x): stopped\n", CC(0x0), CC(0x30));
			return false;
		}
		CC(0x0c) |= 7;
		CC(0x0) |= 1;
		rx_pll_ours = true;
		rxlog("camera rx: PLL0 locked: mode %#x opmode %#x user_ctl %#x\n", CC(0x0), CC(0x38), CC(0x0c));
	}
	rx_reached = 1;
	if (camrx_level < 2) {
		rxlog("camera rx: camrx=%u: stopped after PLL0\n", camrx_level);
		return false;
	}
	rxlog("camera rx: step: RCGs\n");
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		rxlog("camera rx: step: RCG %#x\n", cam_phy_clk(rcgs[i][0]));
		uint32_t cfg = nopll ? 0x001u : rcgs[i][1];

		if (pll600 && !nopll) {
			/* (Test 286) PLL0 at 600: slow_ahb 300/4 = 75 MHz; cphy_rx, CSID, IFE-lite odd 200 / 1; timer even 300 / 1 */
			/* (Test 287) Test 286 hung as before with the RCGs on PLL0 (285, all on TCXO, did not): slow_ahb (the camera's
			 * register bus) kept on TCXO */
			cfg = rcgs[i][0] == 0x7058 ? 0x001u : rcgs[i][0] == 0x604c ? 0x201u : rcgs[i][0] == 0xa064 ? 0x301u :
			    (rcgs[i][0] == 0xc020 || rcgs[i][0] == 0xc004) ? 0x301u : rcgs[i][1];
		}
		CC(rcgs[i][0] + 4) = (CC(rcgs[i][0] + 4) & ~0x371fu) | cfg;
		CC(rcgs[i][0]) |= 1;
		(void)rx_wait(CCA(rcgs[i][0]), 1, 0, 500);
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		rxlog("camera rx: step: branch %#x\n", cam_phy_clk(branches[i]));
		CC(branches[i]) |= 1;
		(void)rx_wait(CCA(branches[i]), 1u << 31, 0, 2000);
	}
	rxlog("camera rx: RCG cmd: cphy_rx %#x timer %#x ife_lite %#x csid %#x; branches csiphy2 %#x timer %#x csid %#x cphy %#x ife %#x "
	    "cpas_ahb %#x camnoc %#x\n", CC(0xa064), CC(0x604c), CC(0xc004), CC(0xc020), CC(0x6068), CC(0x6064), CC(0xc038), CC(0xc040),
	    CC(0xc01c), CC(0xc168), CC(0xc18c));
	if ((CC(0xc168) | CC(0x6068) | CC(0xc038) | CC(0xc040)) & (1u << 31)) {
		rxlog("camera rx: a clock of the path is off: no camera block is touched\n");
		return false;
	}
	rx_reached = 2;
	if (camrx_level < 3) {
		rxlog("camera rx: camrx=%u: stopped after the clocks\n", camrx_level);
		return false;
	}
	rxlog("camera rx: step: CPAS read\n");
	rxlog("camera rx: CPAS: camera version %#x, CPAS version %#x\n", *(volatile uint32_t *)(rx_cpas + 0),
	    *(volatile uint32_t *)(rx_cpas + 4));
	/* CSI-PHY 2: reset, common, the lanes, the interrupt masks */
	rx_reached = 3;
	if (camrx_level < 4) {
		rxlog("camera rx: camrx=%u: stopped after CPAS\n", camrx_level);
		return false;
	}
	rxlog("camera rx: step: CSI-PHY %u\n", cam_phy_index);
	PHY(0x814) = 0; spin_us(5000);
	PHY(0x818) = 0; PHY(0x81c) = 0;
	PHY(0x800) = 1; spin_us(1000); PHY(0x800) = 0;
	PHY(0x814) = 0xd5; PHY(0x818) = 1; PHY(0x81c) = 2;
	PHY(0x800) = 1; spin_us(1000); PHY(0x800) = 0;
	for (unsigned l = 0; l < 5; l++) {
		uint32_t b = lanes[l];
		bool clk = b == 0x700;

		PHY(b + 0x30) = 0; PHY(b + 0x04) = 0x0c; PHY(b + 0x20) = 0; PHY(b + 0x2c) = 1; PHY(b + 0x34) = 0x0f;
		PHY(b + 0x1c) = 0x0a; PHY(b + 0x14) = 0x60; PHY(b + 0x28) = clk ? 0x04 : 0; PHY(b + 0x3c) = 0xb8;
		PHY(b + 0x00) = clk ? 0x80 : 0x90; PHY(b + 0x08) = settle & 0xff;
		if (clk) {
			PHY(b + 0x0c) = 0x16;
		}
		PHY(b + 0x10) = 0x52; PHY(b + 0x38) = clk ? 0x1f : 0xfe;
		PHY(0x800) = 2;
	}
	for (unsigned i = 0; i < 11; i++) {
		PHY(0x82c + 4 * i) = irq_masks[i];
		PHY(0x858 + 4 * i) = 0xff;
	}
	PHY(0x828) = 1; PHY(0x828) = 0;
	rxlog("camera rx: CSI-PHY %u configured (lane enable %#x, settle %u)\n", cam_phy_index, PHY(0x814), settle);
	/* CSID-lite 0: reset, RX from PHY 2, RDI0 RAW10 */
	rx_reached = 4;
	if (camrx_level < 5) {
		rxlog("camera rx: camrx=%u: stopped after the CSI-PHY\n", camrx_level);
		return false;
	}
	rxlog("camera rx: step: CSID\n");
	CSID(0x024) = 0; CSID(0x034) = 0; CSID(0x044) = 0; CSID(0x054) = 0; CSID(0x064) = 0;
	CSID(0x078) = 1; CSID(0x028) = 0x0fffffff; CSID(0x038) = 0x7fff; CSID(0x048) = 0x7fff; CSID(0x058) = 0x7fff;
	CSID(0x068) = 0x7fff; CSID(0x080) = 1;
	CSID(0x104) = 0x80; CSID(0x200) = 2; CSID(0x300) = 2; CSID(0x400) = 2; CSID(0x500) = 2;
	CSID(0x010) = 0x1e;
	rxlog("camera rx: CSID hw reset %s\n", rx_wait((volatile uint32_t *)(rx_csid + 0x070), 1, 1, 100000) ? "done" : "NOT DONE");
	CSID(0x078) = 1; CSID(0x080) = 1;
	CSID(0x010) = 0x1;
	rxlog("camera rx: CSID sw reset %s; version %#x\n", rx_wait((volatile uint32_t *)(rx_csid + 0x070), 1, 1, 100000) ?
	    "done" : "NOT DONE", CSID(0x000));
	CSID(0x078) = 1; CSID(0x080) = 1;
	CSID(0x100) = 0x00032103 | (cam_phy_index << 20);     /* (Test 334) phy_sel << 20: 0x232103 for PHY 2 */
	CSID(0x104) = 0x41;
	CSID(0x200) = 0x002bf007;
	CSID(0x204) = 2;
	CSID(0x210) = 1; CSID(0x20c) = 0; CSID(0x218) = 1; CSID(0x214) = 0;
	CSID(0x224) = 0; CSID(0x228) = 1; CSID(0x22c) = 0; CSID(0x230) = 1;
	CSID(0x208) = 0;
	CSID(0x200) |= 1u << 31;
	CSID(0x108) = 0;
	CSID(0x208) = 1;
	CSID(0x028) = 0x0fffffff; CSID(0x038) = 0x7fff; CSID(0x080) = 1;
	rx_reached = 5;
	rxlog("camera rx: CSID RX cfg %#x %#x, RDI0 cfg %#x ctrl %#x\n", CSID(0x100), CSID(0x104), CSID(0x200), CSID(0x208));
	if (camrx_level >= 6) {
		/*
		 * (Test 305) Bandwidth votes for the IFE's writes: camnoc HF -> MM1, mnoc -> MM0 (MM2 the SF side), LLCC SH0,
		 * DDR MC0, as Linux's camera votes through the interconnect. Test 304 held the receive side alone (no memory
		 * writes) 150 s without a hang; every session that wrote memory for long enough hung the whole phone. Kept
		 * once cast: this voter may hold the boot's display votes on the same BCMs (never voted back to 0).
		 */
		static bool bw_voted;
		extern uint32_t redwood_r5q_rpmh_bcm(const char *id, uint64_t avg_kbps, uint64_t peak_kbps, uint32_t mask);

		uint32_t bw_on = 0;

		/*
		 * (Test 311) Opt-in (cam_bw=1): with the votes cast right before the IFE's first writes, every unpaced session
		 * soak on kernel t305 died at its start (Tests 306, 309, 310), and the votes had fixed nothing (Test 305).
		 */
		(void)PE_parse_boot_argn("cam_bw", &bw_on, sizeof(bw_on));
		if (!bw_voted && bw_on != 0) {
			uint32_t mm0 = redwood_r5q_rpmh_bcm("MM0", 500000, 2000000, 0), mm1 = redwood_r5q_rpmh_bcm("MM1", 500000, 2000000, 0);
			uint32_t mm2 = redwood_r5q_rpmh_bcm("MM2", 500000, 2000000, 0), sh0 = redwood_r5q_rpmh_bcm("SH0", 500000, 2000000, 0);
			uint32_t mc0 = redwood_r5q_rpmh_bcm("MC0", 500000, 2000000, 0);

			rxlog("camera rx: bandwidth votes: MM0 %#x MM1 %#x MM2 %#x SH0 %#x MC0 %#x (kept)\n", mm0, mm1, mm2, sh0, mc0);
			bw_voted = mm0 != 0 && mm1 != 0 && sh0 != 0 && mc0 != 0;
		}
		/* (Test 288) the camera NoC (camnoc_axi: PLL0 even 300 / 2 = 150 MHz with cam_pll600) for the IFE's writes */
		lck_mtx_lock(&redwood_smmu_lock);         /* (Test 317) */
		rxlog("camera rx: step: camera NoC (SMMU lock taken)\n");
		CC(0xc170 + 4) = (CC(0xc170 + 4) & ~0x371fu) | (nopll ? 0x001u : pll600 ? 0x203u : 0x207u);
		CC(0xc170) |= 1;
		(void)rx_wait((volatile uint32_t *)(rx_cc + 0xc170), 1, 0, 500);
		/* (Test 327) cam_cc_camnoc_dcd_xo_clk (0xc194: the camera NoC's dynamic clock divider's crystal) on first */
		CC(0xc194) |= 1;
		(void)rx_wait((volatile uint32_t *)(rx_cc + 0xc194), 1u << 31, 0, 2000);
		rxlog("camera rx: camnoc RCG cmd %#x cfg %#x; dcd_xo %#x; now the branch\n", CC(0xc170), CC(0xc174), CC(0xc194));
		CC(0xc18c) |= 1;
		(void)rx_wait((volatile uint32_t *)(rx_cc + 0xc18c), 1u << 31, 0, 2000);
		rxlog("camera rx: camnoc RCG %#x cfg %#x branch %#x; step: memory\n", CC(0xc170), CC(0xc174), CC(0xc18c));
		if (cam_rx_memory_up()) {
			rx_reached = 6;
		}
		lck_mtx_unlock(&redwood_smmu_lock);
	}
	return true;
}

/*
 * (r5q Test 288, camrx=6) One frame into memory (wifi/CAMERA-RX-SPEC.md (e), (f)): the CSID's RDI0 decoding RAW10 to
 * 16 bits a pixel and cropping 1024 x 16 (32 KB), IFE-lite 0's write master 0 in frame mode writing it to the loader's
 * low block (lowdma_pa, 64 KB below 4 GB; IMAGE_ADDR is 32 bits) with FRAME_INC 0 (every frame on the same 32 KB: no
 * walk through memory), the SMMU matching the IFE streams (SMR 0x86200840) with the S2CR of a boot bank that does
 * not translate (the IFE then writes physical addresses), the camera NoC and AXI on. The write master is stopped at
 * the first frame done; the match is taken away only after it is off and a frame time has passed.
 */
#include <vm/memory_types.h>             /* (Test 288) VM_WIMG_WCOMB */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);

static vm_offset_t rx_ife, rx_smmu;
static volatile uint8_t *rx_buf;
static uint64_t rx_buf_pa;
static uint32_t rx_buf_size;
static uint32_t rx_smr = ~0u, rx_s2cr_was;
/*
 * (Test 301) The SMMU match is put in once and kept for later sessions. Taking it away while the IFE could still write
 * (its write master's "off" waits for a frame start, which a stopped sensor never sends) let a later write go out as
 * an unmatched stream: Test 300 hung in the teardown, 36 s after the match was gone with the IFE still armed.
 */
static uint32_t rx_smr_kept = ~0u;
static bool rx_frame_done;

#define IFE(o)  (*(volatile uint32_t *)(rx_ife + (o)))

static bool
cam_rx_memory_up(void)
{
	uint32_t idr0, idr1, n, numcb, page, numpage, free_smr = ~0u, share = ~0u;

	/* (Test 289) camrx=7: whole frames into the loader's 4 MB camera block (camdma_pa); camrx=6: the cropped 32 KB one */
	rx_buf_size = camrx_level >= 7 ? 0x400000u : 0x10000u;
	if (!PE_parse_boot_argn(camrx_level >= 7 ? "camdma_pa" : "lowdma_pa", &rx_buf_pa, sizeof(rx_buf_pa)) || rx_buf_pa == 0 ||
	    rx_buf_pa + rx_buf_size > 0x100000000ull) {
		rxlog("camera rx: no %s below 4 GB from the loader: no memory stage\n", camrx_level >= 7 ? "camdma_pa" : "lowdma_pa");
		return false;
	}
	rx_buf = (volatile uint8_t *)ml_io_map_unmappable(rx_buf_pa, rx_buf_size, VM_WIMG_WCOMB);
	rx_ife = ml_io_map(0x0acc4000UL, 0x4000);
	rx_smmu = ml_io_map_unmappable(0x15000000UL, 0x100000, VM_WIMG_IO);
	if (rx_buf == NULL || rx_ife == 0 || rx_smmu == 0) {
		rxlog("camera rx: no mapping for the memory stage\n");
		return false;
	}
	for (uint32_t i = 0; i < rx_buf_size; i++) {
		rx_buf[i] = 0xa5;               /* (what the IFE writes stands out against it) */
	}
	/* the SMMU: SID 0x840 mask 0x620 (all the IFEs' streams) in a free SMR, the S2CR of a non-translating boot bank */
	if (rx_smr_kept != ~0u && *(volatile uint32_t *)(rx_smmu + 0x800 + 4 * rx_smr_kept) == 0x86200840u) {
		rx_smr = rx_smr_kept;
		rxlog("camera rx: smmu: SMR%u %#x kept from an earlier session (S2CR %#x); buffer %#llx\n", rx_smr,
		    *(volatile uint32_t *)(rx_smmu + 0x800 + 4 * rx_smr), *(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * rx_smr), rx_buf_pa);
		goto smr_ready;
	}
	idr0 = *(volatile uint32_t *)(rx_smmu + 0x20);
	idr1 = *(volatile uint32_t *)(rx_smmu + 0x24);
	n = idr0 & 0xff;
	numcb = idr1 & 0xff;
	page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	numpage = 1u << (((idr1 >> 28) & 7) + 1);
	for (uint32_t i = 0; i < n && i < 128; i++) {
		uint32_t smr = *(volatile uint32_t *)(rx_smmu + 0x800 + 4 * i), s2cr = *(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * i);
		uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff, cb = s2cr & 0xff;

		if (!(smr & 0x80000000u)) {
			if (i < 70) {
				free_smr = i;   /* (the highest free below 70: Wi-Fi takes the lowest, SLIMbus below 80, SLPI the highest) */
			}
			continue;
		}
		if ((0x840 & ~0x620 & ~m) == (id & ~0x620 & ~m) && ((id | m) & 0x7c0) >= 0x800) {
			rxlog("camera rx: smmu: SMR%u %#x (S2CR %#x) may already match the IFE's streams: no memory stage\n", i, smr, s2cr);
			return false;
		}
		if (share == ~0u && ((s2cr >> 16) & 3) == 0 && cb < numcb &&
		    !(*(volatile uint32_t *)(rx_smmu + (vm_offset_t)numpage * page + (vm_offset_t)cb * page) & 1) &&
		    *(volatile uint32_t *)(rx_smmu + page + 4 * cb) == 0x1f000u) {
			share = i;
		}
	}
	if (free_smr == ~0u || share == ~0u) {
		rxlog("camera rx: smmu: no free SMR (%#x) or no non-translating bank to share (%#x)\n", free_smr, share);
		return false;
	}
	rx_smr = free_smr;
	rx_s2cr_was = *(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * free_smr);
	*(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * free_smr) = *(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * share);
	*(volatile uint32_t *)(rx_smmu + 0x800 + 4 * free_smr) = 0x86200840u;
	__asm__ volatile ("dsb sy" ::: "memory");
	rxlog("camera rx: smmu: SMR%u %#x, S2CR %#x (as SMR%u's); buffer %#llx (64 KB, 0xa5)\n", free_smr,
	    *(volatile uint32_t *)(rx_smmu + 0x800 + 4 * free_smr), *(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * free_smr), share,
	    rx_buf_pa);
	rx_smr_kept = rx_smr;
smr_ready:
	/* the CSID's RDI0 again: PLAIN16_10 with a 1024 x 16 crop (its path is halted and set up anew); camrx=7 keeps RAW10 */
	CSID(0x208) = 2;
	if (camrx_level < 7) {
		CSID(0x200) = 0x002b2467;
		CSID(0x21c) = (1023u << 16) | 0;
		CSID(0x220) = (15u << 16) | 0;
	}
	CSID(0x200) |= 1u << 31;
	/* IFE-lite 0: reset, bus init, write master 0 in frame mode */
	IFE(0x05c) = 0x80000000u;
	IFE(0x018) = 0x3f9f;
	if (!rx_wait((volatile uint32_t *)(rx_ife + 0x06c), 1u << 31, 1u << 31, 100000)) {
		rxlog("camera rx: IFE-lite reset NOT DONE (status %#x)\n", IFE(0x06c));
		return false;
	}
	IFE(0x064) = 0xffffffffu; IFE(0x068) = 0xffffffffu; IFE(0x058) = 1;
	IFE(0x2080) = 0; IFE(0x200c) = 0xfffff; IFE(0x211c) = 0; IFE(0x2084) = 0xffffff;
	IFE(0x225c) = 0xf; IFE(0x221c) = 0xff01; IFE(0x2220) = 0; IFE(0x2224) = 0; IFE(0x2228) = 0xff01;
	IFE(0x2250) = 0; IFE(0x2254) = 1; IFE(0x2248) = 0; IFE(0x224c) = 1;
	IFE(0x2214) = (uint32_t)rx_buf_pa;
	IFE(0x2258) = 0;                                /* FRAME_INC 0: every frame on the same buffer */
	IFE(0x2050) = 0xffffffffu; IFE(0x2054) = 0xffffffffu; IFE(0x2058) = 0xffffffffu; IFE(0x2068) = 1;
	IFE(0x2208) = 0x3;                              /* WM0: enable, frame based */
	IFE(0x04ac) = 0x2;                              /* register update, RDI0 */
	CSID(0x208) = 1;                                /* RDI0 on again, at the frame boundary */
	rxlog("camera rx: IFE-lite version %#x, bus %#x; WM0 cfg %#x addr %#x; CSID RDI0 cfg %#x\n", IFE(0x000), IFE(0x2000),
	    IFE(0x2208), IFE(0x2214), CSID(0x200));
	return true;
}

/* while streaming: the first frame done stops the write master */
static void
cam_rx_memory_poll(void)
{
	if (rx_ife == 0 || rx_frame_done) {
		return;
	}
	for (int i = 0; i < 2000 && !rx_frame_done; i++) {
		if (IFE(0x2060) & 1) {
			IFE(0x2208) = 0;
			IFE(0x04ac) = 0x2;
			rx_frame_done = true;
			rxlog("camera rx: FRAME DONE: bus status0 %#x 1 %#x 2 %#x, WM0 status %#x %#x, top status %#x %#x, violation %#x\n",
			    IFE(0x205c), IFE(0x2060), IFE(0x2064), IFE(0x2200), IFE(0x2204), IFE(0x06c), IFE(0x070), IFE(0x07c));
		} else {
			spin_us(10);
		}
	}
	if (!rx_frame_done) {
		rxlog("camera rx: no frame done yet: bus status %#x %#x %#x, top %#x %#x, WM0 %#x %#x, violation %#x, comp %#x\n",
		    IFE(0x205c), IFE(0x2060), IFE(0x2064), IFE(0x06c), IFE(0x070), IFE(0x2200), IFE(0x2204), IFE(0x07c), IFE(0x206c));
	}
}

static void
cam_rx_memory_down(void)
{
	if (rx_ife != 0) {
		IFE(0x2208) = 0;
		IFE(0x04ac) = 0x2;
	}
	delay_for_interval(20, NSEC_PER_MSEC);          /* (more than a frame at 120 a second, before the match goes) */
	if (rx_smmu != 0 && rx_smr != ~0u) {
		uint32_t keep = 0;

		/*
		 * (Test 315) Keeping the match (Test 301) is opt-in again (cam_smrkeep=1): Test 314 hung bringing its second
		 * session up with the match kept through the GDSC's collapse and power-up; t295 (taking it away) did not.
		 */
		(void)PE_parse_boot_argn("cam_smrkeep", &keep, sizeof(keep));
		if (keep != 0) {
			rxlog("camera rx: smmu: SMR%u kept; global fault status %#x\n", rx_smr, *(volatile uint32_t *)(rx_smmu + 0x48));
		} else {
			*(volatile uint32_t *)(rx_smmu + 0x800 + 4 * rx_smr) = 0;
			*(volatile uint32_t *)(rx_smmu + 0xc00 + 4 * rx_smr) = rx_s2cr_was;
			__asm__ volatile ("dsb sy" ::: "memory");
			rx_smr_kept = ~0u;
			rxlog("camera rx: smmu: SMR%u taken away; global fault status %#x\n", rx_smr,
			    *(volatile uint32_t *)(rx_smmu + 0x48));
		}
		rx_smr = ~0u;
	}
	if (rx_buf != NULL && rx_buf_size > 0x10000) {
		/*
		 * (Test 289) A whole RAW10 frame: how much was written, where the writing ends, and a 64 x 24 picture of it in
		 * characters (each cell one pixel's 8 high bits: MIPI RAW10 packs 4 pixels in 5 bytes, the first 4 their high
		 * bytes), at a stride of 1296 bytes a line.
		 */
		static const char shades[] = " .:-=+*#%@";
		uint32_t changed = 0, last = 0, sum = 0, cells = 0;
		char row[72];

		for (uint32_t i = 0; i < rx_buf_size; i++) {
			if (rx_buf[i] != 0xa5) {
				changed++;
				last = i;
			}
		}
		for (uint32_t gy = 0; gy < 24; gy++) {
			uint32_t y = gy * 32 + 16, x;

			for (uint32_t gx = 0; gx < 64; gx++) {
				uint8_t v;

				x = gx * 16 + 8;
				v = rx_buf[y * 1296 + (x / 4) * 5 + (x % 4)];
				sum += v;
				cells++;
				row[gx] = shades[v * 10 / 256];
			}
			row[64] = 0;
			rxlog("camera rx: |%s|\n", row);
		}
		rxlog("camera rx: the whole frame: %u of %u bytes written, the last at %#x (one frame: 1296 x 772 = %#x); mean "
		    "high byte %u\n", changed, rx_buf_size, last, 1296 * 772, cells ? sum / cells : 0);
	} else if (rx_buf != NULL) {
		uint32_t changed = 0, lo = 0xffff, hi = 0;
		char line[200];
		int at;

		for (uint32_t i = 0; i < 0x10000; i++) {
			changed += rx_buf[i] != 0xa5;
		}
		for (uint32_t i = 0; i < 0x8000; i += 2) {
			uint32_t v = rx_buf[i] | rx_buf[i + 1] << 8;

			if (v < lo) {
				lo = v;
			}
			if (v > hi) {
				hi = v;
			}
		}
		at = snprintf(line, sizeof(line), "camera rx: the buffer: %u of 65536 bytes written; pixels %u..%u; first:", changed, lo, hi);
		for (uint32_t i = 0; i < 24 && at < (int)sizeof(line) - 4; i++) {
			at += snprintf(line + at, sizeof(line) - at, " %02x", rx_buf[i]);
		}
		rxlog("%s\n", line);
		at = snprintf(line, sizeof(line), "camera rx: line 8:");
		for (uint32_t i = 0; i < 24 && at < (int)sizeof(line) - 4; i++) {
			at += snprintf(line + at, sizeof(line) - at, " %02x", rx_buf[8 * 2048 + 512 + i]);
		}
		rxlog("%s; past the frame (32 KB on): %02x %02x %02x %02x\n", line, rx_buf[0x8000], rx_buf[0x8001], rx_buf[0x9000],
		    rx_buf[0xffff]);
	}
}

static void
cam_rx_show(const char *when)
{
	if (rx_csid == 0 || rx_reached < 5) {
		return;
	}
	rxlog("camera rx: %s: RX irq %#x, packets %u, ECC %#x, CRC errors %u; RDI0 irq %#x status %#x, bytes %u/%u; "
	    "PHY status %#x %#x %#x\n", when, CSID(0x020), CSID(0x160), CSID(0x164), CSID(0x168), CSID(0x030), CSID(0x250),
	    CSID(0x2e0), CSID(0x2e4), PHY(0x8b0), PHY(0x8b4), PHY(0x8b8));
}

static void
cam_rx_down(void)
{
	static const uint32_t branches[] = { 0xc01c, 0xc040, 0xc038, 0x6064, 0x6068, 0xc18c, 0xc168 };

	lck_mtx_lock(&redwood_smmu_lock);         /* (Test 317) the teardown apart from any TLB sync */
	/* (Test 312) a line before every block of the teardown: a hang names its block */
	if (rx_reached >= 6 || rx_smr != ~0u) {      /* (also when the memory stage stopped after its match) */
		rxlog("camera rx: down 1: memory stage (WM0, SMMU match)\n");
		cam_rx_memory_down();           /* (Test 288) the write master off and the SMMU match away first */
		rxlog("camera rx: down 2: camnoc branch 0xc18c %#x off, then dcd_xo 0xc194 %#x\n", CC(0xc18c), CC(0xc194));
		CC(0xc18c) &= ~1u;
		CC(0xc194) &= ~1u;                      /* (Test 327) */
	}
	if (rx_csid != 0 && rx_reached >= 5) {
		rxlog("camera rx: down 3: CSID (RX %#x, RDI0 %#x)\n", CSID(0x100), CSID(0x200));
		CSID(0x034) = 0;
		CSID(0x208) = (CSID(0x208) & ~3u) | 2;
		CSID(0x200) &= ~(1u << 31);
		CSID(0x100) = 0;
	}
	if (rx_phy != 0 && rx_reached >= 4) {
		rxlog("camera rx: down 4: CSI-PHY (lanes %#x)\n", PHY(0x814));
		PHY(0x814) = 0; PHY(0x818) = 0; PHY(0x81c) = 0;
		PHY(0x800) = 1; spin_us(1000); PHY(0x800) = 0;
	}
	if (rx_cc != 0) {
		for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
			rxlog("camera rx: down 5: camcc branch %#x (%#x) off\n", cam_phy_clk(branches[i]), CC(branches[i]));
			CC(branches[i]) &= ~1u;
		}
		/*
		 * (Test 327) The RCGs fed by PLL0 parked on the crystal (cfg 0x001: source XO, divider 1) before PLL0 goes off,
		 * as Linux's shared RCG ops do. Turning PLL0 off under them left their next switch (the next session's bring-up)
		 * starting from a source killed beneath it: Test 326's third bring-up hung exactly at the camera NoC's RCG
		 * (0xc170) and branch, after two good power cycles.
		 */
		{
			static const uint32_t pll_rcgs[] = { 0xc170, 0xc004, 0xc020, 0x604c, 0xa064 };

			for (unsigned i = 0; i < sizeof(pll_rcgs) / sizeof(pll_rcgs[0]); i++) {
				CC(pll_rcgs[i] + 4) = (CC(pll_rcgs[i] + 4) & ~0x371fu) | 0x001u;
				CC(pll_rcgs[i]) |= 1;
				(void)rx_wait(CCA(pll_rcgs[i]), 1, 0, 500);
				rxlog("camera rx: down 5b: RCG %#x parked on the crystal: cmd %#x cfg %#x\n", cam_phy_clk(pll_rcgs[i]),
				    CC(pll_rcgs[i]), CC(pll_rcgs[i] + 4));
			}
		}
		if (rx_pll_ours) {
			rxlog("camera rx: down 6: PLL0 (mode %#x) off\n", CC(0x0));
			CC(0x0) &= ~1u;
			CC(0x0c) &= ~7u;
			CC(0x38) = 0;
			CC(0x0) &= ~(1u << 2);
			rx_pll_ours = false;
		}
	}
	if (rx_gcc != 0) {
		rxlog("camera rx: down 7: GCC camera AXI (hf %#x sf %#x) off\n", *(volatile uint32_t *)(rx_gcc + 0x30),
		    *(volatile uint32_t *)(rx_gcc + 0x34));
		*(volatile uint32_t *)(rx_gcc + 0x30) &= ~1u;
		*(volatile uint32_t *)(rx_gcc + 0x34) &= ~1u;
	}
	/*
	 * (Test 292) The MMCX and MX votes stay. Voting them to 0 here (Tests 287-291) replaced the boot's own vote from
	 * this same voter: camcc lost power (Test 291's second session read every camcc register as 0), and MX at 0 is
	 * the likely cause of Test 290's hang after its session.
	 */
	rxlog("camera rx: down (MMCX and MX votes kept; SMMU lock given back)\n");
	lck_mtx_unlock(&redwood_smmu_lock);
}

/* 16-bit register address, `count` bytes */
static const char *
bb_read16(uint16_t reg, uint8_t *bytes, unsigned count)
{
	const char *problem = NULL;

	bus_start();
	if (!bus_put(bb->address << 1)) {
		problem = "no acknowledge of its address";
	} else if (!bus_put((uint8_t)(reg >> 8)) || !bus_put((uint8_t)reg)) {
		problem = "no acknowledge of the register number";
	} else {
		bus_start();
		if (!bus_put(bb->address << 1 | 1)) {
			problem = "no acknowledge of its address for the read";
		} else {
			for (unsigned i = 0; i < count; i++) {
				bytes[i] = bus_get(i + 1 == count);
			}
		}
	}
	bus_stop();
	return problem;
}

/*
 * (Test 322) A PERSISTENT PIPELINE. Every hang of Tests 290-321 came at a power or teardown transition (a session's
 * start or end, iOS's camera switch, the Camera app opening), while streaming itself held for minutes (148 s in Test
 * 321; 150 s holds in 304/307/308). iOS opens, switches and closes the camera all the time, so the camera now comes up
 * once and stays up: a session only starts and stops handing frames to iOS; the sensor keeps streaming and IFE-lite
 * keeps writing into the camera block in between. cam_persist=0 brings back a teardown after every session.
 */
static bool cam_pipe_up;

/*
 * (Test 329) The pipeline stays up only across iOS's camera switches; on a close, or when iOS stops reading (the app
 * left), it is taken down and powered off -- safe since Test 327. redwood_camera.c says which before it ends a session
 * (redwood_r5q_cam_keep). What the probe's power-down needs is kept here when it leaves the pipeline up.
 */
static bool cam_keep_next = true;
static struct {
	const uint8_t *tab;
	uint64_t tab_size;
	struct bitbang pm;
	vm_offset_t north, east, camcc, camcc_c;
	uint32_t was31, was32, was22, was22io, was14, was47, was48, was_gdscr, was_gdsc_clk;
	uint8_t ldo2, ldo6;
} pipe_keep;

void redwood_r5q_cam_keep(bool keep);
void
redwood_r5q_cam_keep(bool keep)
{
	cam_keep_next = keep;
}

/*
 * (Test 328) iOS's exposure on the S5K3L6: redwood_camera.c passes on what iOS's auto-exposure asks for
 * (REDWOOD_CAMERA_EXPOSURE) as lines, the sensor's analog gain (SMIA 0x0204, 32 = 1x) and frame lines; the session
 * applies it between two frames over the sensor's own bus, under the grouped-parameter hold (0x0104). The bus: a kept
 * copy of the probe's (whose own lives on the probe's stack), valid while the sensor is powered.
 */
static struct bitbang cam_bus_keep;
static bool cam_bus_ok;
static volatile bool exp_pending;
static volatile uint32_t exp_lines, exp_gain, exp_frame;
static uint32_t exp_applied;
/*
 * (Test 340) The front camera (the IMX616) streaming for iOS: the exposure turned into its terms. iOS's numbers stay in
 * the S5K3L6's (a line 9.19 us, frames from 3268 lines at ~30 a second): the IMX616's line is ~7.29 us (frame length
 * 5160 at ~26.6 a second, MCLK 19.2 MHz), so lines and frame lengths are scaled by 919/729 and 5160/3268; its analog
 * gain (SMIA 0x0204) is Sony's code 1024 - 1024 / gain (0 = 1x, 960 = 16x), its coarse time at most frame - 48.
 */
/* (Test 342) the Sony sensor streaming now (sony_stream), whose terms iOS's exposure is put in; NULL: the S5K3L6 */
struct sony_cam {
	const char *name, *tab, *mode;
	uint16_t crop_x, crop_y, frame_lines, line_length;        /* line_length 0: the mode's own */
	uint16_t width, height;                                    /* (Test 350) the frame: the crop's size */
	uint32_t phy, settle, iop_mpy;
	uint32_t line_num, line_den, frame_num, frame_den, margin; /* iOS's lines x num / den; its frame lines likewise */
};
static const struct sony_cam *cam_live;

void redwood_r5q_cam_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines);
void
redwood_r5q_cam_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	exp_lines = lines;
	exp_gain = gain;
	exp_frame = frame_lines;
	exp_pending = true;
}

static void
cam_exposure_apply(void)
{
	struct bitbang *was;
	uint32_t fl, ln, g;
	const char *problem = NULL;

	if (!exp_pending || !cam_bus_ok) {
		return;
	}
	exp_pending = false;
	fl = exp_frame < 3268 ? 3268 : exp_frame > 0xfff0 ? 0xfff0 : exp_frame;    /* (~30 a second at the shortest) */
	ln = exp_lines < 4 ? 4 : exp_lines > fl - 8 ? fl - 8 : exp_lines;
	g = exp_gain < 32 ? 32 : exp_gain > 512 ? 512 : exp_gain;                  /* 1x .. 16x */
	uint32_t w_fl = fl, w_ln = ln, w_g = g;

	if (cam_live != NULL) {
		w_fl = fl * cam_live->frame_num / cam_live->frame_den;
		w_fl = w_fl > 0xfff0 ? 0xfff0 : w_fl;
		w_ln = ln * cam_live->line_num / cam_live->line_den;
		w_ln = w_ln < 4 ? 4 : w_ln > w_fl - cam_live->margin ? w_fl - cam_live->margin : w_ln;
		w_g = 1024u - 1024u * 32u / g;
	}
	was = bb;
	bb = &cam_bus_keep;
	if ((problem = bb_write16(0x0104, 1, 1)) == NULL && (problem = bb_write16(0x0340, (uint16_t)w_fl, 2)) == NULL &&
	    (problem = bb_write16(0x0202, (uint16_t)w_ln, 2)) == NULL && (problem = bb_write16(0x0204, (uint16_t)w_g, 2)) == NULL) {
		problem = bb_write16(0x0104, 0, 1);
	}
	bb = was;
	if (exp_applied++ < 12 || exp_applied % 30 == 0 || problem != NULL) {
		rxlog("camera sensor: exposure from iOS: %u lines (%u.%02u ms), gain %u/32, frame %u lines: %s\n", ln,
		    ln * 919 / 100000, (ln * 919 / 1000) % 100, g, fl, problem ? problem : "taken");
	}
}

static uint32_t
cam_session_frames(uint32_t *misses_out)
{
	/*
	 * (Test 290) A session for iOS: two frames in the camera block taken in turn (the write master pointed at the other
	 * one as soon as one is done; it takes the new address at the next frame's start, the 75 % blanking of the longer
	 * frame leaves time), each done one handed to the caller, until it says stop.
	 */
	uint32_t frames = 0, misses = 0;
	static uint32_t half;
	/*
	 * (Test 350) The frame's size from the sensor streaming: 1028x772 (1,000,512 bytes) for the S5K3L6 and the crops,
	 * the Sony sensors' whole binned frames for iOS (IMX586 2000x1500: 3,768,000 bytes; IMX616 1632x1224: 2,506,752).
	 * Lines at a 16-byte stride as the write master leaves them. Two halves of the 4 MB camera block while a frame fits
	 * one (1 or 2 MB); a bigger one in one place, rewritten each frame: it is copied out at frame-done, ~1-2 ms, well
	 * inside the vertical blanking before the next frame's first line.
	 */
	uint32_t w = cam_live != NULL ? cam_live->width : 1028, h = cam_live != NULL ? cam_live->height : 772;
	uint32_t bytes = ((w * 10 / 8 + 15) & ~15u) * h;
	uint32_t place = bytes <= 0x100000u ? 0x100000u : bytes <= 0x200000u ? 0x200000u : 0;

	if (place == 0) {
		half = 0;
		IFE(0x2214) = (uint32_t)rx_buf_pa;
		IFE(0x04ac) = 0x2;
	}

	rxlog("camera sensor: SESSION: frames for iOS from the %s%s\n", cam_live != NULL ? cam_live->name : "S5K3L6 (rear)", cam_pipe_up ? " (the pipeline was already up)" : "");
	while (misses < 50) {
		int i;

		for (i = 0; i < 1000 && !(IFE(0x2060) & 1); i++) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
		if (!(IFE(0x2060) & 1)) {
			misses++;
			continue;
		}
		IFE(0x2054) = 1;
		IFE(0x2068) = 1;
		IFE(0x2214) = (uint32_t)(rx_buf_pa + place * (half ^ 1));
		IFE(0x04ac) = 0x2;
		frames++;
		misses = 0;
		if (frames % 30 == 0) {
			/* (Test 312) a line a second: the IFE's bus/top/violation status, the CSID, the SMMU */
			rxlog("camera sensor: SESSION: %u frames; IFE bus st0 %#x st1 %#x st2 %#x top %#x/%#x violation %#x WM0 st "
			    "%#x/%#x addr %#x; CSID pkts %u ecc %#x crc %u; SMMU gfsr %#x\n", frames, IFE(0x205c), IFE(0x2060),
			    IFE(0x2064), IFE(0x06c), IFE(0x070), IFE(0x07c), IFE(0x2200), IFE(0x2204), IFE(0x2214), CSID(0x160),
			    CSID(0x164), CSID(0x168), rx_smmu ? *(volatile uint32_t *)(rx_smmu + 0x48) : 0);
		}
		if (!rx_session_cb((const uint8_t *)(rx_buf + place * half), bytes, rx_session_ctx)) {
			break;
		}
		half = place != 0 ? half ^ 1 : 0;
		cam_exposure_apply();           /* (Test 328) iOS's exposure, between two frames */
	}
	*misses_out = misses;
	return frames;
}

static void
camsensor_probe(void)
{
	struct bitbang pm = { .sda = 47, .scl = 48, .address = 0x56 }, cam = { .sda = 31, .scl = 32, .address = 0x5a }, *was = bb;
	vm_offset_t north = ml_io_map(0x03900000UL, 0x1000UL * 33), east = ml_io_map(0x03500000UL, 0x1000UL * 49);
	vm_offset_t camcc = ml_io_map(0x0ad00000UL + 0x5000, 0x1000);
	uint32_t was47, was48, was14, was22, was22io, was31, was32, was_gdsc_clk = 0, was_gdscr = 0;
	vm_offset_t camcc_c = 0;
	uint8_t ldo2 = 0, ldo6 = 0, l14[2] = { 0 }, id[2] = { 0 };
	const char *problem = NULL;
	int us;

	if (!camsensor_on || camsensor_done || north == 0 || east == 0 || camcc == 0) {
		return;
	}
	camsensor_done = true;
	{
		/* camcc's registers are reached through GCC_CAMERA_AHB_CLK (0x10b008): unclocked, a read could hang the bus */
		vm_offset_t gccb = ml_io_map(0x0010b000UL, 0x1000);
		uint32_t ahb = gccb ? *(volatile uint32_t *)(gccb + 0x8) : 0x80000000u;

		/*
		 * (Test 272) Test 271 read 0x80000003: enabled (bit 0) under hardware gating (bit 1), which reads "off" while
		 * idle and runs on access (Linux gcc-sm8150: hwcg_bit 1). camcc's power, MMCX, is the display's too (on).
		 * So what is required is "enabled", not "running".
		 */
		rxlog("camera sensor: GCC_CAMERA_AHB_CBCR %#x (bit 0 enabled, bit 1 hardware-gated, bit 31 idle/off)\n", ahb);
		if ((ahb & 1) == 0) {
			rxlog("camera sensor: the camera clock controller's bus clock is not enabled: nothing done\n");
			return;
		}
		/*
		 * (Test 302) Tests 299-301 hung at a camera register access in a teardown, each time at a different one, and
		 * every session's log showed GCC_CAMERA_XO_CBCR (0x10b044) at 0x80000000: the camera's crystal clock not
		 * enabled -- while slow_ahb, the camera's register bus, runs from that crystal (Test 287). Linux keeps the
		 * camera's GCC AHB and XO clocks on for good (critical clocks). Now: the XO branch enabled and the AHB branch
		 * forced on (its hardware gating, bit 1, off), and both left so (the next session finds them on).
		 */
		uint32_t gccfix = 0;

		/*
		 * (Test 314) Opt-in (cam_gccfix=1). Test 313: kernel t295 (before this) ran a 150 s session, 4500 frames, no
		 * hang; t312 (with it) hung inside its first session's frames. Forcing the camera AHB's clock on (its
		 * hardware gating off) is the prime suspect.
		 */
		(void)PE_parse_boot_argn("cam_gccfix", &gccfix, sizeof(gccfix));
		if (gccb != 0 && gccfix != 0) {
			*(volatile uint32_t *)(gccb + 0x44) |= 1u;
			*(volatile uint32_t *)(gccb + 0x8) = (*(volatile uint32_t *)(gccb + 0x8) & ~(1u << 1)) | 1u;
			for (us = 0; us < 1000 && (*(volatile uint32_t *)(gccb + 0x44) & (1u << 31)); us += 10) {
				spin_us(10);
			}
			rxlog("camera sensor: GCC camera XO %#x, AHB %#x (bit 31 = off; both kept on from here)\n",
			    *(volatile uint32_t *)(gccb + 0x44), *(volatile uint32_t *)(gccb + 0x8));
		}
	}
	/* vio: L14 (PM8150 slave 1, LDO14 at 0x4d00: status 0x08, enable 0x46), then the vote */
	(void)redwood_spmi_peek(0x14d, 0x08, &l14[0], 1);
	(void)redwood_spmi_peek(0x14d, 0x46, &l14[1], 1);
	rxlog("camera sensor: L14 status %#04x enable %#04x before; vote: %s\n", l14[0], l14[1],
	    redwood_r5q_rpmh_enable("ldoa14") ? "done" : "NOT DONE");
	/* vana, vdig: S2MPB03 #2 */
	pm.tlmm = east;
	bb = &pm;
	was47 = rd(BB_CFG(47));
	was48 = rd(BB_CFG(48));
	wr(BB_CFG(47), 0);
	wr(BB_CFG(48), 0);
	spin_us(50);
	if (!bb_recover("S2MPB03 #2") || (problem = bb_read(0x04, &ldo2, 1)) != NULL || (problem = bb_read(0x08, &ldo6, 1)) != NULL) {
		rxlog("camera sensor: S2MPB03 #2: %s: nothing switched on\n", problem ? problem : "the bus stays low");
		goto pins_back;
	}
	rxlog("camera sensor: S2MPB03 #2 LDO2 %#04x LDO6 %#04x as found; LDO6 on at 2.8 V, then LDO2 on at 1.05 V\n", ldo2, ldo6);
	if ((problem = bb_write(0x08, 0x80 | 0x28)) != NULL) {
		goto power_back;
	}
	spin_us(1000);
	if ((problem = bb_write(0x04, 0x80 | 0x1c)) != NULL) {
		goto power_back;
	}
	spin_us(2000);
	{
		uint8_t now2 = 0, now6 = 0;

		(void)bb_read(0x04, &now2, 1);
		(void)bb_read(0x08, &now6, 1);
		rxlog("camera sensor: S2MPB03 #2 LDO2 %#04x LDO6 %#04x now\n", now2, now6);
	}
	/*
	 * (Test 273) Test 272: the MCLK RCG stayed "root off" (cmd 0x80000000) with its branch enabled, and L14 read off
	 * before the vote (not read after). Now: L14 read again; camcc's GDSC clock (CAM_CC_GDSC_CLK 0xc1e4, Linux's
	 * critical one) on and titan_top_gdsc (GDSCR 0xc1bc: bit 0 SW_COLLAPSE, bit 31 PWR_ON) powered before the MCLK;
	 * both put back afterwards.
	 */
	(void)redwood_spmi_peek(0x14d, 0x08, &l14[0], 1);
	(void)redwood_spmi_peek(0x14d, 0x46, &l14[1], 1);
	rxlog("camera sensor: L14 status %#04x enable %#04x after the vote\n", l14[0], l14[1]);
	/*
	 * (Test 292) camcc lives on MMCX. Tests 270-290 found it powered by the boot's vote; Test 291's second session
	 * found it dead (every camcc register 0) after the first session's teardown had voted MMCX to 0. Our own vote
	 * now comes first, and it stays (cam_rx_down no longer lowers it).
	 */
	rxlog("camera sensor: rails before camcc: mmcx %d, mx %d\n", redwood_r5q_rpmh_level("mmcx.lvl", 128),
	    redwood_r5q_rpmh_level("mx.lvl", 192));
	camcc_c = ml_io_map(0x0ad00000UL + 0xc000, 0x1000);
	if (camcc_c != 0) {
		was_gdsc_clk = *(volatile uint32_t *)(camcc_c + 0x1e4);
		was_gdscr = *(volatile uint32_t *)(camcc_c + 0x1bc);
		lck_mtx_lock(&redwood_smmu_lock);     /* (Test 317) the GDSC's power-up apart from any TLB sync */
		{
			/*
			 * (Test 318) The multimedia NoC's SMMU TBUs powered by our vote, and kept: hlos1_vote_mmnoc_mmu_tbu_hf0 /
			 * sf / hf1 (GCC 0x17d050 / 0x17d054 / 0x17d058, live DT). The IFE's streams (0x840 mask 0x620) cross
			 * mnoc_hf_0_tbu and mnoc_hf_1_tbu. Linux's arm-smmu powers every TBU before TLB maintenance: a TLB sync
			 * (Wi-Fi's TLBIASID + TLBSYNC on every radio mapping) that meets a TBU in a power transition never ends.
			 * Tests 311-317 hung with the camera powered while r5qd/Wi-Fi was busy. Bit 0 SW_COLLAPSE (0 = our vote
			 * on), bit 31 PWR_ON.
			 */
			static const struct { uint32_t off; const char *name; } tbus[] = {
				{ 0x7d050, "mnoc tbu hf0" }, { 0x7d054, "mnoc tbu sf" }, { 0x7d058, "mnoc tbu hf1" },
			};
			static vm_offset_t gcc_tbu;

			if (gcc_tbu == 0) {
				gcc_tbu = ml_io_map(0x0017d000UL, 0x1000);
			}
			for (unsigned i = 0; gcc_tbu != 0 && i < sizeof(tbus) / sizeof(tbus[0]); i++) {
				volatile uint32_t *g = (volatile uint32_t *)(gcc_tbu + (tbus[i].off & 0xfff));
				uint32_t before = *g;

				*g = before & ~1u;
				for (us = 0; us < 5000 && !(*g & (1u << 31)); us += 10) {
					spin_us(10);
				}
				rxlog("camera sensor: %s GDSC %#x -> %#x (bit 31 = powered; our vote kept)\n", tbus[i].name, before, *g);
			}
		}
		*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk | 1;
		*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr & ~1u;
		for (us = 0; us < 10000 && !(*(volatile uint32_t *)(camcc_c + 0x1bc) & (1u << 31)); us += 10) {
			spin_us(10);
		}
		lck_mtx_unlock(&redwood_smmu_lock);
		rxlog("camera sensor: CAM_CC_GDSC_CLK %#x -> %#x; titan_top_gdsc %#x -> %#x (bit 31 = powered)\n", was_gdsc_clk,
		    *(volatile uint32_t *)(camcc_c + 0x1e4), was_gdscr, *(volatile uint32_t *)(camcc_c + 0x1bc));
	}
	/* MCLK1: 19.2 MHz from TCXO; GPIO14 cam_mclk */
	*(volatile uint32_t *)(camcc + 0x024 + 4) = 1;                  /* CFG: TCXO, divider 1 */
	*(volatile uint32_t *)(camcc + 0x024) |= 1;                     /* CMD: update */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x024) & 1); us += 10) {
		spin_us(10);
	}
	*(volatile uint32_t *)(camcc + 0x03c) |= 1;                     /* the branch on */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x03c) & (1u << 31)); us += 10) {
		spin_us(10);
	}
	was14 = rd(north + 0x1000UL * 14);
	wr(north + 0x1000UL * 14, (1u << 2) | (1u << 6));               /* cam_mclk, 4 mA, no pull */
	rxlog("camera sensor: MCLK1: RCG cmd %#x cfg %#x, branch %#x (bit 31 = off); GPIO14 %#x\n",
	    *(volatile uint32_t *)(camcc + 0x024), *(volatile uint32_t *)(camcc + 0x028), *(volatile uint32_t *)(camcc + 0x03c),
	    rd(north + 0x1000UL * 14));
	/* reset: GPIO22 out high */
	was22 = rd(east + 0x1000UL * 22);
	was22io = rd(east + 0x1000UL * 22 + 4);
	wr(east + 0x1000UL * 22 + 4, 0);
	wr(east + 0x1000UL * 22, (1u << 9));                            /* GPIO, output (low first) */
	spin_us(1000);
	wr(east + 0x1000UL * 22 + 4, 2);                                /* high: out of reset */
	spin_us(10000);
	/* the chip ID over the CCI pins, bit-banged */
	cam.tlmm = north;
	bb = &cam;
	was31 = rd(BB_CFG(31));
	was32 = rd(BB_CFG(32));
	cam.cfg_sda = 3;                                                /* GPIO, pull-up, 2 mA, input */
	cam.cfg_scl = 3;
	wr(BB_CFG(31), 3);
	wr(BB_CFG(32), 3);
	spin_us(50);
	/*
	 * (Test 274) Test 273: power, the power domain and MCLK all on, still no acknowledge at 0x5A. Qualcomm's camera
	 * DTs give the 8-bit (write) address: 0x5A is 7-bit 0x2D. Both are tried, each at 0x0000 and 0x0016, and a scan
	 * of the whole bus says who is there at all.
	 */
	problem = bb_recover("the sensor's bus") ? NULL : "the bus stays low";
	if (problem == NULL) {
		static const uint8_t tries[] = { 0x2d, 0x5a };
		char seen[160];
		int at = 0;

		for (unsigned a = 0x08; a < 0x78 && at < (int)sizeof(seen) - 6; a++) {
			uint8_t b;

			cam.address = (uint8_t)a;
			if (bb_read(0x00, &b, 1) == NULL || bb_read16(0x0000, &b, 1) == NULL) {
				at += snprintf(seen + at, sizeof(seen) - at, " %#04x", a);
			}
		}
		rxlog("camera sensor: the bus GPIO31/32 answers at:%s\n", at ? seen : " nothing");
		problem = "no answer";
		for (unsigned t = 0; t < sizeof(tries) && problem != NULL; t++) {
			cam.address = tries[t];
			if ((problem = bb_read16(0x0000, id, 2)) == NULL || (problem = bb_read16(0x0016, id, 2)) == NULL) {
				uint64_t tab_size = 0;
				const uint8_t *tab = redwood_image_file("fw", "camera-s5k3l6.tab", &tab_size, NULL);

				rxlog("camera sensor: THE SENSOR ANSWERS at 7-bit %#04x: chip ID %02x%02x (CCI 1 master 0)\n", tries[t],
				    id[0], id[1]);
				/*
				 * (Test 275) Its own tables: init, mode 2 (1028x772 RAW10, 120 frames a second), stream on; its
				 * SMIA frame counter (0x0005) read a few times; stream off. Nothing receives the frames yet: this
				 * shows the sensor runs on its tables.
				 */
				if (tab == NULL || tab_size < 48 || memcmp(tab, "RWCTAB02", 8) != 0) {
					rxlog("camera sensor: no camera-s5k3l6.tab in the root's /fw: no stream test\n");
				} else if (cam_table(tab, tab_size, "init") && cam_table(tab, tab_size, "res2")) {
					/*
					 * (Test 289) Test 288's frame was black (pixels 63-66: the black level): an exposure of our own, by
					 * SMIA's registers, coarse integration time 0x0202 = 768 lines (the mode's frame is 817) and analogue
					 * gain 0x0204 = 0x100 (Samsung's 0x20 = 1x: about 8x). cam_exp=/cam_gain= set them instead.
					 */
					uint32_t exp_lines = 768, gain = 0x100;
					bool rx;

					(void)PE_parse_boot_argn("cam_exp", &exp_lines, sizeof(exp_lines));
					(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
					rxlog("camera sensor: exposure %u lines: %s; gain %#x: %s\n", exp_lines,
					    bb_write16(0x0202, (uint16_t)exp_lines, 2) ?: "taken", gain, bb_write16(0x0204, (uint16_t)gain, 2) ?: "taken");
					if (rx_session_cb != NULL) {
						/* (Test 290) about 30 frames a second: frame length 3268 lines (the mode's 817 x 4) */
						rxlog("camera sensor: frame length 3268 lines: %s\n", bb_write16(0x0340, 3268, 2) ?: "taken");
					}
					/* (Test 276) the receive path before the stream starts, as Android orders it */
					rx = camrx_on && cam_rx_up();

					cam_bus_keep = cam;            /* (Test 328) for iOS's exposure from the session */
					cam_bus_ok = true;
					if (rx_session_cb != NULL && rx && rx_reached >= 6 && cam_table(tab, tab_size, "on")) {
						uint32_t frames, misses = 0, persist = 1;

						/*
						 * (Test 293) The battery's lock let go while frames flow: the loop touches only IFE-lite
						 * registers and the caller's ring, never the PMIC bus.
						 */
						lck_mtx_unlock(&r5q_pmic_lock);
						frames = cam_session_frames(&misses);
						lck_mtx_lock(&r5q_pmic_lock);
						(void)PE_parse_boot_argn("cam_persist", &persist, sizeof(persist));
						if (persist != 0 && cam_keep_next) {
							/*
							 * (Test 322) The pipeline stays up: no stream off, no teardown, no power-down. The pins stay
							 * the bit-banged bus's and `bb` the sensor's; the next session goes straight to its frames.
							 * (Test 329) Only across a camera switch; what the power-down needs is kept for later.
							 */
							pipe_keep.tab = tab;
							pipe_keep.tab_size = tab_size;
							pipe_keep.pm = pm;
							pipe_keep.north = north;
							pipe_keep.east = east;
							pipe_keep.camcc = camcc;
							pipe_keep.camcc_c = camcc_c;
							pipe_keep.was31 = was31;
							pipe_keep.was32 = was32;
							pipe_keep.was22 = was22;
							pipe_keep.was22io = was22io;
							pipe_keep.was14 = was14;
							pipe_keep.was47 = was47;
							pipe_keep.was48 = was48;
							pipe_keep.was_gdscr = was_gdscr;
							pipe_keep.was_gdsc_clk = was_gdsc_clk;
							pipe_keep.ldo2 = ldo2;
							pipe_keep.ldo6 = ldo6;
							cam_pipe_up = true;
							rxlog("camera sensor: SESSION ends: %u frames handed over, %u misses at the end; THE PIPELINE "
							    "STAYS UP (sensor streaming, IFE-lite writing)\n", frames, misses);
							return;
						}
						/*
						 * (Test 301) The write master off while the sensor still streams: its "off" is taken at the
						 * next frame start (register update), which a stopped sensor never sends. 50 ms = more than a
						 * frame at ~30 a second.
						 */
						uint32_t wm0first = 0;

						/* (Test 315) opt-in again (cam_wm0first=1): the t295 order (the stream's off first) ran 150 s */
						(void)PE_parse_boot_argn("cam_wm0first", &wm0first, sizeof(wm0first));
						if (wm0first != 0) {
							IFE(0x2208) = 0;
							IFE(0x04ac) = 0x2;
							delay_for_interval(50, NSEC_PER_MSEC);
						}
						rxlog("camera sensor: SESSION ends: %u frames handed over, %u misses at the end\n", frames, misses);
						(void)cam_table(tab, tab_size, "off");
					} else if (cam_table(tab, tab_size, "on")) {
						/*
						 * (Test 304, cam_hold=N) streaming held for N seconds (a line a second): with camrx=5, the receive
						 * side alone (PLL0, CSI-PHY, CSID) with no IFE, camera NoC or AXI writes
						 */
						uint32_t hold = 0;

						(void)PE_parse_boot_argn("cam_hold", &hold, sizeof(hold));
						for (uint32_t s = 0; s < hold; s++) {
							delay_for_interval(1000, NSEC_PER_MSEC);
							if (rx) {
								char when[24];

								snprintf(when, sizeof(when), "held %u s", s + 1);
								cam_rx_show(when);
							}
						}
						for (int r = 0; r < 4; r++) {
							uint8_t fc = 0, mode = 0;

							spin_us(25000);
							(void)bb_read16(0x0005, &fc, 1);
							(void)bb_read16(0x0100, &mode, 1);
							rxlog("camera sensor: streaming? frame counter %u, mode_select %#x\n", fc, mode);
							if (rx) {
								cam_rx_show("streaming");
								if (rx_reached >= 6) {
									cam_rx_memory_poll();
								}
							}
						}
						(void)cam_table(tab, tab_size, "off");
					}
					if (rx || camrx_on) {
						spin_us(20000);
						cam_rx_show("stopped");
						cam_rx_down();
					}
				}
			} else {
				rxlog("camera sensor: 7-bit %#04x: %s\n", tries[t], problem);
			}
		}
	} else {
		rxlog("camera sensor: GPIO31/32: %s\n", problem);
	}
	rxlog("camera sensor: down 8: pins GPIO31/32 back (%#x %#x)\n", was31, was32);
	cam_bus_ok = false;                     /* (Test 328) the sensor goes down: no more exposure writes */
	wr(BB_CFG(31), was31);
	wr(BB_CFG(32), was32);
	/* everything back: reset low, the pin and MCLK off */
	rxlog("camera sensor: down 9: reset GPIO22 low, back to %#x/%#x; GPIO14 back to %#x\n", was22, was22io, was14);
	wr(east + 0x1000UL * 22 + 4, 0);
	spin_us(1000);
	wr(east + 0x1000UL * 22, was22);
	wr(east + 0x1000UL * 22 + 4, was22io);
	wr(north + 0x1000UL * 14, was14);
	rxlog("camera sensor: down 10: MCLK1 branch (%#x) off\n", *(volatile uint32_t *)(camcc + 0x03c));
	*(volatile uint32_t *)(camcc + 0x03c) &= ~1u;
	if (camcc_c != 0) {                                             /* (Test 273) the power domain back as found */
		rxlog("camera sensor: down 11: titan_top_gdsc %#x -> %#x, GDSC clock -> %#x\n",
		    *(volatile uint32_t *)(camcc_c + 0x1bc), was_gdscr, was_gdsc_clk);
		lck_mtx_lock(&redwood_smmu_lock);     /* (Test 317) the GDSC's collapse apart from any TLB sync */
		*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr;
		*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk;
		spin_us(100);
		lck_mtx_unlock(&redwood_smmu_lock);
		rxlog("camera sensor: titan_top_gdsc back: %#x\n", *(volatile uint32_t *)(camcc_c + 0x1bc));
	}
	bb = &pm;
	problem = NULL;
power_back:
	if (problem != NULL) {
		rxlog("camera sensor: S2MPB03 #2 write: %s\n", problem);
	}
	(void)bb_write(0x04, ldo2);
	spin_us(1000);
	(void)bb_write(0x08, ldo6);
	{
		uint8_t now2 = 0, now6 = 0;

		(void)bb_read(0x04, &now2, 1);
		(void)bb_read(0x08, &now6, 1);
		rxlog("camera sensor: S2MPB03 #2 back: LDO2 %#04x LDO6 %#04x\n", now2, now6);
	}
pins_back:
	wr(BB_CFG(47), was47);
	wr(BB_CFG(48), was48);
	bb = was;
}

/*
 * (Test 329) The kept pipeline taken down, as the probe's own end does it: the sensor's stream off, the receive path
 * down (its RCGs parked before PLL0 goes: Test 327), the pins, reset and MCLK back, the GDSC collapsed, the S2MPB03's
 * LDOs back as found. The caller holds r5q_pmic_lock.
 */
static void
cam_pipe_down(void)
{
	struct bitbang *was = bb;
	vm_offset_t north = pipe_keep.north, east = pipe_keep.east, camcc = pipe_keep.camcc, camcc_c = pipe_keep.camcc_c;

	if (!cam_pipe_up) {
		return;
	}
	rxlog("camera sensor: THE PIPELINE GOES DOWN (iOS closed the camera or stopped reading)\n");
	bb = &cam_bus_keep;
	(void)cam_table(pipe_keep.tab, pipe_keep.tab_size, "off");
	spin_us(20000);
	cam_rx_show("stopped");
	cam_rx_down();
	cam_bus_ok = false;
	rxlog("camera sensor: down 8: pins GPIO31/32 back (%#x %#x)\n", pipe_keep.was31, pipe_keep.was32);
	wr(BB_CFG(31), pipe_keep.was31);
	wr(BB_CFG(32), pipe_keep.was32);
	rxlog("camera sensor: down 9: reset GPIO22 low, back to %#x/%#x; GPIO14 back to %#x\n", pipe_keep.was22,
	    pipe_keep.was22io, pipe_keep.was14);
	wr(east + 0x1000UL * 22 + 4, 0);
	spin_us(1000);
	wr(east + 0x1000UL * 22, pipe_keep.was22);
	wr(east + 0x1000UL * 22 + 4, pipe_keep.was22io);
	wr(north + 0x1000UL * 14, pipe_keep.was14);
	rxlog("camera sensor: down 10: MCLK1 branch (%#x) off\n", *(volatile uint32_t *)(camcc + 0x03c));
	*(volatile uint32_t *)(camcc + 0x03c) &= ~1u;
	if (camcc_c != 0) {
		lck_mtx_lock(&redwood_smmu_lock);
		*(volatile uint32_t *)(camcc_c + 0x1bc) = pipe_keep.was_gdscr;
		*(volatile uint32_t *)(camcc_c + 0x1e4) = pipe_keep.was_gdsc_clk;
		spin_us(100);
		lck_mtx_unlock(&redwood_smmu_lock);
		rxlog("camera sensor: down 11: titan_top_gdsc back: %#x\n", *(volatile uint32_t *)(camcc_c + 0x1bc));
	}
	bb = &pipe_keep.pm;
	(void)bb_write(0x04, pipe_keep.ldo2);
	spin_us(1000);
	(void)bb_write(0x08, pipe_keep.ldo6);
	{
		uint8_t now2 = 0, now6 = 0;

		(void)bb_read(0x04, &now2, 1);
		(void)bb_read(0x08, &now6, 1);
		rxlog("camera sensor: S2MPB03 #2 back: LDO2 %#04x LDO6 %#04x; the camera is off\n", now2, now6);
	}
	wr(BB_CFG(47), pipe_keep.was47);
	wr(BB_CFG(48), pipe_keep.was48);
	bb = was;
	cam_pipe_up = false;
}

/*
 * (Test 342) A Sony SMIA sensor's frames, once it is powered and answers on `bb' (its bus, `cam'): the part of
 * frontsensor_probe() that Tests 334-341 proved on the IMX616, made common so the main camera (the IMX586) runs the
 * same way. `c' is the sensor's: its tables, the mode, the crop to the middle 1028x772 (the frame shape the rest of
 * the pipeline takes), frame and line length, CSI-PHY, settle, the output PLL's multiplier, iOS's exposure in its
 * terms. front_level: 2 a 5 s stream, 3 a settle sweep, 4 the test pattern, 5 crops (the IMX616's), 6 an output
 * PLL sweep, 7 iOS's session.
 */
static const char *sony_name = "camera";
#define SLOG(fmt, ...) rxlog("%s: " fmt, sony_name, ##__VA_ARGS__)

static void
sony_stream(struct bitbang *cam, uint32_t front_level, const struct sony_cam *c)
{
	sony_name = c->name;
	{
	/*
	 * (Test 334, camfront=2) Its frames: init + res3 (1632x1224, 4x4 binned), the sensor's own digital crop
	 * to the middle 1028x772 (SMIA 0x0408-0x040e, offsets 306/226 even: RGGB kept) and output size
	 * (0x034c/0x034e) -- the rear sensor's frame shape, so CSID, IFE-lite, the stride and the camera block are
	 * as proven -- frame length 5160 lines (~30 a second at 24 MHz), CSI-PHY 3 with settle 12.
	 */
	uint64_t tab_size = 0;
	const uint8_t *tab = redwood_image_file("fw", c->tab, &tab_size, NULL);

	if (tab == NULL || tab_size < 48 || memcmp(tab, "RWCTAB02", 8) != 0) {
		SLOG("no fw/%s\n", c->tab);
	} else if (cam_table(tab, tab_size, "init") && cam_table(tab, tab_size, c->mode)) {
		bool rx;

		(void)bb_write16(0x0104, 1, 1);
		SLOG("crop %ux%u at %u,%u, frame %u lines, line %u: %s\n", c->width, c->height, c->crop_x, c->crop_y,
		    c->frame_lines, c->line_length, bb_write16(0x0408, c->crop_x, 2) ?: bb_write16(0x040a, c->crop_y, 2) ?:
		    bb_write16(0x040c, c->width, 2) ?: bb_write16(0x040e, c->height, 2) ?: bb_write16(0x034c, c->width, 2) ?:
		    bb_write16(0x034e, c->height, 2) ?: bb_write16(0x0340, c->frame_lines, 2) ?:
		    (c->line_length ? bb_write16(0x0342, c->line_length, 2) : NULL) ?: "taken");
		(void)bb_write16(0x0104, 0, 1);
		/*
		 * (Test 336, camfront=3) Test 335 streamed (992020 bytes a frame, headers right: ECC 0) but every packet
		 * failed its CRC (65535, RX irq bit 19): the link's timing. A sweep of the CSI-PHY's settle count, the
		 * receive path brought up and down for each (fast: safe since Test 327), 1 s of frames each, the
		 * write master re-armed after every frame as the session does. camfront=2: settle 12 only, 5 s.
		 */
		static const uint32_t settles[] = { 12, 6, 20, 28, 36, 48 };
		uint32_t tries = front_level == 3 ? sizeof(settles) / sizeof(settles[0]) : 1;

		/*
		 * (Test 337, camfront=4) Test 336: settles 12-48 all alike (26 frames a second, every image line's CRC
		 * bad, 2 packets a frame good), 6 no link: not the settle. The sensor's solid-colour test pattern
		 * (SMIA 0x0600/0x0601 = 1, colours 0x0602-0x0609) with four different 10-bit values, so every received
		 * byte has a known value: the frame checked byte by byte below, the errors counted per lane (a packet's
		 * byte k travels on lane k % 4).
		 */
		if (front_level >= 4 && front_level <= 6) {
			SLOG("test pattern: solid colour R 0x2aa Gr 0x155 B 0x0cc Gb 0x333: %s\n",
			    bb_write16(0x0602, 0x2aa, 2) ?: bb_write16(0x0604, 0x155, 2) ?: bb_write16(0x0606, 0x0cc, 2) ?:
			    bb_write16(0x0608, 0x333, 2) ?: bb_write16(0x0600, 0, 1) ?: bb_write16(0x0601, 1, 1) ?: "taken");
		}
		/*
		 * (Test 338, camfront=5) Test 337: the pattern's bytes right up to ~byte 962 of every line (pixel ~770 of
		 * 1028, three quarters across), then two bytes wrong and the rest of the line shifted; all lanes alike.
		 * A fault at a fixed place in the line: the sensor's crop, not the link. Crops tried in one boot, each
		 * checked against the pattern: {analog x start, x end, y start, y end, digital x, y, width, frame lines}.
		 */
		static const uint16_t crops[][8] = {
			{ 0, 0x199f, 0x10, 0x132f, 306, 226, 1028, 5160 },     /* Tests 334-337 */
			{ 0, 0x199f, 0x10, 0x132f, 4, 0, 1028, 5160 },         /* the digital crop at the left edge */
			{ 0, 0x199f, 0x10, 0x132f, 308, 226, 1024, 5160 },     /* 1024 wide (a multiple of 16) */
			{ 1224, 5351, 920, 4023, 2, 2, 1028, 5160 },           /* the window cut in the analog array */
			{ 0, 0x199f, 0x10, 0x132f, 306, 226, 1028, 1290 },     /* the mode's own frame length */
		};
		/*
		 * (Test 339, camfront=6) Test 338: every crop alike -- bytes dropped from the second half of each line
		 * (a group short by two bytes at ~byte 890-960, the line's tail junk "00 00 ff ff"). res3's output PLL
		 * (IOP: 0x030d pre-divider 3, 0x030e/f multiplier 0x101) makes ~2 Gb/s a lane at 24 MHz (~1.6 at our
		 * 19.2): past where D-PHY wants deskew, and faster than the line needs. The multiplier swept down.
		 */
		static const uint16_t iop_mpy[] = { 0x101, 0xc8, 0xa0, 0x80, 0x64 };

		/*
		 * (Test 340) Test 339: multipliers 200 and 160 (~1.28 and ~1.02 Gb/s a lane): the pattern without one
		 * wrong byte, CRC 0, RX irq 0xff as the rear sensor's; 128 and 100 no output at all; 257 drops bytes.
		 * 160 from now on (~1.28 Gb/s once MCLK is the tables' 24 MHz), settle 28. camfront=7 is iOS's session
		 * (redwood_r5q_cam_run with the front camera asked for): frames to rx_session_cb until it says stop.
		 */
		bool session = front_level == 7 && rx_session_cb != NULL;

		if (front_level == 5) {
			tries = sizeof(crops) / sizeof(crops[0]);
		} else if (front_level == 6) {
			tries = sizeof(iop_mpy) / sizeof(iop_mpy[0]);
		}
		rxlog_fast = true;
		for (uint32_t s = 0; s < tries; s++) {
			uint32_t line_bytes = 1285;

			cam_phy_index = c->phy;
			cam_phy_settle = front_level == 3 ? settles[s] : c->settle;
			if (front_level == 6) {
				SLOG("IOP %u: output PLL multiplier %u (~%u Mb/s a lane at 19.2 MHz): %s\n", s,
				    iop_mpy[s], 192u * iop_mpy[s] / 30u, bb_write16(0x030e, iop_mpy[s], 2) ?: "taken");
			} else {
				SLOG("output PLL multiplier %u (~%u Mb/s a lane at 19.2 MHz): %s\n", c->iop_mpy,
				    192u * c->iop_mpy / 30u, bb_write16(0x030e, c->iop_mpy, 2) ?: "taken");
			}
			if (front_level == 5) {
				const uint16_t *c = crops[s];

				line_bytes = c[6] * 5u / 4u;
				(void)bb_write16(0x0104, 1, 1);
				SLOG("CROP %u: analog x %u-%u y %u-%u, digital %u,%u %ux772, frame %u lines: %s\n", s,
				    c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
				    bb_write16(0x0344, c[0], 2) ?: bb_write16(0x0348, c[1], 2) ?: bb_write16(0x0346, c[2], 2) ?:
				    bb_write16(0x034a, c[3], 2) ?: bb_write16(0x0408, c[4], 2) ?: bb_write16(0x040a, c[5], 2) ?:
				    bb_write16(0x040c, c[6], 2) ?: bb_write16(0x040e, 772, 2) ?: bb_write16(0x034c, c[6], 2) ?:
				    bb_write16(0x034e, 772, 2) ?: bb_write16(0x0340, c[7], 2) ?: "taken");
				(void)bb_write16(0x0104, 0, 1);
			}
			camrx_on = true;
			camrx_level = 7;
			rx_reached = 0;
			rx = cam_rx_up();
			if (session && rx && rx_reached >= 6 && cam_table(tab, tab_size, "on")) {
				/*
				 * (Test 340) iOS's front camera: the session's loop as for the S5K3L6 (the write master's two
				 * halves of the camera block in turn, each done frame to the caller, iOS's exposure between two
				 * frames over this bus), the battery's lock let go meanwhile; then everything down (no pipeline
				 * kept for the front camera: a switch brings it up afresh, ~0.4 s).
				 */
				uint32_t sframes, misses = 0;

				cam_bus_keep = *cam;
				cam_bus_ok = true;
				cam_live = c;
				exp_applied = 0;
				lck_mtx_unlock(&r5q_pmic_lock);
				sframes = cam_session_frames(&misses);
				lck_mtx_lock(&r5q_pmic_lock);
				cam_bus_ok = false;
				cam_live = NULL;
				SLOG("SESSION ends: %u frames handed over, %u misses at the end; the camera goes down\n", sframes, misses);
				(void)cam_table(tab, tab_size, "off");
			} else if (!session && rx && rx_reached >= 6 && cam_table(tab, tab_size, "on")) {
				uint32_t frames = 0, sum = 0, ms_all = tries > 1 ? 1000 : 5000;

				for (uint32_t ms = 0; ms < ms_all; ms++) {
					delay_for_interval(1, NSEC_PER_MSEC);
					if (IFE(0x2060) & 1) {
						IFE(0x2054) = 1;
						IFE(0x2068) = 1;
						IFE(0x2214) = (uint32_t)rx_buf_pa;
						IFE(0x04ac) = 0x2;
						frames++;
					}
				}
				for (uint32_t i = 0; i < 1280; i++) {
					if (i % 5 != 4) {
						sum += rx_buf[1296u * 386u + i];
					}
				}
				SLOG("SETTLE %u: FRAMES IN %u ms: %u; middle line mean %u of 255; CSID pkts %u ecc %#x "
				    "crc %u\n", cam_phy_settle, ms_all, frames, sum / 1024u, CSID(0x160), CSID(0x164), CSID(0x168));
				cam_rx_show("streaming");
				if (front_level == 2) {
					/*
					 * (Test 345) The picture's 10-bit levels per Bayer site (rows 100-671, every fourth): in a dark
					 * scene they are the sensor's black level, which the iOS side subtracts (64 assumed). Test 343's
					 * IMX586 read 0-4 of 255 in its high bytes, below 64.
					 */
					uint32_t mn[4] = { 1023, 1023, 1023, 1023 }, mx[4] = { 0 }, n[4] = { 0 };
					uint64_t sum4[4] = { 0 };

					for (uint32_t y = 100; y < 672; y += 4) {
						for (uint32_t r = 0; r < 2; r++) {
							const volatile uint8_t *row = rx_buf + 1296u * (y + r);

							for (uint32_t g = 0; g < 257; g++) {
								uint8_t lo = row[5 * g + 4];

								for (uint32_t k = 0; k < 4; k++) {
									uint32_t v = ((uint32_t)row[5 * g + k] << 2) | ((lo >> (2 * k)) & 3);
									uint32_t site = r * 2 + (k & 1);

									mn[site] = v < mn[site] ? v : mn[site];
									mx[site] = v > mx[site] ? v : mx[site];
									sum4[site] += v;
									n[site]++;
								}
							}
						}
					}
					SLOG("LEVELS (10-bit, min/mean/max) R %u/%u/%u Gr %u/%u/%u Gb %u/%u/%u B %u/%u/%u\n",
					    mn[0], (uint32_t)(sum4[0] / n[0]), mx[0], mn[1], (uint32_t)(sum4[1] / n[1]), mx[1],
					    mn[2], (uint32_t)(sum4[2] / n[2]), mx[2], mn[3], (uint32_t)(sum4[3] / n[3]), mx[3]);
				}
				if (front_level >= 4 && front_level <= 6) {
					/*
					 * (Test 337) Each row's 5-byte groups against the middle rows' first group (row parity:
					 * R/Gr rows and Gb/B rows); the errors per lane and per bit, the first few shown.
					 */
					uint8_t ref[2][5];
					uint32_t lane_err[4] = { 0 }, bit_err[8] = { 0 }, bad = 0, shown = 0, rows_bad = 0, first386 = ~0u;

					for (int k = 0; k < 5; k++) {
						ref[0][k] = rx_buf[1296u * 386u + k];
						ref[1][k] = rx_buf[1296u * 387u + k];
					}
					for (uint32_t y = 0; y < 772; y++) {
						uint32_t row_bad = 0;

						for (uint32_t x = 0; x < line_bytes; x++) {
							uint8_t want = ref[y & 1][x % 5], got = rx_buf[1296u * y + x];

							if (got != want) {
								bad++;
								row_bad++;
								lane_err[x % 4]++;
								for (int b = 0; b < 8; b++) {
									bit_err[b] += ((got ^ want) >> b) & 1;
								}
								if (y == 386 && first386 == ~0u) {
									first386 = x;
								}
								if (shown < 6) {
									shown++;
									SLOG("pattern: row %u byte %u: %#04x, not %#04x\n", y, x, got, want);
								}
							}
						}
						rows_bad += row_bad != 0;
					}
					SLOG("PATTERN: groups %02x%02x%02x%02x%02x / %02x%02x%02x%02x%02x; %u bytes of %u-byte "
					    "lines wrong in %u rows; per lane %u %u %u %u; per bit %u %u %u %u %u %u %u %u\n",
					    ref[0][0], ref[0][1], ref[0][2], ref[0][3], ref[0][4], ref[1][0], ref[1][1], ref[1][2],
					    ref[1][3], ref[1][4], bad, line_bytes, rows_bad, lane_err[0], lane_err[1], lane_err[2], lane_err[3],
					    bit_err[0], bit_err[1], bit_err[2], bit_err[3], bit_err[4], bit_err[5], bit_err[6], bit_err[7]);
					if (first386 != ~0u) {
						/* the middle row's bytes from 8 before its first wrong one, and its last 12 and 4 after */
						char hex[3 * 40 + 1];
						uint32_t at = first386 >= 8 ? first386 - 8 : 0, n = 0;

						for (uint32_t k = 0; k < 32 && n + 3 < sizeof(hex); k++) {
							n += snprintf(hex + n, sizeof(hex) - n, "%02x ", rx_buf[1296u * 386u + at + k]);
						}
						SLOG("row 386 from byte %u: %s\n", at, hex);
						n = 0;
						for (uint32_t k = line_bytes - 12; k < line_bytes + 4 && n + 3 < sizeof(hex); k++) {
							n += snprintf(hex + n, sizeof(hex) - n, "%02x ", rx_buf[1296u * 386u + k]);
						}
						SLOG("row 386 from byte %u (the end, and 4 after): %s\n", line_bytes - 12, hex);
					}
				}
				(void)cam_table(tab, tab_size, "off");
			}
			spin_us(20000);
			cam_rx_show("stopped");
			cam_rx_down();
		}
		rxlog_fast = false;
		cam_phy_index = 2;
		cam_phy_settle = 14;
	}
	}
}

/* (Test 342) the front camera's: Tests 334-341 */
static const struct sony_cam imx616_cam = {
	.name = "front camera", .tab = "camera-imx616.tab", .mode = "res3", .crop_x = 306, .crop_y = 226, .width = 1028,
	.height = 772, .frame_lines = 5160,
	.line_length = 0, .phy = 3, .settle = 28, .iop_mpy = 160, .line_num = 919, .line_den = 729, .frame_num = 5160,
	.frame_den = 3268, .margin = 48,
};
/*
 * (Test 350) iOS's sessions take the sensors' whole binned frames, not the 1028x772 crop: the field of view of the
 * sensor's mode and ~2.5-3.7x the pixels for photographs (mediaserverd renders its outputs from any frame size).
 * The IMX616: res3 as it is (1632x1224, digital crop at 4,0), frame length 5160. cam_full=0: the crops again.
 */
static const struct sony_cam imx616_full = {
	.name = "front camera", .tab = "camera-imx616.tab", .mode = "res3", .crop_x = 4, .crop_y = 0, .width = 1632,
	.height = 1224, .frame_lines = 5160, .line_length = 0, .phy = 3, .settle = 28, .iop_mpy = 160, .line_num = 919,
	.line_den = 729, .frame_num = 5160, .frame_den = 3268, .margin = 48,
};

static bool
cam_full_frames(void)
{
	uint32_t full = 1;

	(void)PE_parse_boot_argn("cam_full", &full, sizeof(full));
	return full != 0;
}

/*
 * (Test 333, camfront=1) The FRONT camera, the IMX616 (live DT qcom,cam-sensor@1 on CCI0 master 1; Samsung's
 * sm8150-camera-sensor-r5q-r01.dtsi; module file com.samsung.sensormodule.1_sony_imx616.bin): powered as its module
 * file's power-up says (vana 2.9 V S2MPB03 #2 LDO5, vdig 1.05 V LDO1, vio 1.8 V L14, reset high 10 ms, MCLK 2 ms),
 * its bus GPIO19/20 (NORTH) driven by hand, its ID (0x0616 at 0x0016, 7-bit 0x1a) read, everything put back.
 * MCLK2: camcc RCG 0x5044 / branch 0x505c from TCXO (19.2 MHz; the sensor's tables want 24: enough for the ID),
 * GPIO15 function cam_mclk; reset GPIO82 (SOUTH).
 */
static void
frontsensor_probe(uint32_t front_level)
{
	struct bitbang pm = { .sda = 47, .scl = 48, .address = 0x56 }, cam = { .sda = 19, .scl = 20, .address = 0x1a }, *was = bb;
	vm_offset_t north = ml_io_map(0x03900000UL, 0x1000UL * 33), east = ml_io_map(0x03500000UL, 0x1000UL * 49);
	vm_offset_t south = ml_io_map(0x03d00000UL, 0x1000UL * 83), camcc = ml_io_map(0x0ad00000UL + 0x5000, 0x1000);
	vm_offset_t camcc_c = ml_io_map(0x0ad00000UL + 0xc000, 0x1000);
	uint32_t was47, was48, was15, was19, was20, was82, was82io, was_gdscr = 0, was_gdsc_clk = 0;
	uint8_t ldo1 = 0, ldo5 = 0, id[2] = { 0 };
	const char *problem = NULL;
	int us;

	if (north == 0 || east == 0 || south == 0 || camcc == 0 || camcc_c == 0) {
		rxlog("front camera: no mapping\n");
		return;
	}
	rxlog("front camera: the IMX616 (7-bit 0x1a on GPIO19/20, MCLK2 GPIO15, reset GPIO82): its ID\n");
	rxlog("front camera: L14 vote: %s\n", redwood_r5q_rpmh_enable("ldoa14") ? "done" : "NOT DONE");
	/* vana, vdig: S2MPB03 #2 (0x56 on GPIO47/48, EAST): LDO5 2.9 V (1.8 V + 44 x 25 mV), then LDO1 1.05 V (0.7 V + 28 x 12.5 mV) */
	pm.tlmm = east;
	bb = &pm;
	was47 = rd(BB_CFG(47));
	was48 = rd(BB_CFG(48));
	wr(BB_CFG(47), 0);
	wr(BB_CFG(48), 0);
	spin_us(50);
	if (!bb_recover("S2MPB03 #2") || (problem = bb_read(0x03, &ldo1, 1)) != NULL || (problem = bb_read(0x07, &ldo5, 1)) != NULL) {
		rxlog("front camera: S2MPB03 #2: %s: nothing switched on\n", problem ? problem : "the bus stays low");
		goto pins_back;
	}
	rxlog("front camera: S2MPB03 #2 LDO1 %#04x LDO5 %#04x as found; LDO5 on at 2.9 V, then LDO1 at 1.05 V\n", ldo1, ldo5);
	if ((problem = bb_write(0x07, 0x80 | 44)) != NULL) {
		goto power_back;
	}
	spin_us(1000);
	if ((problem = bb_write(0x03, 0x80 | 28)) != NULL) {
		goto power_back;
	}
	spin_us(1000);
	{
		uint8_t n1 = 0, n5 = 0;

		(void)bb_read(0x03, &n1, 1);
		(void)bb_read(0x07, &n5, 1);
		rxlog("front camera: S2MPB03 #2 LDO1 %#04x LDO5 %#04x now\n", n1, n5);
	}
	/* the camera's power domain and clock controller, as the rear sensor's probe does */
	rxlog("front camera: rails mmcx %d mx %d\n", redwood_r5q_rpmh_level("mmcx.lvl", 128), redwood_r5q_rpmh_level("mx.lvl", 192));
	was_gdsc_clk = *(volatile uint32_t *)(camcc_c + 0x1e4);
	was_gdscr = *(volatile uint32_t *)(camcc_c + 0x1bc);
	lck_mtx_lock(&redwood_smmu_lock);
	*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk | 1;
	*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr & ~1u;
	for (us = 0; us < 10000 && !(*(volatile uint32_t *)(camcc_c + 0x1bc) & (1u << 31)); us += 10) {
		spin_us(10);
	}
	lck_mtx_unlock(&redwood_smmu_lock);
	rxlog("front camera: titan_top_gdsc %#x -> %#x\n", was_gdscr, *(volatile uint32_t *)(camcc_c + 0x1bc));
	/* reset high (10 ms), then MCLK2 from TCXO on GPIO15 (2 ms) */
	was82 = rd(south + 0x1000UL * 82);
	was82io = rd(south + 0x1000UL * 82 + 4);
	wr(south + 0x1000UL * 82 + 4, 0);
	wr(south + 0x1000UL * 82, (1u << 9));                           /* GPIO, output, low first */
	spin_us(1000);
	wr(south + 0x1000UL * 82 + 4, 2);                               /* high: out of reset */
	spin_us(10000);
	*(volatile uint32_t *)(camcc + 0x044 + 4) = 1;                  /* MCLK2 RCG CFG: TCXO, divider 1 */
	*(volatile uint32_t *)(camcc + 0x044) |= 1;                     /* CMD: update */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x044) & 1); us += 10) {
		spin_us(10);
	}
	*(volatile uint32_t *)(camcc + 0x05c) |= 1;                     /* the branch */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x05c) & (1u << 31)); us += 10) {
		spin_us(10);
	}
	was15 = rd(north + 0x1000UL * 15);
	wr(north + 0x1000UL * 15, (1u << 2) | (1u << 6));               /* cam_mclk, 4 mA, no pull */
	spin_us(2000);
	rxlog("front camera: MCLK2 RCG cmd %#x cfg %#x branch %#x; GPIO15 %#x; reset GPIO82 %#x io %#x\n",
	    *(volatile uint32_t *)(camcc + 0x044), *(volatile uint32_t *)(camcc + 0x048), *(volatile uint32_t *)(camcc + 0x05c),
	    rd(north + 0x1000UL * 15), rd(south + 0x1000UL * 82), rd(south + 0x1000UL * 82 + 4));
	/* the bus GPIO19/20 by hand: what answers, then the ID */
	cam.tlmm = north;
	bb = &cam;
	was19 = rd(BB_CFG(19));
	was20 = rd(BB_CFG(20));
	cam.cfg_sda = 3;                                                /* GPIO, pull-up, 2 mA, input */
	cam.cfg_scl = 3;
	wr(BB_CFG(19), 3);
	wr(BB_CFG(20), 3);
	spin_us(50);
	if (!bb_recover("the front sensor's bus")) {
		rxlog("front camera: GPIO19/20: the bus stays low\n");
	} else {
		char seen[160];
		int at = 0;

		for (unsigned a = 0x08; a < 0x78 && at < (int)sizeof(seen) - 6; a++) {
			uint8_t b;

			cam.address = (uint8_t)a;
			if (bb_read16(0x0000, &b, 1) == NULL) {
				at += snprintf(seen + at, sizeof(seen) - at, " %#04x", a);
			}
		}
		rxlog("front camera: the bus GPIO19/20 answers at:%s\n", at ? seen : " nothing");
		cam.address = 0x1a;
		problem = bb_read16(0x0016, id, 2);
		rxlog("front camera: 7-bit 0x1a: ID %s %02x%02x%s\n", problem ? problem : "", id[0], id[1],
		    problem == NULL && id[0] == 0x06 && id[1] == 0x16 ? ": THE IMX616 ANSWERS" : "");
		if (problem == NULL && id[0] == 0x06 && id[1] == 0x16 && front_level >= 2) {
			sony_stream(&cam, front_level, front_level == 7 && cam_full_frames() ? &imx616_full : &imx616_cam);
		}
	}
	wr(BB_CFG(19), was19);
	wr(BB_CFG(20), was20);
	/* everything back: MCLK off, reset low, the domain, the LDOs */
	wr(north + 0x1000UL * 15, was15);
	*(volatile uint32_t *)(camcc + 0x05c) &= ~1u;
	wr(south + 0x1000UL * 82 + 4, 0);
	spin_us(1000);
	wr(south + 0x1000UL * 82, was82);
	wr(south + 0x1000UL * 82 + 4, was82io);
	lck_mtx_lock(&redwood_smmu_lock);
	*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr;
	*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk;
	spin_us(100);
	lck_mtx_unlock(&redwood_smmu_lock);
	bb = &pm;
	problem = NULL;
power_back:
	(void)bb_write(0x03, ldo1);
	spin_us(1000);
	(void)bb_write(0x07, ldo5);
	rxlog("front camera: back: S2MPB03 #2 LDO1 %#04x LDO5 %#04x%s%s\n", ldo1, ldo5, problem ? "; write: " : "",
	    problem ? problem : "");
pins_back:
	wr(BB_CFG(47), was47);
	wr(BB_CFG(48), was48);
	bb = was;
}

/*
 * (Test 342) The main camera, the IMX586 (48 MP, `qcom,cam-sensor@0' in the live DT): CCI 0 master 0 = GPIO17/18
 * (NORTH), 7-bit 0x1a (slave-addr 0x34), ID 0x0586 at 0x0016 (two bytes), CSI-PHY 0; MCLK0 = camcc RCG 0x5004 / branch
 * 0x501c on GPIO13 (NORTH, function 1); reset GPIO92 (NORTH); "RCAM1_OIS" GPIO133 (SOUTH). Its supplies, mostly on a
 * second S2MPB03 (#1: 0x56 on GPIO94/95, NORTH; LDOn's register 0x02 + n, LDO1-2 0.7 V + 12.5 mV, LDO3-7 1.8 V + 25 mV):
 * vdig #1 LDO1 1.1 V, vana #1 LDO6 2.9 V, vaf #1 LDO7 2.8 V, vio L14, custom1 #2 LDO3 1.8 V, custom2 #1 LDO3 1.8 V,
 * custom3 #1 LDO5 2.8 V. Its module file's power-up order: vana, custom1, vdig, vaf, vio, custom2, the OIS line,
 * custom3, MCLK, reset (10 ms). main_level: 1 its ID; 2-7 as sony_stream's.
 */
static const struct sony_cam imx586_cam = {
	/* res5: 2000x1500 (binned) at 120 a second; its middle 1028x772 at 486,364; line length doubled (10752) and frame
	 * length 5356: ~30 a second at 24 MHz (~24 at our 19.2); the output PLL at 160 (~1 Gb/s a lane: res5's 312 is
	 * ~2.5) */
	.name = "main camera", .tab = "camera-imx586.tab", .mode = "res5", .crop_x = 486, .crop_y = 364, .width = 1028,
	.height = 772, .frame_lines = 5356,
	.line_length = 10752, .phy = 0, .settle = 28, .iop_mpy = 160, .line_num = 919, .line_den = 778, .frame_num = 5356,
	.frame_den = 3268, .margin = 48,
};
/*
 * (Test 350) The IMX586's whole res5 frame (2000x1500) for iOS: its line takes ~4.9 us on four lanes at ~1 Gb/s, so the
 * line length tripled (16128, ~11.7 us a line at 19.2 MHz) and frame length 3571 (~30 a second at 24 MHz).
 */
static const struct sony_cam imx586_full = {
	.name = "main camera", .tab = "camera-imx586.tab", .mode = "res5", .crop_x = 0, .crop_y = 0, .width = 2000,
	.height = 1500, .frame_lines = 3571, .line_length = 16128, .phy = 0, .settle = 28, .iop_mpy = 160, .line_num = 919,
	.line_den = 1167, .frame_num = 3571, .frame_den = 3268, .margin = 48,
};

static void
mainsensor_probe(uint32_t main_level)
{
	struct bitbang pm1 = { .sda = 94, .scl = 95, .address = 0x56 }, pm2 = { .sda = 47, .scl = 48, .address = 0x56 };
	struct bitbang cam = { .sda = 17, .scl = 18, .address = 0x1a }, *was = bb;
	vm_offset_t north = ml_io_map(0x03900000UL, 0x1000UL * 100), east = ml_io_map(0x03500000UL, 0x1000UL * 49);
	vm_offset_t south = ml_io_map(0x03d00000UL, 0x1000UL * 134), camcc = ml_io_map(0x0ad00000UL + 0x5000, 0x1000);
	vm_offset_t camcc_c = ml_io_map(0x0ad00000UL + 0xc000, 0x1000);
	/* #1's LDOs 1, 3, 5, 6, 7 and #2's LDO3 as found (their registers 0x03, 0x05, 0x07, 0x08, 0x09) */
	static const uint8_t ldo_reg[5] = { 0x03, 0x05, 0x07, 0x08, 0x09 };
	uint8_t ldo1[5] = { 0 }, ldo2_3 = 0, id[2] = { 0 };
	uint32_t was94, was95, was47, was48, was13 = 0, was17 = 0, was18 = 0, was92 = 0, was92io = 0, was133 = 0, was133io = 0;
	uint32_t was_gdscr = 0, was_gdsc_clk = 0;
	const char *problem = NULL;
	bool pm1_ok = false, pm2_ok = false;
	int us;

	if (north == 0 || east == 0 || south == 0 || camcc == 0 || camcc_c == 0) {
		rxlog("main camera: no mapping\n");
		return;
	}
	rxlog("main camera: the IMX586 (7-bit 0x1a on GPIO17/18, MCLK0 GPIO13, reset GPIO92, OIS GPIO133): its ID\n");
	pm1.tlmm = north;
	pm2.tlmm = east;
	bb = &pm1;
	was94 = rd(BB_CFG(94));
	was95 = rd(BB_CFG(95));
	wr(BB_CFG(94), 0);
	wr(BB_CFG(95), 0);
	bb = &pm2;
	was47 = rd(BB_CFG(47));
	was48 = rd(BB_CFG(48));
	wr(BB_CFG(47), 0);
	wr(BB_CFG(48), 0);
	spin_us(50);
	bb = &pm1;
	pm1_ok = bb_recover("S2MPB03 #1");
	for (int i = 0; pm1_ok && i < 5 && problem == NULL; i++) {
		problem = bb_read(ldo_reg[i], &ldo1[i], 1);
	}
	if (!pm1_ok || problem != NULL) {
		rxlog("main camera: S2MPB03 #1 (GPIO94/95): %s: nothing switched on\n", problem ? problem : "the bus stays low");
		goto pins_back;
	}
	bb = &pm2;
	pm2_ok = bb_recover("S2MPB03 #2");
	if (!pm2_ok || (problem = bb_read(0x05, &ldo2_3, 1)) != NULL) {
		rxlog("main camera: S2MPB03 #2 (GPIO47/48): %s: nothing switched on\n", problem ? problem : "the bus stays low");
		goto pins_back;
	}
	rxlog("main camera: S2MPB03 #1 LDO1 %#04x LDO3 %#04x LDO5 %#04x LDO6 %#04x LDO7 %#04x, #2 LDO3 %#04x as found\n",
	    ldo1[0], ldo1[1], ldo1[2], ldo1[3], ldo1[4], ldo2_3);
	/* vana, custom1, vdig, vaf, vio, custom2 -- 1 ms apart */
	bb = &pm1;
	if ((problem = bb_write(0x08, 0x80 | 44)) != NULL) {             /* vana: #1 LDO6 2.9 V */
		goto power_back;
	}
	spin_us(1000);
	bb = &pm2;
	if ((problem = bb_write(0x05, 0x80 | 0)) != NULL) {              /* custom1: #2 LDO3 1.8 V */
		goto power_back;
	}
	spin_us(1000);
	bb = &pm1;
	if ((problem = bb_write(0x03, 0x80 | 32)) != NULL) {             /* vdig: #1 LDO1 1.1 V */
		goto power_back;
	}
	spin_us(1000);
	if ((problem = bb_write(0x09, 0x80 | 40)) != NULL) {             /* vaf: #1 LDO7 2.8 V */
		goto power_back;
	}
	spin_us(1000);
	rxlog("main camera: L14 vote: %s\n", redwood_r5q_rpmh_enable("ldoa14") ? "done" : "NOT DONE");
	spin_us(1000);
	if ((problem = bb_write(0x05, 0x80 | 0)) != NULL) {              /* custom2: #1 LDO3 1.8 V */
		goto power_back;
	}
	spin_us(1000);
	was133 = rd(south + 0x1000UL * 133);
	was133io = rd(south + 0x1000UL * 133 + 4);
	wr(south + 0x1000UL * 133 + 4, 2);
	wr(south + 0x1000UL * 133, (1u << 9));                          /* the OIS line: GPIO, output, high */
	spin_us(1000);
	if ((problem = bb_write(0x07, 0x80 | 40)) != NULL) {             /* custom3: #1 LDO5 2.8 V */
		goto ois_back;
	}
	spin_us(1000);
	{
		uint8_t now[5] = { 0 }, now2 = 0;

		for (int i = 0; i < 5; i++) {
			(void)bb_read(ldo_reg[i], &now[i], 1);
		}
		bb = &pm2;
		(void)bb_read(0x05, &now2, 1);
		rxlog("main camera: S2MPB03 #1 LDO1 %#04x LDO3 %#04x LDO5 %#04x LDO6 %#04x LDO7 %#04x, #2 LDO3 %#04x now\n",
		    now[0], now[1], now[2], now[3], now[4], now2);
	}
	/* the camera's power domain, as the other probes */
	rxlog("main camera: rails mmcx %d mx %d\n", redwood_r5q_rpmh_level("mmcx.lvl", 128), redwood_r5q_rpmh_level("mx.lvl", 192));
	was_gdsc_clk = *(volatile uint32_t *)(camcc_c + 0x1e4);
	was_gdscr = *(volatile uint32_t *)(camcc_c + 0x1bc);
	lck_mtx_lock(&redwood_smmu_lock);
	*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk | 1;
	*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr & ~1u;
	for (us = 0; us < 10000 && !(*(volatile uint32_t *)(camcc_c + 0x1bc) & (1u << 31)); us += 10) {
		spin_us(10);
	}
	lck_mtx_unlock(&redwood_smmu_lock);
	rxlog("main camera: titan_top_gdsc %#x -> %#x\n", was_gdscr, *(volatile uint32_t *)(camcc_c + 0x1bc));
	/* MCLK0 from TCXO on GPIO13, then reset high (10 ms) */
	*(volatile uint32_t *)(camcc + 0x004 + 4) = 1;                  /* MCLK0 RCG CFG: TCXO, divider 1 */
	*(volatile uint32_t *)(camcc + 0x004) |= 1;                     /* CMD: update */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x004) & 1); us += 10) {
		spin_us(10);
	}
	*(volatile uint32_t *)(camcc + 0x01c) |= 1;                     /* the branch */
	for (us = 0; us < 10000 && (*(volatile uint32_t *)(camcc + 0x01c) & (1u << 31)); us += 10) {
		spin_us(10);
	}
	was13 = rd(north + 0x1000UL * 13);
	wr(north + 0x1000UL * 13, (1u << 2) | (1u << 6));               /* cam_mclk, 4 mA, no pull */
	spin_us(1000);
	was92 = rd(north + 0x1000UL * 92);
	was92io = rd(north + 0x1000UL * 92 + 4);
	wr(north + 0x1000UL * 92 + 4, 0);
	wr(north + 0x1000UL * 92, (1u << 9));                           /* GPIO, output, low */
	spin_us(1000);
	wr(north + 0x1000UL * 92 + 4, 2);                               /* high: out of reset */
	spin_us(10000);
	rxlog("main camera: MCLK0 RCG cmd %#x cfg %#x branch %#x; GPIO13 %#x; reset GPIO92 %#x io %#x; OIS GPIO133 %#x io %#x\n",
	    *(volatile uint32_t *)(camcc + 0x004), *(volatile uint32_t *)(camcc + 0x008), *(volatile uint32_t *)(camcc + 0x01c),
	    rd(north + 0x1000UL * 13), rd(north + 0x1000UL * 92), rd(north + 0x1000UL * 92 + 4), rd(south + 0x1000UL * 133),
	    rd(south + 0x1000UL * 133 + 4));
	/* the bus GPIO17/18 by hand: what answers, then the ID */
	cam.tlmm = north;
	bb = &cam;
	was17 = rd(BB_CFG(17));
	was18 = rd(BB_CFG(18));
	cam.cfg_sda = 3;
	cam.cfg_scl = 3;
	wr(BB_CFG(17), 3);
	wr(BB_CFG(18), 3);
	spin_us(50);
	if (!bb_recover("the main sensor's bus")) {
		rxlog("main camera: GPIO17/18: the bus stays low\n");
	} else {
		char seen[160];
		int at = 0;

		for (unsigned a = 0x08; a < 0x78 && at < (int)sizeof(seen) - 6; a++) {
			uint8_t b;

			cam.address = (uint8_t)a;
			if (bb_read16(0x0000, &b, 1) == NULL) {
				at += snprintf(seen + at, sizeof(seen) - at, " %#04x", a);
			}
		}
		rxlog("main camera: the bus GPIO17/18 answers at:%s\n", at ? seen : " nothing");
		cam.address = 0x1a;
		problem = bb_read16(0x0016, id, 2);
		rxlog("main camera: 7-bit 0x1a: ID %s %02x%02x%s\n", problem ? problem : "", id[0], id[1],
		    problem == NULL && id[0] == 0x05 && id[1] == 0x86 ? ": THE IMX586 ANSWERS" : "");
		if (problem == NULL && id[0] == 0x05 && id[1] == 0x86 && main_level >= 2) {
			sony_stream(&cam, main_level, main_level == 7 && cam_full_frames() ? &imx586_full : &imx586_cam);
		}
	}
	wr(BB_CFG(17), was17);
	wr(BB_CFG(18), was18);
	/* everything back, the other way round: reset low, MCLK off, the domain */
	wr(north + 0x1000UL * 92 + 4, 0);
	spin_us(1000);
	wr(north + 0x1000UL * 92, was92);
	wr(north + 0x1000UL * 92 + 4, was92io);
	wr(north + 0x1000UL * 13, was13);
	*(volatile uint32_t *)(camcc + 0x01c) &= ~1u;
	lck_mtx_lock(&redwood_smmu_lock);
	*(volatile uint32_t *)(camcc_c + 0x1bc) = was_gdscr;
	*(volatile uint32_t *)(camcc_c + 0x1e4) = was_gdsc_clk;
	spin_us(100);
	lck_mtx_unlock(&redwood_smmu_lock);
	problem = NULL;
	bb = &pm1;
	(void)bb_write(0x07, ldo1[2]);                                  /* custom3 */
	spin_us(1000);
ois_back:
	wr(south + 0x1000UL * 133, was133);
	wr(south + 0x1000UL * 133 + 4, was133io);
	spin_us(1000);
power_back:
	bb = &pm1;
	(void)bb_write(0x05, ldo1[1]);                                  /* custom2 */
	spin_us(1000);
	(void)bb_write(0x09, ldo1[4]);                                  /* vaf */
	spin_us(1000);
	(void)bb_write(0x03, ldo1[0]);                                  /* vdig */
	spin_us(1000);
	bb = &pm2;
	(void)bb_write(0x05, ldo2_3);                                   /* custom1 */
	spin_us(1000);
	bb = &pm1;
	(void)bb_write(0x08, ldo1[3]);                                  /* vana */
	rxlog("main camera: back: S2MPB03 #1 LDO1 %#04x LDO3 %#04x LDO5 %#04x LDO6 %#04x LDO7 %#04x, #2 LDO3 %#04x%s%s\n",
	    ldo1[0], ldo1[1], ldo1[2], ldo1[3], ldo1[4], ldo2_3, problem ? "; write: " : "", problem ? problem : "");
pins_back:
	bb = &pm1;
	wr(BB_CFG(94), was94);
	wr(BB_CFG(95), was95);
	bb = &pm2;
	wr(BB_CFG(47), was47);
	wr(BB_CFG(48), was48);
	bb = was;
}

/* (Test 294) cam_soak: the kernel's own reader, one line a second (every 30 frames) with the middle line's mean */
bool redwood_r5q_cam_run(bool (*cb)(const uint8_t *frame, uint32_t bytes, void *ctx), void *ctx);
static uint32_t soak_frames, soak_frames_max;
void redwood_r5q_cam_front(bool front);
void redwood_r5q_cam_sensor(uint32_t which);
void redwood_r5q_cam_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines);

static bool
soak_frame(const uint8_t *frame, uint32_t bytes, __unused void *ctx)
{
	soak_frames++;
	if (soak_frames == 15) {
		redwood_r5q_cam_exposure(1500, 64, 3268);      /* (Test 340) the exposure path too: ~13.8 ms, 2x */
	}
	if (soak_frames % 30 == 0 && bytes >= 1296u * 400u) {
		/* (Test 350) the live frame's shape: 1028x772, or a Sony sensor's whole frame */
		uint32_t w = cam_live != NULL ? cam_live->width : 1028, h = cam_live != NULL ? cam_live->height : 772;
		uint32_t stride = (w * 10 / 8 + 15) & ~15u, sum = 0;

		for (uint32_t i = 0; i < 1280; i++) {
			if (i % 5 != 4) {
				sum += frame[stride * (h / 2) + i];     /* RAW10: four high bytes, then a byte of low bits */
			}
		}
		rxlog("camera soak: %u frames (%u s); middle line mean %u of 255\n", soak_frames, soak_frames / 30, sum / 1024u);
	}
	return soak_frames < soak_frames_max;
}

/* (Test 284) the camera probe's own thread: the battery's lock held while it runs, the touch thread left alone */
static void
camsensor_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t soak = 0, front = 0;

	uint32_t main_level = 0;

	/* (Test 342) cammain=N: the main sensor's (the IMX586's) probe instead, at sony_stream's levels */
	if (PE_parse_boot_argn("cammain", &main_level, sizeof(main_level)) && main_level != 0) {
		lck_mtx_lock(&r5q_pmic_lock);
		mainsensor_probe(main_level);
		lck_mtx_unlock(&r5q_pmic_lock);
		thread_terminate(current_thread());
	}
	/* (Test 333) camfront=1: the front sensor's ID probe instead (the rear's pipeline untouched) */
	if (PE_parse_boot_argn("camfront", &front, sizeof(front)) && front != 0) {
		lck_mtx_lock(&r5q_pmic_lock);
		frontsensor_probe(front);
		lck_mtx_unlock(&r5q_pmic_lock);
		thread_terminate(current_thread());
	}
	/*
	 * (Test 294, cam_soak=N) A whole session with nobody but the kernel reading: Tests 292-293 went down ~3-4 s into
	 * the first session that lasted (iOS reading). This says whether the hardware streaming for N seconds alone does it.
	 */
	if (PE_parse_boot_argn("cam_soak", &soak, sizeof(soak)) && soak != 0) {
		uint32_t first = 0;

		/*
		 * (Test 295, cam_soak_first=N) As iOS does it: a short session of N frames, taken down, then the long one.
		 * Tests 292-293 went down in the second session; Test 294's single 20 s session did not.
		 */
		if (PE_parse_boot_argn("cam_soak_first", &first, sizeof(first)) && first != 0) {
			uint32_t count = 1;

			/* (Test 300, cam_soak_n=K) K short sessions first: iOS's hangs came at the third session after boot */
			(void)PE_parse_boot_argn("cam_soak_n", &count, sizeof(count));
			uint32_t alt = 0;

			/*
			 * (Test 340, cam_soak_alt=1) The short sessions alternate front (odd) and rear (even), as iOS's camera
			 * switches would; the long one is the front camera's with cam_soak_front=1.
			 */
			(void)PE_parse_boot_argn("cam_soak_alt", &alt, sizeof(alt));
			for (uint32_t s = 1; s <= count; s++) {
				soak_frames = 0;
				soak_frames_max = first;
				/* (Test 343) cam_soak_alt=2: the three cameras in turn (front, main, ultra-wide) */
				redwood_r5q_cam_sensor(alt == 2 ? s % 3 : alt != 0 && (s & 1) != 0 ? 1 : 0);
				rxlog("camera soak: short session %u of %u (%u frames)\n", s, count, first);
				(void)redwood_r5q_cam_run(soak_frame, NULL);
				rxlog("camera soak: short session %u over: %u frames\n", s, soak_frames);
			}
			soak_frames = 0;
		}
		soak_frames_max = soak * 30;
		{
			uint32_t soak_front = 0;

			(void)PE_parse_boot_argn("cam_soak_front", &soak_front, sizeof(soak_front));
			redwood_r5q_cam_sensor(soak_front);      /* (Test 343) 1 the front camera, 2 the main one */
		}
		rxlog("camera soak: a session for %u s (%u frames at ~30 fps), only the kernel reading\n", soak, soak_frames_max);
		(void)redwood_r5q_cam_run(soak_frame, NULL);
		rxlog("camera soak: over: %u frames; the phone is still up\n", soak_frames);
		thread_terminate(current_thread());
	}
	lck_mtx_lock(&r5q_pmic_lock);
	camsensor_probe();
	lck_mtx_unlock(&r5q_pmic_lock);
	thread_terminate(current_thread());
}

/*
 * (r5q Test 290) For redwood_camera.c's /dev/redwood-camera on the S10 Lite (boot-arg cam_r5q=1): the S5K3L6 powered,
 * configured (1028x772 RAW10, about 30 frames a second), received and written into the loader's camera block, each
 * frame handed to `cb` (the frame, 1296-byte lines, 772 of them) until it returns false; then everything put back as
 * the probe does. Blocks for the whole session; under the battery's lock, as the probe.
 */
bool redwood_r5q_cam_run(bool (*cb)(const uint8_t *frame, uint32_t bytes, void *ctx), void *ctx);

/*
 * (Test 340) redwood_camera.c says which camera iOS asked for before each session: true = the front one (the IMX616).
 * (Test 343) redwood_r5q_cam_sensor: 0 the S5K3L6 (ultra-wide), 1 the IMX616 (front), 2 the IMX586 (main).
 */
static volatile uint32_t cam_want_sensor;
void redwood_r5q_cam_front(bool front);
void redwood_r5q_cam_sensor(uint32_t which);
void
redwood_r5q_cam_sensor(uint32_t which)
{
	cam_want_sensor = which;
}
void
redwood_r5q_cam_front(bool front)
{
	cam_want_sensor = front ? 1 : 0;
}

bool
redwood_r5q_cam_run(bool (*cb)(const uint8_t *frame, uint32_t bytes, void *ctx), void *ctx)
{
	if (!redwood_platform_is("r5q") || cb == NULL) {
		return false;
	}
	if (cam_want_sensor != 0) {
		/*
		 * (Test 340) The front camera: the rear pipeline down first if a switch left it up (one receive path, one camera
		 * block), then the IMX616's probe in its session mode (camfront level 7), all of it torn down at the end.
		 * (Test 343) The main camera (the IMX586) the same way (cammain level 7).
		 */
		lck_mtx_lock(&r5q_pmic_lock);
		if (cam_pipe_up) {
			rxlog("camera sensor: the %s asked for: the S5K3L6's pipeline goes down first\n",
			    cam_want_sensor == 1 ? "front camera" : "main camera");
			cam_pipe_down();
		}
		rx_session_cb = cb;
		rx_session_ctx = ctx;
		rx_frame_done = false;
		if (cam_want_sensor == 1) {
			frontsensor_probe(7);
		} else {
			mainsensor_probe(7);
		}
		rx_session_cb = NULL;
		rx_session_ctx = NULL;
		rxlog_fast = false;
		lck_mtx_unlock(&r5q_pmic_lock);
		return true;
	}
	if (cam_pipe_up) {
		/* (Test 322) the pipeline is up from an earlier session: straight to the frames, nothing powered or set up */
		uint32_t frames, misses = 0, persist = 1;

		cam_keep_next = true;
		rx_session_cb = cb;
		rx_session_ctx = ctx;
		frames = cam_session_frames(&misses);
		rx_session_cb = NULL;
		rx_session_ctx = NULL;
		(void)PE_parse_boot_argn("cam_persist", &persist, sizeof(persist));
		rxlog("camera sensor: SESSION ends: %u frames handed over, %u misses at the end; the pipeline %s\n", frames,
		    misses, persist != 0 && cam_keep_next ? "stays up (a camera switch)" : "goes down");
		if (persist == 0 || !cam_keep_next) {
			lck_mtx_lock(&r5q_pmic_lock);
			cam_pipe_down();
			lck_mtx_unlock(&r5q_pmic_lock);
		}
		return true;
	}
	cam_keep_next = true;
	lck_mtx_lock(&r5q_pmic_lock);
	rx_session_cb = cb;
	rx_session_ctx = ctx;
	/*
	 * (Test 325) The bring-up paced (300 ms after each "camera rx: step"), in sessions too. Every bring-up that hung
	 * (Tests 312, 318, 324) ran fast (rxlog_fast, from Test 290); none of the paced ones ever did (287-289, 300-308).
	 * With the persistent pipeline it happens once per boot: ~6 s, inside iOS's 15 s wait for the first frame.
	 * cam_fast=1 brings back the fast bring-up.
	 */
	{
		/*
		 * (Test 331) Fast again by default: with the RCGs parked before PLL0 goes off (Test 327), Test 330 ran eight
		 * fast power cycles (~0.37 s each) and 60 s of frames without a hang. cam_slow=1 brings the pacing back.
		 */
		uint32_t slow = 0;

		(void)PE_parse_boot_argn("cam_slow", &slow, sizeof(slow));
		rxlog_fast = slow == 0;
	}
	camsensor_on = true;
	camsensor_done = false;
	camrx_on = true;
	camrx_level = 7;
	rx_reached = 0;
	rx_frame_done = false;
	camsensor_probe();
	rx_session_cb = NULL;
	rx_session_ctx = NULL;
	rxlog_fast = false;
	lck_mtx_unlock(&r5q_pmic_lock);
	return true;
}

static void
charger_dump(void)
{
	struct bitbang *was = bb;
	uint32_t was_sda, was_scl;
	uint8_t regs[0x40];
	const char *problem = NULL;
	char line[200];

	if (!charger_dump_on || charger_dump_off) {
		return;
	}
	charger_bus.tlmm = tlmm;                /* (the SOUTH tile, as the gauge's GPIO57/58: GPIO51/52 are qup4, SOUTH) */
	bb = &charger_bus;
	was_sda = rd(BB_CFG(PIN_CHG_SDA));
	was_scl = rd(BB_CFG(PIN_CHG_SCL));
	if (charger_dumps == 0) {
		printf("charger: S2MU106 pins GPIO%u config %#x (function %u), GPIO%u config %#x (function %u)\n", PIN_CHG_SDA,
		    was_sda, (was_sda >> 2) & 0xf, PIN_CHG_SCL, was_scl, (was_scl >> 2) & 0xf);
	}
	charger_bus.cfg_sda = 0;
	charger_bus.cfg_scl = 0;
	wr(BB_CFG(PIN_CHG_SDA), 0);
	wr(BB_CFG(PIN_CHG_SCL), 0);
	spin_us(50);
	if (!bb_recover("the charger's bus")) {
		problem = "the bus stays low";
	}
	for (unsigned r = 0; problem == NULL && r < sizeof(regs); r += 8) {
		problem = bb_read((uint8_t)r, regs + r, 8);
	}
	wr(BB_CFG(PIN_CHG_SDA), was_sda);
	wr(BB_CFG(PIN_CHG_SCL), was_scl);
	bb = was;
	charger_dumps++;
	if (problem != NULL) {
		printf("charger: S2MU106 read %u: %s%s\n", charger_dumps, problem, charger_dumps >= 3 ? "; no more tries" : "");
		charger_dump_off = charger_dumps >= 3;
		return;
	}
	for (unsigned r = 0; r < sizeof(regs); r += 16) {
		int at = snprintf(line, sizeof(line), "charger: S2MU106 %02x:", r);

		for (unsigned i = 0; i < 16; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", regs[r + i]);
		}
		printf("%s\n", line);
	}
}

static void
step_watch(void)
{
	static uint8_t events[SEC_TS_EVENT_COUNT * SEC_TS_EVENT_SIZE];
	unsigned left, ticks = 0;
	const char *problem;

	printf("touch: step 7: watching the interrupt line every %u us, posting the touches\n", POLL_US);
	for (;;) {
		if (!(rd(GPIO_IO(PIN_INT)) & 1) || any_finger_down()) {
			uint64_t start = mach_absolute_time(), taken;

			problem = sec_ts_read(SEC_TS_READ_ONE_EVENT, events, SEC_TS_EVENT_SIZE);
			if (problem == NULL && events[0] != 0) {
				/*
				 * How many more wait: the low five bits (Linux's sec_ts_event_coordinate:
				 * left_event:5, max_energy:1, ttype_1_0:2). Read as six, the max_energy bit
				 * that this chip sets in nearly every event made it 32 -- 31 events asked for
				 * that were not there, and now and then a lift lost with them.
				 */
				left = events[7] & 0x1f;
				if (left > SEC_TS_EVENT_COUNT - 1) {
					left = SEC_TS_EVENT_COUNT - 1;
				}
				if (left != 0) {
					problem = sec_ts_read(SEC_TS_READ_ALL_EVENT, events + SEC_TS_EVENT_SIZE, left * SEC_TS_EVENT_SIZE);
				}
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &taken);
			reads++;
			read_ns += taken;
			if (taken > read_longest_ns) {
				read_longest_ns = taken;
			}
			if (reads == 10) {
				printf("touch: the first 10 reads of events took %llu us each (the longest %llu)\n", read_ns / reads / 1000,
				    read_longest_ns / 1000);
			}
			if (problem == NULL && events[0] != 0) {
				for (unsigned i = 0; problem == NULL && i <= left && events[i * SEC_TS_EVENT_SIZE] != 0; i++) {
					handle_event(events + i * SEC_TS_EVENT_SIZE);
				}
			}
			if (problem != NULL && read_errors++ < 8) {
				printf("touch: reading events: %s\n", problem);
			}
		}
		/* once in a while a line about what has happened */
		if (++ticks % (60 * 1000000 / POLL_US) == 10 * 1000000 / POLL_US) {
			printf("touch: so far %llu events, %llu touches, %llu failed reads; a read takes %llu us (the longest %llu)\n",
			    events_read, touches_down, read_errors, reads ? read_ns / reads / 1000 : 0, read_longest_ns / 1000);
		}
		/* r5q: the battery level, every 5 s, while the touch chip is quiet */
		if (r5q && ticks % (5 * 1000000 / POLL_US) == 2 && (rd(GPIO_IO(PIN_INT)) & 1) && !any_finger_down()) {
			lck_mtx_lock(&r5q_pmic_lock);
			gauge_poll();
			lck_mtx_unlock(&r5q_pmic_lock);
		}
		if (r5q && ticks % (60 * 1000000 / POLL_US) == 3 * 1000000 / POLL_US && (rd(GPIO_IO(PIN_INT)) & 1) &&
		    !any_finger_down()) {
			lck_mtx_lock(&r5q_pmic_lock);
			charger_dump();         /* (Test 87: charger_dump=1, reads only) */
			lck_mtx_unlock(&r5q_pmic_lock);
		}
		if (r5q && ticks == 30 * 1000000 / POLL_US && (rd(GPIO_IO(PIN_INT)) & 1) && !any_finger_down()) {
			lck_mtx_lock(&r5q_pmic_lock);
			campmic_peek();         /* (Test 270: campmic=1, the cameras' regulators, reads only) */
			lck_mtx_unlock(&r5q_pmic_lock);
		}
		if (r5q && ticks == 40 * 1000000 / POLL_US && (rd(GPIO_IO(PIN_INT)) & 1) && !any_finger_down()) {
			/*
			 * (Test 284) In a thread of its own: Tests 279-283 hung whenever the probe took more than ~3 s inside
			 * this (the touch) thread -- whatever the step -- and the earlier, sub-second probes never did.
			 */
			thread_t cam_thread;

			if (camsensor_on && !camsensor_done &&
			    kernel_thread_start(camsensor_thread, NULL, &cam_thread) == KERN_SUCCESS) {
				thread_deallocate(cam_thread);
			}
		}
		delay(POLL_US);
	}
}

static void
sec_ts_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	unsigned done = 1;

	bb_touch_thread = current_thread();     /* (Test 324) this thread's bit-banging has its own bus pointer */
	step_survey();
	if (steps >= 2 && (r5q ? step_r5q_supplies(false) : step_pmic())) {
		done = 2;
		if (steps >= 3 && (r5q ? step_r5q_supplies(true) : step_power())) {
			done = 3;
			if (steps >= 4 && step_clocks_pins()) {
				done = 4;
				if (steps >= 5 && step_identity()) {
					done = 5;
					if (steps >= 6 && step_sense()) {
						done = 6;
						if (steps >= 7) {
							step_watch();
						}
					}
				}
			}
		}
	}
	printf("touch: stopping here (touch_steps=%u, %u done)\n", steps, done);
	thread_terminate(current_thread());
}

/*
 * r5q (Test 126): touches from the PC, for testing iOS without a hand on the phone. The loader, which keeps serving
 * fastboot from another core while iOS runs, finds this mailbox in the loaded kernel by its first word (as it finds
 * loader_live) and fills it on "oem touch KIND X0 Y0 X1 Y1 MS" (0..65535 across iOS's screen, as the panel's touches
 * are scaled here), bumping seq last; this thread plays each new gesture as one more finger (id 9) through the same
 * redwood_touch_post() the panel's touches take: 1 a tap, 2 a swipe from (x0, y0) to (x1, y1) over MS, 3 a press
 * held MS. Nothing is read from or written to the touch chip.
 */
struct r5q_touch_mailbox {
	uint64_t magic;                         /* "XNUTOUCH" */
	volatile uint32_t seq;                  /* the loader's: one more after each gesture written */
	volatile uint32_t done;                 /* ours: the last seq played */
	volatile uint32_t kind, x0, y0, x1, y1, ms;
};
__attribute__((used, aligned(64))) struct r5q_touch_mailbox r5q_touch_mbox = { .magic = 0x484355544f554e58ULL };

static void
pc_touch_thread(__unused void *parameter, __unused wait_result_t result)
{
	r5q_touch_mbox.done = r5q_touch_mbox.seq;
	for (;;) {
		uint32_t seq = r5q_touch_mbox.seq;

		if (seq != r5q_touch_mbox.done) {
			uint32_t kind = r5q_touch_mbox.kind, x0 = r5q_touch_mbox.x0, y0 = r5q_touch_mbox.y0;
			uint32_t x1 = r5q_touch_mbox.x1, y1 = r5q_touch_mbox.y1, ms = r5q_touch_mbox.ms;

			if (x0 > 65535 || y0 > 65535 || x1 > 65535 || y1 > 65535 || ms > 10000) {
				printf("touch: from the PC: gesture %u out of range, not played\n", seq);
			} else if (kind == 4) {
				/* (Test 144) a hardware button pressed for MS: x0 is which (REDWOOD_BUTTON_LOCK 1, VOLUME_UP 2, VOLUME_DOWN 3) */
				if (x0 >= 1 && x0 <= 3) {
					redwood_touch_post(REDWOOD_BUTTON_DOWN, x0, 0, 0);
					delay_for_interval(ms ? ms : 120, NSEC_PER_MSEC);
					redwood_touch_post(REDWOOD_BUTTON_UP, x0, 0, 0);
					printf("touch: from the PC: button %u pressed for %u ms\n", x0, ms ? ms : 120);
				}
			} else if (kind == 2) {
				uint32_t steps = ms / 16 < 4 ? 4 : ms / 16;

				redwood_touch_post(REDWOOD_TOUCH_DOWN, 9, x0, y0);
				for (uint32_t i = 1; i <= steps; i++) {
					delay_for_interval(16, NSEC_PER_MSEC);
					redwood_touch_post(REDWOOD_TOUCH_MOVE, 9, (uint32_t)((int64_t)x0 + ((int64_t)x1 - x0) * i / steps),
					    (uint32_t)((int64_t)y0 + ((int64_t)y1 - y0) * i / steps));
				}
				redwood_touch_post(REDWOOD_TOUCH_UP, 9, x1, y1);
				printf("touch: from the PC: swipe %u,%u -> %u,%u in %u ms\n", x0, y0, x1, y1, ms);
			} else {
				redwood_touch_post(REDWOOD_TOUCH_DOWN, 9, x0, y0);
				delay_for_interval(kind == 3 ? (ms ? ms : 800) : 70, NSEC_PER_MSEC);
				redwood_touch_post(REDWOOD_TOUCH_UP, 9, x0, y0);
				printf("touch: from the PC: %s at %u,%u\n", kind == 3 ? "press" : "tap", x0, y0);
			}
			r5q_touch_mbox.done = seq;
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
}

void
redwood_sec_ts_init(void)
{
	uint32_t enabled = 0, steps = 7;
	thread_t thread;

	if (redwood_platform_is("r5q")) {
		uint32_t pc = 1;

		if (!(PE_parse_boot_argn("pc_touch", &pc, sizeof(pc)) && pc == 0) &&
		    kernel_thread_start(pc_touch_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (redwood_platform_is("r5q")) {
		/* Galaxy S10 Lite (SM8150): see the note at the top */
		r5q = true;
		o_vote = 0x52014;
		o_m_ahb = 0x1e004;
		o_s_ahb = 0x1e008;
		o_se_cbcr = 0x1e144;
		o_se_cmd = 0x1e148;
		o_core = o_core2x = 0;
		v_core = 1u << 0;
		v_s_ahb = 1u << 1;
		v_m_ahb = 1u << 2;
		v_core2x = 1u << 3;
		v_se = 1u << 4;
		p_tlmm_bus = 0x03000000UL + 0xd00000UL;   /* SOUTH: GPIO55/56, 131, 140 */
		p_tlmm_int = 0x03000000UL + 0x500000UL;   /* EAST: GPIO87 */
		p_se = 0x00c80000UL;
		PIN_SDA = 55;
		PIN_SCL = 56;
		PIN_INT = 87;
		PIN_FUNCTION_QUP5 = 1;                    /* qup17 */
		touch_bus.sda = PIN_SDA;
		touch_bus.scl = PIN_SCL;
	} else if (!redwood_platform_is("y2q")) {
		return;
	}
	PE_parse_boot_argn("touch_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("touch_log", &touch_log, sizeof(touch_log));
	{
		uint32_t dump = 0;

		charger_dump_on = PE_parse_boot_argn("charger_dump", &dump, sizeof(dump)) && dump != 0;
		dump = 0;
		campmic_on = PE_parse_boot_argn("campmic", &dump, sizeof(dump)) && dump != 0;
		dump = 0;
		camsensor_on = PE_parse_boot_argn("camsensor", &dump, sizeof(dump)) && dump != 0;
		dump = 0;
		camrx_on = PE_parse_boot_argn("camrx", &dump, sizeof(dump)) && dump != 0;
		camrx_level = camrx_on ? dump : 0;
	}
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(sec_ts_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
