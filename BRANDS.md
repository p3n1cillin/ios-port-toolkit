# Brands: how a phone enters a test and comes back

The boot chain is the same everywhere: the stock bootloader loads an Android boot image whose kernel is
BootShim, then the UEFI FD, then loader.efi, then XNU. What differs by brand is **how the test image gets in**
and **how you get back**. `brands.json` holds this data, and `tools/assess_device.py --brand <b>` uses it.

> These are general facts as of 2026. **Check your exact model**: unlockability differs by region and carrier
> and changes with firmware. On nearly every brand, **unlocking the bootloader wipes the phone**. That step, and
> the decision to take it, belong to the owner (SAFETY.md R7).

## The two entry methods

| `flash.method` | What a test does | The way back | Brands |
|---|---|---|---|
| `recovery` | Writes the test image to the **recovery** partition (and only there), then reboots into it with the reboot-recovery helper | The runner writes the original recovery back when Android returns, and verifies recovery, boot and vbmeta. If Android does not return: Download Mode, original RECOVERY only | Samsung (no `fastboot boot`) |
| `fastboot-boot` | `fastboot boot <image>`: the image is loaded into **RAM** and started once. **Nothing on the phone is written** | Any reboot is Android again. There is nothing to restore | Xiaomi/POCO/Redmi, Google Pixel, OnePlus, Motorola, Nothing, Sony, Lenovo, Asus, many others |

`fastboot-boot` is the safer of the two. Use it whenever the bootloader supports it.
`runner/fastboot_boot.py` sends the image. `runner/safety.py: assert_fastboot_safe` allows only
`fastboot boot <image>`, read-only `getvar`, `devices` and a plain `reboot`. It refuses flash, erase, format,
set_active, oem, flashing and update, a `boot` with options, and `reboot bootloader/recovery/edl`.

The S10 Lite's recovery flow is `runner/run_test.py`, with `generator/` making its per-test scripts.

## Per brand

| Brand | Method | Unlock | Restore if bricked | Watch out for |
|---|---|---|---|---|
| Samsung | recovery | OEM unlocking + Download Mode prompt (international/Exynos, most non-US Snapdragon); US/Canada carrier Snapdragon generally not | Download Mode (Odin protocol: samloader/Odin/Heimdall) | Knox warranty bit trips for good; images need the AVB footer + `SEANDROIDENFORCE` |
| Xiaomi / POCO / Redmi | fastboot-boot | Mi Unlock tool, account + waiting period | fastboot ROM (MiFlash, never "lock"); EDL needs authorization | anti-rollback: never an older ROM |
| Google Pixel | fastboot-boot | OEM unlocking + `fastboot flashing unlock` | factory images | Pixel 6+ = Tensor (Exynos-derived): no Mu-Silicium UEFI, tier D |
| OnePlus | fastboot-boot | `fastboot oem unlock` / `flashing unlock` | MSM Download Tool (EDL) | many Mu-Silicium ports (kebab, lemonade, aston, salami, ...) |
| Motorola | fastboot-boot | unlock code from Motorola | stock fastboot firmware | carrier models not unlockable |
| Nothing | fastboot-boot | `fastboot flashing unlock` | fastboot images / EDL | Phone (1) = SM7325 like redwood |
| Sony | fastboot-boot | unlock code from Sony | Xperia Companion / newflasher | unlocking erases the DRM keys for good |
| OPPO / realme | fastboot-boot (varies) | deep-testing app on some models only | MSM tool / EDL | many models cannot be unlocked |
| Asus | fastboot-boot | official unlock app where offered | raw firmware | ROG Phone 5 has a Mu-Silicium port |
| Lenovo | fastboot-boot | `fastboot flashing unlock` (tablets) | Rescue and Smart Assistant | tablets have Mu-Silicium ports |
| LG | varies | unlock program ended | LGUP/KDZ | G8x and V35 have Mu-Silicium ports |
| Huawei / Honor | blocked | no official unlock | | not portable |

## Brand-specific image details the toolkit handles

- **Samsung:** an AVB footer with algorithm NONE (`package_test<N>.py`) and the `SEANDROIDENFORCE` trailer.
  The log slot goes in the recovery padding.
- **Header versions:** `tools/bootimg.py` reads and repacks boot images v0-v4 and reads vendor_boot v3/v4.
  It is byte-identical to AOSP mkbootimg. Pixels and most phones from 2020 on are v3/v4: the kernel slot holds
  BootShim, and vendor_boot keeps the DTB.
