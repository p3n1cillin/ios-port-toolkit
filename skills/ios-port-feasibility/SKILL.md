---
name: ios-port-feasibility
description: Decide whether (and how) a given Android phone can get the native iOS port - brand unlock and entry method, SoC vendor, whether a Mu-Silicium UEFI exists for its SoC, generic kernel requirements (GICv3, PSCI, timer INTID), and which peripherals the toolkit can resolve. Use when someone names a phone model or brand and asks "can this run iOS natively", or before starting a new device.
---

# Is this phone portable?

Answer from data, and say plainly what is unknown. Never promise that a phone will run iOS.

## 1. Collect what you can, without writes

- The brand and exact model (and region or carrier: unlockability depends on them).
- The SoC part number (SM8250, MT6893, gs101, ...). Without the phone, take it from the spec sheet.
- With the phone, unlocked and rooted: the merged live tree, from
  `adb shell su -c 'cat /sys/firmware/fdt' > live.dtb; dtc -I dtb -O dts -q live.dtb -o live.dts`.
  Base DTBs from boot/vendor_boot images lack the board parts (touch, keys, power key); the extractor warns.

## 2. Run the assessment

`python toolkit/tools/assess_device.py --dts live.dts --brand <brand>`

Without a tree, use `--profile` with a scaffolded profile, or reason from `toolkit/socs/mu-silicium.json`
and `toolkit/brands.json` directly.

| Tier | What to tell the owner |
|---|---|
| A | The method is proven on this SoC (SM8150 S10 Lite, SM8250 S20+, SM7325 POCO X5 Pro). Expect device-specific work (panel, touch chip, memory map), not research. |
| B | Qualcomm with a Mu-Silicium UEFI and full SoC data. The S10 Lite path applies step by step. Months of careful tests, as the S10 Lite took 68 of them. |
| C | A UEFI exists, but MediaTek/Exynos peripherals (clocks, PMIC, I2C, display) need new drivers. A substantial project. |
| D | No UEFI for the SoC (Tensor, most Dimensity). A Mu-Silicium silicon port comes first, and that is its own project. |
| X | Blocked: the bootloader cannot be unlocked (Huawei/Honor, US carrier Samsung Snapdragon), or a GICv2-only SoC. |

## 3. Always mention

- **Unlocking wipes the phone** on nearly every brand, and Samsung's Knox bit trips for good. It is the
  owner's decision and step (SAFETY.md R7).
- **The entry method** (BRANDS.md):
  - Samsung writes recovery and restores it after each test.
  - Others with `fastboot boot` write nothing: RAM only.
- **What runs today on the S10 Lite** (the reference): home screen, touch, 8 cores at full clocks, battery,
  buttons. Not yet: Wi-Fi, installation to storage. Others start from there.
- Apple's iOS image comes from Apple's servers to the owner's machine (the donor's `ios165/fetch.sh`). It is
  never redistributed.

## 4. Next step

If the owner wants to go ahead, use the skill `ios-port-new-device`.
