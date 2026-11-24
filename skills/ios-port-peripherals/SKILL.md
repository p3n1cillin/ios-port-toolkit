---
name: ios-port-peripherals
description: Bring up touch, battery gauge, hardware buttons, CPU frequency and (later) Wi-Fi in a native iOS (XNU) port on a Qualcomm phone, using the device profile's extracted addresses - bit-banged I2C on GSI-only serial engines, SPMI observer-channel PMIC reads, OSM/EPSS cpufreq. Use when porting or debugging one of these drivers for the S10 Lite or a new phone.
---

# Peripherals: touch, battery, buttons, cpufreq

The kernel drivers live in the device's kernel overlay (S10 Lite: `work/ios-r5q/kernel-build/r5q-overlay/bsd/dev/`).
Each driver picks its per-device table by the loader's platform name (`redwood_platform_is("r5q")`). A new phone
adds a table entry filled from its profile. Get the values with `tools/extract_profile.py` and check them with
`tools/validate_profile.py`.

## Ground rules

- **Read first.** Each driver goes in numbered steps (`touch_steps=N`, `buttons_steps=N`). Every step says in the
  log what it is about to do before doing it. The first steps only read.
- **Never read a block whose clocks are off.** Read the GCC branch (CBCR bit 31 = off) before the serial engine.
- **Never write a PMIC.** Buttons and the PON are read through the SPMI arbiter's *observer* channel. The only
  write is the arbiter command word that asks for a read. The touch chip's supplies are raised only if they read
  low. On the S10 Lite they are GPIO-switched fixed regulators. A PMIC LDO, as on the S20+, needs the owner's
  explicit go-ahead before its first write.
- Log through `loader_log_putc` too, so the lines reach the RAM ring and the log slot during a run.

## Touch (I2C)

- Profile fields: `touch.*`. The extractor resolves the engine, the bus pins and their mux function, the TLMM
  tile, the IRQ pin, the supplies, and the GCC vote register, bits, CBCRs and RCG.
- If the engine's FIFO interface is disabled (GSI DMA only, as on the S10 Lite), bit-bang SDA/SCL as GPIOs at
  100 kHz. Lines are open drain: drive low by making the pin an output at 0, release by making it an input.
  A read of one event takes about 1.4 ms.
- Samsung `sec_ts` speaks the s6sy761 protocol. Read the chip id, panel size and event FIFO.

## Battery gauge

- Profile fields: `battery.*` and `battery.registers`. These are read-only registers taken from the vendor
  driver source.
- On the S10 Lite, the S2MU106 at 0x3B on GPIO57/58 is polled every 5 s by the touch thread while no finger is
  down. RSOC is scaled by `capacity_max`. On the S20+, the MAX77705 at 0x66 on GPIO28/29 (donor driver).

## Buttons

- Profile fields: `buttons.*`. Power is KPDPWR of the main PMIC's PON. Volume down is RESIN. Volume up is
  usually a PMIC GPIO (`gpio_keys`). Its peripheral id is `sid << 8 | (0xC000 + (n-1)*0x100) >> 8`.
- Arbiter v5 layout: core/chnls/obsrvr/cnfg come from the DT `reg-names`. Find each peripheral's channel in the
  core table, then read through `obsrvr + 0x10000*ee + 0x80*channel`.
- Check the PON subtype range and status bits against the PON generation (gen2: KPDPWR 0x01, RESIN 0x02;
  PMK8350 gen3: 0x80/0x40).

## CPU frequency

- `cpufreq.kind`: `osm` (SM8150, `qcom,clk-cpu-osm`: LUT at 0x110, 32-byte rows, perf state 0x920) or
  `epss` / `cpufreq-hw` (SM8250+: LUT 0x100, 4-byte rows, perf state 0x320).
- Set each cluster that has a running core to its top row. The hardware switches the source and voltage itself.
  Its limits block still throttles when hot.
- Boot-arg `cpufreq=1`. With `cpus=8`, all cores run (loader `R5Q_LAB_SMP`, kernel `cpus=8`).
- `cpufreq=2` (S10 Lite, Test 439): a governor thread moves each cluster between its lowest and top LUT rows by
  load, with `kern.redwood.cpufreq_state` for telemetry. Pair it with event-driven driver threads (`msleep`/`wakeup`
  instead of polling: the S10 Lite went from >12,000 to ~350 wakeups a second) and longer poll periods while the
  panel is off (`redwood_display_asleep()`), or the cores never idle long enough for it to matter.

## Wi-Fi (working on the S10 Lite)

`wifi.kind` icnss is the WCN399x inside the SoC. It needs the modem/WLAN PIL firmware path and GLINK over SMEM,
which makes it a much bigger job than PCIe (QCA6390/6490, BCM43xx). The donor did the S20+'s BCM4375 over PCIe
with brcmfmac.

On the S10 Lite it works: the kernel's own WLFW/ath10k-style driver, WPA2, iOS's wifid through an Apple80211
ioctl shim (`redwood_apple80211.c`: scan, join, link rate), automatic rejoin. For the full story follow
`ports/r5q/wifi/WIFI-PLAN.md` (in the repo; `work/ios-r5q/wifi/` locally). Each stage is one test, and each stage
reads before it writes. The first stage is the read-only survey in `redwood_wifi.c` (`wifi_survey`): the SMEM table of
contents at 0x86000000, the SMP2P items, the AOP mailbox and the APCS doorbell. Writes later go only to the
modem's own reserved memory and SMEM items.
