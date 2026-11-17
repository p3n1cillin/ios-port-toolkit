# Samsung Galaxy S10 Lite project

The authoritative project is `C:/Users/user/source/repos/WIP/S10-Lite-project`.

**Start here when you're home: [HOME-CHECKLIST.md](HOME-CHECKLIST.md)** — how to recover the phone and the
order of the next tests.

## Phone state (2026-11-16 14:00)

The rooted test phone is **SM-G770F / r5q, <SERIAL>**. It is **stuck in Test 18**: the test did not return
to Android and needs a physical Volume Down + Side reset. Only the recovery partition holds a test image;
Android boot and VBMeta were never written. A restore watcher (`restore_native_on_return.py`, restarted detached at 13:59 via
`work/ios-r5q/start_test18_watcher.cmd`, gives up about 01:59 next day) will put the original recovery back and verify all three hashes as soon as Android with
root is back. If the phone lands in Download Mode instead, flash only the original recovery (as after Test 15).

[PHONE-STATE.json](work/ios-r5q/PHONE-STATE.json) and [SESSION.md](work/ios-r5q/SESSION.md) hold the current
state and detailed continuity notes. No native iOS has run on the phone yet.

## Verified progress

- Tests 5 and 9 verified the native EFI diagnostic loader; Test 9 passed twice with the full unused XNU payload.
- Test 11 verified a reserved RAM mapping without exposing it as a general firmware heap.
- Test 13 verified actual XNU segment preparation, return-slot patching, Apple-format DeviceTree/boot_args
  construction, clean EFI return and recovery restoration. It did not execute XNU.
- **Test 17 passed on the phone** (09:39): the corrected RAM logger works, the phone returned to Android by
  itself and the original recovery was restored and verified.
- Tests 15, 16 and 18 did not return. Every test that passed avoided the MMU-off → return-stub → MMU-on round
  trip, and every test that hung performed it; see the root-cause analysis below.
- **The XNU kernel now builds from source** in a no-network sandbox (`work/ios-r5q/build_xnu_linux.py`), and
  with the iOS 16.5 system image it **boots to the iOS home screen in QEMU** on the PC
  (`work/ios-r5q/port/build/qemu-boot/ios-source-portloader-400s.png`). That is emulation, not the phone.
- The phone's own hypervisor (Samsung uH) was decompiled with Ghidra and **ruled out** as the cause of the
  hang; a QEMU EL2 model agrees. The likely cause is a cache-coherency gap in the saved CPU context.

## Virtual twin of the phone

