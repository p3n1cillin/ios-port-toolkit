---
name: ios-port-new-device
description: Start or continue a native iOS port to a new Android phone (Qualcomm first) with the toolkit - gather the device facts in the right order, extract a profile from the live device tree, validate it, and plan the bring-up test ladder. Use when someone wants to port to a phone other than the S10 Lite, or asks what to collect from their phone.
---

# Porting to a new phone

Read `toolkit/README.md` ("The method") once. Load the `ios-port-safety` skill before touching the phone.

## 1. Feasibility (no writes)

Run the skill `ios-port-feasibility` first (`tools/assess_device.py`). It gives a tier from A to X, the brand's
entry method, and the Mu-Silicium devices on the same SoC whose UEFI package to start from.

You need an unlocked bootloader. The brand decides how a test enters (BRANDS.md):

- **Samsung:** root (Magisk) plus the recovery partition. Tests write recovery and restore it after.
- **Phones with `fastboot boot`:** RAM only, nothing written, no root needed.

`tools/bootimg.py` handles boot image headers v0-v4.

SoC data:

- Qualcomm: `toolkit/socs/<soc>.json` (24 SoCs, and `aliases.json` maps part numbers to them).
- MediaTek and Exynos/Tensor: pins only, from `tools/soc_other.py` (ARCHITECTURES.md).

When data is missing, generate it from a Linux tree.

## 2. Gather, read-only, in this order

1. Back up and hash recovery, boot, vbmeta, dtbo and xbl with `dd` as root (`baseline.*`, `flash.*`). Keep the
   backups private.
2. Dump the live device tree: `su -c 'cat /sys/firmware/fdt'` > live.dtb, then `dtc -I dtb -O dts`.
   Use the **merged** live tree. A dtb taken from a boot/vendor_boot image is only the SoC base: the board's
   touch, keys and power key come from DTBO overlays. The extractor warns when it sees a base tree.
3. Scaffold the profile: `python toolkit/tools/new_device.py <codename> --dts live.dts --model <SM-XXXX>`.
   If the donor (HackiOS, `/var/tmp/codex-ios-r5q/work/linux-source` in WSL) already has
   `common/devices/<codename>.conf`, add `--conf` with that file so its values win. This writes
   `devices/<codename>.json`, `.evidence.json` and `.local.json`. Put the serial and paths in the `.local.json`.
   To use the donor's `common/build.sh` (kernel, loader, `REST=1` images), run
   `python toolkit/tools/conf_bridge.py export devices/<codename>.json -o devices/<codename>.conf` and copy the
   result into the donor's `common/devices/`. It is guarded: `./xl` and `common/run.sh` refuse it.
   Never give a recovery-tested phone `BOOT_VIA=twrp`, because that writes boot.
4. Extract the XBL's `uefiplat.cfg` (UEFIExtract on the xbl backup). From it take the FD base and size, Display
   Reserved, DXE heap, and every reserved region (`memory.*`, `firmware.uefiplat_cfg`).
5. Read the recovery.img header for `bootimg.*` (version, page size, the AVB footer, the SEANDROIDENFORCE trailer).
6. Check: `python toolkit/tools/validate_profile.py devices/<codename>.json --dts live.dts`.
   The MISSING list per stage is your to-do list. ERRORs are overlaps or bad geometry. Fix them before
   building anything.
7. For the later stages, still read-only: `python toolkit/tools/cam_map.py live.dts` (every camera part, its bus,
   pins and supplies) and `python toolkit/tools/pmic_ldo.py dts live.dts` (the register value of each supply on an
   I2C PMIC). Keep the output with the profile's evidence; `ios-port-media` uses it.

Treat extractor values as **candidates**. Its notes flag the known traps:

- Vendor trees number GCC clocks differently from upstream bindings (seen on the S20+). The extractor resolves
  a bus by its engine address and says when the two disagree.
- The virtual timer INTID is often not 27 on Qualcomm (19 on SM8150). The kernel needs `gic_vtimer=`.
- The Samsung RAM log ring sits at sec_debug_region + 0x200000. Verify it against `/proc/last_kmsg`.
- If a QUP serial engine has `dmas` and its FIFO is disabled at run time, bit-bang the pins.

## 3. Firmware and loader

- Make a Mu-Silicium platform package from the closest upstream device with the same SoC. Feed it your own
  XBL's DXEs and `uefiplat.cfg`. Never use another phone's boot image.
- In MemoryMapLib, reserve every live-DT `no-map` region the stock map leaves out. Otherwise RamManagerDxe
  hands them out as free RAM and the phone hangs (S10 Lite Test 40). The validator's `--dts` check lists them.
- Add a `struct platform` to `xnuloader/loader.c`. The firmware's GOP framebuffer base tells devices apart.

## 4. The test ladder (one hardware run each, each needs the owner's OK)

Follow the S10 Lite's order. Each rung is proven before the next:

1. BootShim colour bands, and the ring log line `BootShim entry` (the shim runs in place of the kernel).
2. UEFI FD from BootShim, then the minimal firmware's dashboard, then the loader's return-path self test, then
   a warm PS_HOLD reset back to Android. Use ABL's own shutdown sequence (set/way flush + SCTLR off), never a
   per-region clean over the firmware map (S10 Lite Tests 15-19 hung on it; Test 31 fixed it).
3. Real XNU to checkpoint 1, then 5 (MMU on), then a timed run with the correct virtual-timer INTID.
4. Full firmware (all Qualcomm DXEs) with the log slot in the recovery padding (its log survives cold resets),
   then USB RAM drive delivery of the system image, then iOS userland (launchd, SpringBoard).
5. Display alignment (skill `ios-port-display`), then touch, battery, buttons and cpufreq
   (skill `ios-port-peripherals`).

Make each test with `toolkit/generator/gen_test.py` (skill `ios-port-next-test`). The template `qcom-userland` covers
rung 4 onwards. Earlier rungs need the matching loader flags in the spec.
