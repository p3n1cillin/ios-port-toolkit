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
static struct bitbang *bb = &pmic_bus;  /* the bus the functions below drive */

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

	if (chg_se_off) {
		return false;
	}
	if (chg_se == 0) {
		chg_se = ml_io_map(R5Q_SE4_PHYS, 0x4000);
		/* the loader's set-up must be there: the I2C protocol, FIFO mode, the divider it wrote */
		if (((rd(chg_se + 0x68) >> 8) & 0xff) != 3 || (rd(chg_se + 0x48) & 0xff1) != 0x71) {
			printf("charger: SE4 is not set up for I2C (fw %#x, clk %#x): status not read\n", rd(chg_se + 0x68),
			    rd(chg_se + 0x48));
			chg_se_off = true;
			return false;
		}
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
			gauge_poll();
		}
		if (r5q && ticks % (60 * 1000000 / POLL_US) == 3 * 1000000 / POLL_US && (rd(GPIO_IO(PIN_INT)) & 1) &&
		    !any_finger_down()) {
			charger_dump();         /* (Test 87: charger_dump=1, reads only) */
		}
		delay(POLL_US);
	}
}

static void
sec_ts_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	unsigned done = 1;

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

void
redwood_sec_ts_init(void)
{
	uint32_t enabled = 0, steps = 7;
	thread_t thread;

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
	}
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(sec_ts_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