[work/ios-r5q/twin](work/ios-r5q/twin/README.md) is a QEMU "virtual S10 Lite" for the boot path from the
bootloader onwards. It runs the exact phone test images, under a model of the phone's hypervisor, memory
map, RAM log (seeded with the phone's real one), reset and watchdog. It reproduces the phone's Test 17 log
almost line for line and the real firmware's first log writes byte for byte. All Tests 17-30 behave as
designed on it; Test 18 completes there but hung on the phone, which points at what the twin cannot model:
CPU caches. Run: `python3 work/ios-r5q/twin/run_twin.py 18 29 30` in WSL.

**The twin boots iOS.** `python3 work/ios-r5q/twin/run_twin.py ios 660` runs the production r5q loader, the
source-built XNU and the iOS 16.5 userland on the phone's memory map and GIC addresses, behind the
hypervisor model. It reaches the **iOS home screen at 1080×2400 in about 4 minutes**. Apart from the GIC,
the only r5q hardware touched is PS_HOLD for the final reset. Details:
[research/r5q-twin-ios-boot.md](work/ios-r5q/research/r5q-twin-ios-boot.md). Emulation only.

## Built, checked in emulation, not yet run (each needs an explicit OK)

| Test | Purpose |
|---|---|
| 19–23 | Step-by-step bisection of the hand-off, each ending in a self-reset |
| 24 | Probe: can the phone's hardware watchdog reboot it by itself? |
| 25–28 | Tests 20–23 with that watchdog armed, so a hang reboots the phone |
| 29 | **Candidate fix 1**: Test 18 plus an explicit flush of the saved CPU context |
| 30 | **Candidate fix 2**: Test 18 using the phone's own bootloader's cache/MMU shutdown sequence (reverse-engineered) |
| 31 | **Fixed the hand-off on the phone (PASSED)**: no per-region cache-clean loop; ABL-style flush |
| 32 | **PASSED**: the real kernel ran on the phone to checkpoint 1 and returned |
| 33 | Real kernel to checkpoint 5, just after its own MMU/cache enable (built, guarded) |

Bundles are in `outputs/S10-Lite-native-iOS-port/test19` … `test33`; `work/ios-r5q/run_stage_test_auto.ps1
-Number N` runs one with automatic log capture and restore.

## Key documents

| Document | What it covers |
|---|---|
| [HOME-CHECKLIST.md](HOME-CHECKLIST.md) | Phone recovery steps and test order |
| [work/ios-r5q/SESSION.md](work/ios-r5q/SESSION.md) | Full chronological continuity log |
| [research/r5q-handoff-rca.md](work/ios-r5q/research/r5q-handoff-rca.md) | Root cause of the hand-off hang, per-test predictions, Test 29 |
| [research/r5q-uh-tvm-decompile.md](work/ios-r5q/research/r5q-uh-tvm-decompile.md) | Decompiled uH trap handler; why the hypervisor is not the cause |
| [research/r5q-abl-shutdown-sequence.md](work/ios-r5q/research/r5q-abl-shutdown-sequence.md) | How the stock bootloader switches the MMU/caches off; basis of Test 30 |
| [research/r5q-hypervisor-watchdog.md](work/ios-r5q/research/r5q-hypervisor-watchdog.md) | uH/RKP facts, debug level, download mode, watchdog layout |
| [research/r5q-porting-gap.md](work/ios-r5q/research/r5q-porting-gap.md) | What an S10 Lite kernel port still needs |

## Folder guide

| Folder | Contents |
|---|---|
| [outputs/DualSpace-prototype](outputs/DualSpace-prototype/) | Android privacy prototype, source, APK and module |
| [outputs/S10-Lite-native-iOS-port](outputs/S10-Lite-native-iOS-port/) | Numbered diagnostics, exact images, source snapshots, test evidence and recovery restores |
| [work/ios-r5q](work/ios-r5q/) | Port sources, build/check helpers, private backups, hardware maps and research |
| [work/ios-r5q/research](work/ios-r5q/research/) | Analyses, Samsung kernel subset, stock firmware + disassembly, Ghidra project/scripts, helper scripts |
| [work/ios-r5q/twin](work/ios-r5q/twin/) | Virtual twin of the phone's boot path (QEMU), its runner and README; results in `port/build/twin*/` |
| [work/ios-r5q/el2-model](work/ios-r5q/el2-model/) | QEMU EL2 stand-in for the phone's hypervisor, its runner and built outputs |
| [work/ios-r5q/port/build](work/ios-r5q/port/build/) | Built loaders/kernels, receipts, emulator results, QEMU boot logs and screenshots |
| [work/ios-r5q/donor-additions](work/ios-r5q/donor-additions/) | Scripts generated into the donor tree (QEMU runners with a selectable kernel/loader) |
| [work/privacy-build](work/privacy-build/) | Android toolchain, intermediates and lab signing material |
| [work/s10-relock](work/s10-relock/) | Samsung firmware, flashing tools and earlier relocking work |
| [linux-workspace](linux-workspace/) | Earlier symlink-preserving WSL workspace snapshot |

The separate earlier phone RF8N8182EJV was restored and relocked. Native experiments target <SERIAL> only.

## Linux workspace (outside the project folder by design)

Live sources and toolchains remain under `/var/tmp/codex-ios-r5q/work` in Ubuntu WSL, because the build trees
rely on Linux symlinks. Everything written today that is not regenerable has been copied into this folder
(`work/ios-r5q/collect_outside_work.sh`). What stays only in WSL is large and regenerable:

- `linux-source/ios165/` (~13 GB): the iOS 16.5 files from Apple's servers — `ios165/fetch.sh` (KEEP=1).
- `linux-source/userland/*.apfs` (~2.4 GB): the iOS system and boot images — `userland/build.sh` with `PREBUILT=1`.
- `qemu-kernel-runs/`, `el2-model/run-*` (~3.7 GB): disposable QEMU run directories; their logs are in the project.
- Toolchains, SDKs, Mu-Silicium and the donor tree as they were before today.

Installed tools outside the project: Python 3.13 (winget, user scope), QEMU 10.2.1 + AAVMF in WSL, and
Ghidra 12.2 at `%LOCALAPPDATA%\Programs\Ghidra` (SHA-256 verified).

## Original test-phone baseline

| Partition | SHA-256 |
|---|---|
| Recovery | `332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd` |
| Android boot | `381e6aa72b0fee42a04c4f7be51d0c63bccaaed26e54f9ac30d3de6255e1bc1a` |
| VBMeta | `6e614ba7ab223c23548dc65734358f4786d4f4008107695dd9e8b1494791790c` |
