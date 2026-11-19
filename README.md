# Native iOS porting toolkit (Android phones: Qualcomm first, MediaTek and Exynos/Tensor started)

> **This repository** also carries the S10 Lite port's own working tree in [ports/r5q](ports/r5q/README.md)
> (loader, kernel drivers, build pipeline, phone tool, iOS-side libraries and test tools, notes; up to Test 328:
> Wi-Fi, sound, sensors with rotation, auto-brightness and CoreMotion device motion, Bluetooth under iOS's own
> bluetoothd, and the phone's own rear camera in iOS's Camera app: preview under iOS's auto-exposure, photos saved).
> Set-up: [docs/SETUP.md](docs/SETUP.md). Licences: MIT, with the loader and kernel drivers under the Apple Public
> Source License 2.0: [NOTICE.md](NOTICE.md).

This toolkit captures how iOS 16.5 was made to boot natively on the **Samsung Galaxy S10 Lite** (SM-G770F,
`r5q`, Snapdragon 855 / SM8150). As of Test 68 the phone runs to a usable home screen with:

- an aligned display and touch
- all 8 cores at full clocks and the right time
- a battery gauge and hardware buttons

None of it was flashed permanently: every test went to the recovery partition and was restored automatically.

The port built on a donor port (redwood-src: POCO X5 Pro, plus the Galaxy S20+ `y2q`). The toolkit turns
what was hard-coded or hand-copied per test into four things:

- a **device profile** (one JSON file per phone)
- **tools** that fill and check that profile from the phone's own device tree
- a **generator** that writes each test's build and bundle scripts from a small spec
- **Claude Code skills** for the recurring jobs: safety, a new device, the next test, the display, peripherals,
  and reading results

It serves three uses:

- the next tests on the S10 Lite
- judging whether another phone is portable ([BRANDS.md](BRANDS.md), [ARCHITECTURES.md](ARCHITECTURES.md),
  `tools/assess_device.py`)
- starting a port to that phone

> **Safety first:** [SAFETY.md](SAFETY.md).
>
> - **Samsung:** tests write only `recovery`, restore it automatically, and never touch boot or vbmeta.
>   A Download Mode restore flashes only the original recovery.
> - **Phones with `fastboot boot`:** tests are RAM-only and write nothing.
> - **Everywhere:** no PSCI reset and no PMIC write. Wiping Android, which includes unlocking a bootloader,
>   needs a stock-firmware download plus the owner's explicit confirmation.
>
> Only `runner/run_test.py` (with the operator it drives) and `runner/fastboot_boot.py` touch a phone. Both
> refuse to run without an authorization string and a passing safety check.

## Layout

```
toolkit/
  README.md            this file: the method, what to gather, quick starts, what is generic
  SAFETY.md            the rules R1-R7 (+ R1f for fastboot-boot) and where each is enforced
  BRANDS.md            per brand: entry method (recovery write+restore / fastboot boot RAM-only), unlock, restore
  ARCHITECTURES.md     what carries over to Qualcomm / MediaTek / Exynos / Tensor, and the feasibility tiers
  PROFILE.md           every profile field: type, stage that needs it, where it comes from, r5q example (generated)
  brands.json          the brand data assess_device.py uses
  install-skills.ps1   copies skills/* to <project>/.claude/skills so Claude Code picks them up
  devices/
    r5q.json           Galaxy S10 Lite (Samsung, SM8150, recovery): complete, values from the working port
    r5q.local.json     PRIVATE: serial, absolute paths (never share)
    y2q.json           Galaxy S20+ 5G (Samsung, SM8250): from the donor port + its DT; safety values need the phone
    redwood.json       POCO X5 Pro (Xiaomi, SM7325, fastboot-boot, SPI touch): from the donor port + its DT
    *.conf             each profile in the donor's format (common/devices/<codename>.conf), generated
  socs/
    <qcom soc>.json    24 Qualcomm SoCs from Linux: GPIO -> TLMM tile + mux functions, GCC clock -> CBCR/vote
                       registers, QUP engine address -> clock, GIC/TLMM/GCC bases
    <mt soc>.json      10 MediaTek SoCs: pin modes, pio blocks, I2C controllers
    <exynos>.json      8 Exynos/Tensor SoCs (incl. gs101): pin banks per controller
    aliases.json       part numbers / code names -> the data that applies (sm7325 -> sc7280, s5e9830 -> exynos990)
    mu-silicium.json   which SoCs/devices have a Mu-Silicium UEFI (88 devices, 33 SoCs)
  tests/fixtures/      synthetic MediaTek (mt6797) and Tensor (gs101) trees built from upstream data
  tools/
    dtslib.py          parser for `dtc -O dts` output (phandles, reg, pinctrl states, vendor-prefixed names)
    qcom_socdata.py    Linux tree -> socs/<qcom soc>.json (--all-phone)
    soc_other.py       Linux tree -> socs/<mediatek|exynos soc>.json
    mu_silicium_index.py  Mu-Silicium checkout -> socs/mu-silicium.json
    assess_device.py   feasibility: tier A-D/X, brand method, UEFI ports on the same SoC, kernel requirements
    bootimg.py         Android boot images v0-v4 + vendor_boot v3/v4: info, repack the kernel (test_bootimg.py)
    extract_profile.py live DT -> candidate profile values, each with its evidence; --compare to a profile
    new_device.py      start devices/<codename>.json (+ .evidence.json, .local.json) from a live DT (+ a donor .conf)
    conf_bridge.py     JSON profile <-> the donor's common/devices/<codename>.conf (export / import / check)
    validate_profile.py  per-stage checklist of missing fields + consistency checks (overlaps, frames, slot, ...)
    profile_spec.py    the profile format (fields, stages, sources); profile_doc.py writes PROFILE.md from it
    selftest.py        checks the whole toolkit against the real S10 Lite port (host-only)
  generator/
    gen_test.py        profile + test spec -> the test's build/package/check/bundle scripts, loader profile,
                       runner markers, stage script, next-steps note
    templatize.py      how the templates were made from Test 68 (re-runnable)
    templates/qcom-userland/   the per-test files with @@PLACEHOLDERS@@
  tests/r5q/           test66.json, test67.json, test68.json (specs that regenerate the real tests),
                       test69.example.json (how the next one is written)
  runner/
    safety.py          the rules as code: check_bundle, assert_shell_safe, assert_download_restore, assert_wipe_allowed
    operator.ps1.tmpl  the guarded Check/Stage/Start/Restore operator, profile-driven (make_operator.py renders it)
    run_test.py        one authorized run: Start, wait, log + log slot, Restore, baseline, markers (device-independent
                       run_native_test.py)
    fastboot_boot.py   one authorized RAM-only `fastboot boot` of a test image (fastboot-boot phones)
  skills/              ios-port-feasibility, ios-port-safety, ios-port-new-device, ios-port-next-test,
                       ios-port-display, ios-port-peripherals, ios-port-sensors, ios-port-log-triage
  docs/
    SENSORS.md         the sensors DSP (SLPI) from the kernel: FastRPC, the file server, SEE; where each value is
    IOS-SENSORS.md     iOS's side: virtual HID sensors, rotation, auto-brightness, CoreMotion, compass, magnetometer cal
    SYSTEM-PATCHING.md changing Apple code without breaking code signing: page hashes, the kernel's read-time cache
                       patch, patched daemons in /var, live reloads with r5qd
  tools/ios/           dyld shared cache and Mach-O readers/patchers (find code by its strings, patch a word and its
                       CodeDirectory hash), magfit.py (magnetometer offset, units and axes)
```

## Quick start

**The S10 Lite's next test**

1. Load the skill `ios-port-next-test`.
2. Copy `tests/r5q/test69.example.json` to `test69.json` and fill it in.
3. Generate: `python generator/gen_test.py --device devices/r5q.json --spec tests/r5q/test69.json --out <scratch>`
4. Follow the generated `NEXT-test69.md`: stage, check, bundle, add the runner case, then run with the owner's OK.

**Is this phone portable?**

Load the skill `ios-port-feasibility`, then run
`python tools/assess_device.py --dts live.dts --brand <brand>` (or `--profile`).

**A new phone**

1. Load the skill `ios-port-new-device`.
2. Dump the live DT (root): `adb shell su -c 'cat /sys/firmware/fdt' > live.dtb && dtc -I dtb -O dts -q live.dtb -o live.dts`
3. `python tools/new_device.py <codename> --dts live.dts --model <SM-XXXX>`
4. `python tools/validate_profile.py devices/<codename>.json --dts live.dts`. Gather what it lists, in stage order.

**Check the toolkit:** `python tools/selftest.py` (17 checks with all options). Options:

- `--y2q-dts <y2q.dts>` and `--donor-conf-dir <copy of the donor's common/devices>` add the S20+ checks.
- `--mkbootimg <AOSP mkbootimg.py>` adds the boot-image v0-v4 checks.

## Relation to the donor's device profiles

The donor (HackiOS; its full working copy in WSL is `/var/tmp/codex-ios-r5q/work/linux-source`) already describes
devices with shell profiles. They live in `common/devices/<codename>.conf`: `_template.conf`, `redwood.conf`,
`y2q.conf` and `qemu-virt.conf`. `common/new-device.sh` makes them; `common/build.sh`, `common/run.sh` and `./xl`
source them. `docs/PORTING.md` explains them.

The toolkit builds on that format rather than replacing it:

- **The JSON profile is a superset.** Its `donor` section holds the donor's variables value for value:
  `KIND`, `SOC`, `BOOT_VIA`, `UEFI_TARGET`, `UEFI_FD`, `DTB`, `WIFI_ATH11K`, `BOOTARGS`, `DEFAULT_SECS`,
  `USERLAND_VARS`, `FIRMWARE`, `AUTOBOOT_ARGS` and `SMP`. `FD_BASE`, `FD_SIZE`, `PAYLOAD_BASE` and `FB_BASE`
  map to `memory.*` and `loader.platform_struct.fb_base`. `conf_bridge.py check` confirms that `y2q.json` agrees
  with the donor's `y2q.conf` on all 10 shared variables.
- **`conf_bridge.py export`** writes `devices/<codename>.conf` in the donor's format. It is generated, so edit the
  JSON. Copied into the donor's `common/devices/`, `common/build.sh r5q` builds the kernel, the loader and the
  `REST=1` images, the donor's phone recipe.
- **Extended for a recovery-tested phone**, where the donor format had no place:
  - `KIND=phone-recovery`, a new kind. `build.sh` skips its `fastboot boot` image step (that step is KIND=phone
    only), and `run.sh` would reject the kind anyway.
  - `BOOT_VIA=recovery-test`, never `twrp`, because that writes the boot partition.
  - A guard at the top of the file: sourced by `./xl` or `common/run.sh`, it exits 1 before anything runs.
    Their phone paths do `adb reboot bootloader` + fastboot, or write boot from TWRP. This was checked by
    sourcing it in bash from scripts named `xl`, `run.sh` and `build.sh`.
  - `IOSPORT_*` variables for what the donor has no place for: recovery-only flashing (partition, size,
    baseline hashes, Download Mode restore), the RAM ring and the log slot, the USB RAM drive and the SD card,
    the resident image and kernel window, the panel fixes, the virtual timer, and the path of the full JSON.
- **`conf_bridge.py import`** reads any donor `.conf` as text (it never executes it). `new_device.py --conf`
  merges it over the device-tree candidates.

The donor's own `y2q` flow writes boot from TWRP. The toolkit's `y2q.conf` export turns that into the
recovery-only form, and the donor's original stays recorded in `donor.boot_via`.

## The method, step by step

### 0. Preconditions

You need:

- An unlocked bootloader and root (Magisk) in Android.
- A `recovery` partition that the stock bootloader (ABL) boots, holding an Android boot image. The packager
  handles header v2. Samsung adds an AVB footer and a `SEANDROIDENFORCE` trailer.
- A Qualcomm SoC with a Mu-Silicium port for it or a sibling.
- A Linux build box (WSL is fine) with clang/lld, dtc, avbtool and unicorn, and the donor's XNU sources and
  build environment.

The S10 Lite has no fastboot `boot` and no permanent changes were wanted. Everything runs from recovery and
goes back.

### 1. The boot chain

```
ABL (stock) --loads recovery.img as a Linux "Image"-->  BootShim  -->  Mu-Silicium UEFI FD  -->  loader.efi  -->  XNU
                                                          |  copies FD to its base, XNUP payload to free RAM; colour bands + ring log
```

- **BootShim** (`port/test<N>/BootShim.S`) is the "kernel" in recovery.img: an ARM64 Image header, then the FD,
  then an `XNUP` payload. It probes the panel and paints colour bands as progress (green: display set up; blue:
  FD copied; white or red: payload accepted or rejected). It logs into the Samsung RAM ring (`memory.log_ring`).
  It copies the FD to `memory.uefi_fd.base` and the payload to `memory.payload.base`, cleans caches, and jumps
  into SEC. Device values: `-DFD_BASE/FD_SIZE/PAYLOAD_BASE/PAYLOAD_MAX`, plus the `MDSS`, `INFO` and
  `SEC_LOG_BASE` defines. The generator's stage script sets them from the profile.
- **The UEFI FD** is a Mu-Silicium platform package built from the phone's own XBL modules and `uefiplat.cfg`.
  There are two flavours:
  - minimal: our boot manager only; ends with a warm reset that keeps the RAM ring
  - full: every Qualcomm DXE, needed for USB and storage; ends with a cold reset that wipes the RAM ring
  Its MemoryMapLib must reserve every live-DT `no-map` region the stock map leaves out (Test 40).
  The Qualcomm DXEs need about 60 MiB of DXE heap (Tests 37-39).
- **loader.efi** (`port/xnuloader/loader.c`) finds its `struct platform` by the GOP framebuffer base. It
  validates the firmware map and builds the device tree for XNU. For lab runs it saves its context so the
  kernel can come back (timed returns with the kernel log). It hands off with **ABL's own shutdown sequence**:
  set/way flush, then SCTLR C/I/M off. It does **no per-region DC clean** over the firmware map, because that hung
  the S10 Lite (Tests 15-19, fixed in Test 31). Compile-time flags (`R5Q_*`) select each test's behaviour.
  The generator writes them into `port/xnuloader/build-test<N>.sh`.
- **XNU** is the donor's kernel with the device's overlay on top (`kernel-build/<codename>-overlay`):
  - `gic_v3_irq.c`: the virtual-timer INTID from `gic_vtimer=` (SM8150 wires it to PPI 3, INTID 19, not ARM's 27)
  - the touch, gauge, buttons and cpufreq drivers, with per-platform tables
  - the framebuffer driver: flip buffers, HW_CTL clock reading, fbscale

### 2. Recovery-only test flashing with automatic restore

Each test is a **bundle** (`outputs/.../test<N>/`) holding:

- the test recovery image, the original recovery and the reboot helper
- `manifest.json` with the hashes and a `start_blocked_reason` guard
- `Manage-Test<N>.ps1`, the guarded operator: `Check`, `Stage`, `Start`, `Restore`

The runner reads the previous `last_kmsg` first. `Start` writes recovery and reboots into it with the helper.
The runner then waits for Android with a new boot id and root. It reads `last_kmsg` and the **log slot**,
restores the original recovery, verifies recovery, boot and vbmeta, and checks the expected markers in order.
This test's anchor marker must be newer than in the previous boot. Exit codes are 0, 2, 3 and 4
(skill `ios-port-log-triage`).

### 3. The RAM log and the log slot

- Samsung keeps a RAM ring (`/proc/last_kmsg`) at `sec_debug_region + 0x200000`. It has a 16-byte header with
  the magic "LOGM" and the write index, and it survives a **warm** reset. BootShim, the firmware's DEBUG writer
  (`RingFbSerialPortLib`) and the loader all append to it.
- A cold reset (full firmware PS_HOLD, forced key reset) wipes it. So the full firmware copies the ring into a
  **log slot** inside the test recovery image's zero padding, after the AVB data and before the footer
  (`log_slot.offset`, 513 blocks). It writes only after reading the slot's magic, and saves again just before
  its final reset. The runner reads the slot back with `dd` before Restore. The original image is zero there, so
  Restore also clears it.

### 4. USB RAM-disk delivery of the system image

The full firmware serves a 3 GiB FAT32 RAM disk through Qualcomm UsbMsdDxe (`port/r5qfull/R5qUsbRamDisk.c`).
It first signals the USB controller init event group (`firmware.usb_controller_init_event`), or UsbfnIo never
appears (Tests 45-48).

The PC (`usb_msd_session.py`) waits for the volume labelled `firmware.usb_msd_label` and copies `SYSTEM.APF`
(the iOS system volume, 2.49 GB, about 4 minutes at 10 MiB/s) and `EPOCH.TXT`. It finishes with `DONE.TXT`
(name, size, crc32).

The firmware then:

1. rebuilds `SYSTEM.APF` at `memory.resident_image.base` and checks its CRC
2. releases it without clearing (`PcdDebugPropertyMask` 0x27)
3. patches the epoch into the payload cmdline

The loader finds it as the resident image (md0, the root). The payload's `root.apfs` becomes md1.

Build the root volume with `REST=1` (the donor's phone recipe; `common/build.sh` sets it).

From firmware fulldbg23 (Test 69 on), the firmware can also load `SYSTEM.APF` from a **FAT32 microSD card**
(R5qFileSystemLoad). exFAT is not read. `work/ios-r5q/prepare_sdcard.py` writes `SYSTEM.APF` and `SYSTEM.CRC`
to the card once, so the 4-minute USB copy is no longer needed every run (`userland.system_sources`).

### 5. Display: panel window and DSI fixes

On the S10 Lite the picture came out shifted by a per-boot amount. The pipe fetch was correct. The panel's own
write window had been garbled early in boot. The fix (`R5qPanelSync.c`, Test 61):

1. Turn autorefresh off and drain.
2. Reset the DSI at the **shifted** register 0x118 (the DSI 6G registers are offset by 4).
3. Resend DCS `13`, `2A 0..w-1`, `2B 0..h-1` by command DMA from the bootloader's buffer, with the done mask on.
4. Send one clean frame.
5. Set autorefresh back to the stock value (every second TE).
6. Release the command page before the USB stage (Test 63).

Later work added flip buffers inside Display Reserved and `fbscale=2`. The SM8150 `disp_ahb` branch is HW_CTL,
so its CLK_OFF bit reads 1 when the bus is idle, and that is not a lost clock. Skill: `ios-port-display`.

### 6. Touch, battery, buttons: bit-banged I2C and SPMI reads

- **Touch** (`sec_ts`) sits on a QUP serial engine whose FIFO interface is disabled (GSI DMA only). The driver
  votes the engine's clocks (GCC), muxes the pins, and **bit-bangs I2C at 100 kHz** on SDA/SCL as GPIOs. It reads
  the chip id, panel size and events. The supplies are GPIO-switched regulators: read them, raise only if low.
- **Battery**: the S2MU106 fuel gauge sits on another engine's pins and is read the same way, read-only, every 5 s.
- **Buttons**: power (KPDPWR) and volume down (RESIN) are bits in the main PMIC's PON status. Volume up is a
  PM8150L GPIO. All are read through the SPMI arbiter's observer channel. Nothing is ever written to a PMIC.

Every address comes from the profile, and the extractor finds the pins, tiles, mux functions and clock registers.
Skill: `ios-port-peripherals`.

### 7. cpufreq

SM8150 uses OSM blocks per cluster (`qcom,clk-cpu-osm`): set the perf-state register to the top LUT row. SM8250+
uses EPSS/cpufreq-hw with another layout (`cpufreq.*`). Boot-arg `cpufreq=1`. All 8 cores come up with
`cpus=8` (loader `R5Q_LAB_SMP`).

### 8. Sensors: the SLPI from the kernel

The phone's sensors sit behind a Qualcomm DSP (the SLPI). With no Android, the kernel does what Android's daemons
did:

- it starts the SLPI and serves its file reads (the sensor registry and configs) over FastRPC;
- it speaks the sensors framework's protocol (SEE, protobuf over QMI) to stream the accelerometer, gyroscope,
  magnetometer, light and proximity.

Every value comes from the phone's device tree, and the order matters. Details: [docs/SENSORS.md](docs/SENSORS.md).
Skill: `ios-port-sensors`.

### 9. Sensors in iOS

A library in backboardd makes virtual HID services that look like an iPhone's accelerometer, gyroscope and compass,
and drives:

- rotation, through CoreMotion's orientation manager;
- auto-brightness, through BackBoardServices. An Apple-style light service crashes CoreBrightness.

CoreMotion device motion and the compass need two small patches to Apple's per-model tables. Details:
[docs/IOS-SENSORS.md](docs/IOS-SENSORS.md).

### 10. Changing Apple code safely

When iOS decides something from a fixed per-model table:

- patch the one instruction **and its page's hash** in the CodeDirectory;
- for the shared cache, do it in the kernel's APFS reader as the file is read.

A wrong page hash silently strips every process's entitlements; on the S10 Lite that meant no touch screen.
`tools/ios/` finds code by its strings and computes the hash. r5qd's `reload` swaps an injected library without a
reboot. Details: [docs/SYSTEM-PATCHING.md](docs/SYSTEM-PATCHING.md).

### 11. Bluetooth: a Qualcomm controller under Apple's bluetoothd

The kernel owns the chip: it powers the WCN399x over a polled GENI UART and loads its firmware as Linux `btqca`
does. It then hands iOS an H4 byte stream on the kernel-control socket `com.apple.uart.bluetooth`. Apple's own
bluetoothd runs **unpatched**, with:

- one environment variable that picks that socket;
- a stand-in for BlueTool;
- a registered IOService named `bluetooth`;
- an HCI filter that answers the Broadcom vendor commands bluetoothd sends.

On the S10 Lite this gives LE scanning, classic discovery and a working Settings > Bluetooth. Details, including
each failure on the way: [docs/BLUETOOTH.md](docs/BLUETOOTH.md).

### 12. Camera: a CAMSS sensor into Camera.app

First prove the iOS side with no sensor at all: a synthetic chart through `/dev/redwood-camera` and a camera vendor
inside mediaserverd. Then bring up the real path one step per test, logging after each step:

1. the rails and camcc;
2. the sensor's power, MCLK and ID;
3. its tables and exposure;
4. CSI-PHY and CSID (count packets and errors);
5. IFE-lite into a buffer below 4 GB through one SMMU stream match;
6. a ping-pong session that hands frames to iOS.

On the S10 Lite the S5K3L6's frames reach iOS this way. The recipe and the lessons that each cost a hang (slow AHB
on TCXO, never voting a rail to 0 that the boot raised): [docs/CAMERA.md](docs/CAMERA.md).

## What to gather for a new phone, in this order

Everything is read-only until step 7. `validate_profile.py` shows what is still missing for each stage.

1. **Identity and the way back** (`device.*`, `flash.*`, `baseline.*`): model and codename. Then root `dd`
   backups and sha256 of recovery, boot, vbmeta, dtbo and xbl. Also the recovery block node and size, the
   Download Mode procedure, and a stock firmware download for the exact model.
2. **Live device tree** to `tools/new_device.py`. That gives the CPU, GIC, timer INTID, reserved memory,
   TLMM, touch, battery, buttons, cpufreq, TE pin and Wi-Fi candidates, with evidence.
3. **XBL `uefiplat.cfg`** (UEFIExtract on the xbl backup) for `memory.uefi_fd`, `memory.display_reserved`, the
   DXE heap and the stock reservations. Run `validate_profile.py --dts` to list the no-map regions to add.
4. **recovery.img header** for `bootimg.*`, and where the log slot fits in its padding (`log_slot.offset`).
5. **Free RAM** for `memory.payload`, `memory.loader_pe_base` and `memory.loader_window_check` (check them
   against all reservations). Later, `memory.xnu_window` and `resident_image` come from the full firmware's
   probe (largest free block; S10 Lite Test 44).
6. **Panel**: size, MDSS base, TE pin. Then, if the picture is misaligned, a read-only MDSS/DSI dump for
   `display.abl_command_buffer` and `autorefresh`.
7. Then the **test ladder** (skill `ios-port-new-device`, step 4), one authorized run at a time.

## What is generic now, and what still needs per-device work

**Generic (driven by the profile and specs):**

- The profile format, validator and checklist, aware of the entry method (recovery or fastboot-boot), the
  touch bus (I2C or SPI) and the system-image source. Three profiles exist:
  - `r5q` is complete and passes every stage.
  - `y2q` (Samsung) and `redwood` (Xiaomi) are filled from the donor and their DTs, with the gaps listed.
- DT extraction of memory, CPU/GIC/timer, pins, touch (I2C and SPI), battery, buttons (PON generations 2 and 3),
  cpufreq (OSM/EPSS), TE pin and Wi-Fi (icnss/WPSS/PCIe/Broadcom/MediaTek). It reads Qualcomm, MediaTek and
  Samsung/Tensor pin syntaxes. SoC data comes from Linux: 24 Qualcomm, 10 MediaTek and 8 Exynos/Tensor SoCs.
  Results so far:
  - On r5q it agrees with the working port on 77 of 80 values; the other 3 are refinements made by hand.
  - On y2q (S20+) and redwood (POCO X5 Pro) it reproduces the donor drivers' touch clock registers and pins.
  - On synthetic MediaTek and Tensor trees it resolves I2C pins, modes and pin registers.
- Feasibility: tiers from the SoC data, the Mu-Silicium index (which SoCs have a UEFI) and the brand table.
- Boot images v0-v4 and vendor_boot v3/v4. `bootimg.py` repacks the kernel byte-identically to AOSP mkbootimg,
  and reproduces the r5q stock recovery and the Test 68 image.
- Per-test scripts. `gen_test.py` reproduces S10 Lite Tests 66, 67 and 68 **byte for byte**: 7 files plus the
  runner case each. The inputs are a profile and specs of about 15 lines (66 and 67 extend 68). It also writes
  the staging, runner JSON and next-steps note that genNN.py did by hand.
- Safety: rule checks in code. The operator is rendered from the profile. A device-independent runner covers
  Start, wait, log slot, Restore, baseline and markers. A RAM-only `fastboot boot` runner whose safety check
  allows nothing that writes.

**Still per device (code, not data):**

- The **Mu-Silicium platform package**: memory map, APRIORI, DXE heap, the boot manager's USB, panel and
  log-slot pieces (`port/r5qfull/*.c` use r5q addresses as `#define`s).
- **loader.c** `struct platform` entries, and its `R5Q_*` flag names (the profile records `loader.flag_prefix`).
- The **kernel overlay drivers**' per-platform tables (touch, gauge, buttons, cpufreq, framebuffer). They are
  hand-written C today; the profile holds every value they need.
- **BootShim**'s fallback geometry and band layout (the defines are handled). The generator's `package_test`
  template is Samsung-recovery shaped (v2 + AVB footer + trailer). For other images, package with
  `tools/bootimg.py`, which handles every header version.
- **Non-Qualcomm peripherals:** MediaTek and Exynos/Tensor clocks, PMICs, I2C engines and display have no data
  or drivers yet (ARCHITECTURES.md). SoCs without a Mu-Silicium UEFI (Tensor, most Dimensity) need a UEFI port
  before anything else.
- **Runner wiring on the S10 Lite**: the generator emits the `run_stage_test_auto.ps1` case block, but
  `ValidateRange` and `$iosRuns` are still edited by hand. `runner/run_test.py` avoids that for other phones.
- **Wi-Fi, sound, sensors and the rest** on the S10 Lite:
  - Wi-Fi: icnss with both PCIe controllers disabled, WLFW, HTC/WMI, WPA2 in the kernel, `en0`;
  - sound through the ADSP;
  - sensors through the SLPI.

  They work, but the drivers are r5q's C. The step-by-step record, with every test's result, is
  `ports/r5q/wifi/WIFI-PLAN.md`. The sensors part is written up generically in `docs/SENSORS.md` and
  `docs/IOS-SENSORS.md`. Still blocked:
  - the microphone: the WCD9340 codec never enumerates on SLIMbus; WIFI-PLAN "Tests 237-240" lists what is ruled out;
  - Metal (no GPU driver for iOS);
  - Bluetooth, camera, cellular;
  - FairPlay (App Store apps).
- **Installation to UFS** (the owner's end state: iOS on userdata) is not started here, and is gated by rule R7.

**Not verified here:** `runner/run_test.py` and the rendered operator were not run against a phone (another
session holds the phone). They were compiled and parse-checked, and checked against the real Test 68 bundle
(read-only), and they mirror `run_native_test.py` and Manage-Test68.ps1 line for line apart from the extra
guards. `runner/fastboot_boot.py` was likewise only compiled and checked against its refusal list. The y2q
and redwood profiles are unmeasured, since neither phone is here. The MediaTek and Tensor fixtures are
synthetic, built from upstream data rather than a real phone's tree. The brand facts are general knowledge and
need checking per model.
