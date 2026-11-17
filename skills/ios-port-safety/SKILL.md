---
name: ios-port-safety
description: Safety rules for native-iOS phone porting work. Use BEFORE any command that touches a phone (adb, fastboot, samloader/Odin, Download Mode, dd to a partition, reboot into a test), before running a test bundle, when a test phone does not come back to Android, and before anything that would wipe or repartition Android.
---

# Phone safety for the native iOS port

Every hardware test on the Galaxy S10 Lite (Tests 1-68) ran under these rules, and none of them ever
cost the phone. They apply to every device. `toolkit/runner/safety.py` enforces them in code; this skill is
the judgment part. Full text: `toolkit/SAFETY.md`.

## The rules

1. **Recovery only.** A test writes only the `recovery` partition. Never boot, vbmeta, dtbo, xbl, abl, misc,
   param, efs, persist, userdata, super. If a step seems to need another partition, stop and ask the owner.
2. **Automatic restore and baseline check.** Every run restores the original recovery when Android comes back
   and verifies recovery, boot and vbmeta against the profile's `baseline.*` hashes before and after.
3. **Enter tests with the reboot-recovery helper** (reboot(2) RESTART2 "recovery"). No BCB/misc writes, no
   fastboot. The operator refuses when misc already holds a pending bootloader command.
4. **No PSCI reset, no PMIC writes.** Resets are the firmware's PS_HOLD. Drivers read PMICs through the SPMI
   observer channel only. Nothing writes a PMIC register, and nothing writes regmap debugfs.
5. **Download Mode restores only the original recovery.** Check its hash against `baseline.recovery_sha256` first.
   Use no repartition, no PIT and no other partitions (samloader: `flash -p RECOVERY recovery-original.img`).
   Stop any restore watcher that is still running before you flash.
6. **One run per authorization.** Each run needs the owner's explicit OK, or a standing authorization the owner
   gave in their own words, recorded in PHONE-STATE.json and in the run's manifest. Run one test at a time.
   Never retry or chain tests automatically. A bundle's `start_blocked_reason` stays set until its run is
   authorized, and goes back on afterwards.
7. **Wiping Android needs two things first:** a downloaded stock firmware for the exact model (the way back), and
   the owner's explicit confirmation for that step. No toolkit tool wipes or repartitions.

## Phones with `fastboot boot` (flash.method: fastboot-boot)

On Xiaomi, Pixel, OnePlus, Motorola, Nothing and similar phones, a test only RAM-boots
(`runner/fastboot_boot.py`), and nothing is written. Only `fastboot boot <image>`, read-only `getvar`,
`devices` and a plain `reboot` are allowed (`safety.assert_fastboot_safe`). Never flash, erase, oem, set_active
or `flashing`. Never `flashing lock`, because relocking with a modified image can brick the phone.

Unlocking the bootloader wipes the phone. It is the owner's step, never ours (R7).

## Before a run (checklist)

- The bundle passes `python toolkit/runner/safety.py`-based checks (`run_test.py` runs `check_bundle` first).
- The operator's `Check` action passes: right model, root, unlocked bootloader, recovery node and size, boot and
  vbmeta hashes, battery at or above the profile minimum.
- Another session is not running hardware tests on the same phone right now. Ask if unsure.
- You know the way back: Download Mode keys for this model and the original recovery image path.

## When Android does not come back

- Exit 2 from the runner means it "needs physical access". Tell the owner what happened and what to do:
  - Force a restart (S10 Lite: hold Volume Down + Side until the screen goes black).
  - If the phone loops or hangs, enter Download Mode (USB id `04E8:685D` on Samsung).
- Then, and only then: stop the watcher, flash ONLY the original recovery, and verify the baseline in Android.
- Pull `/proc/last_kmsg` raw. A forced key reset usually wipes the RAM ring. A warm-reset loop keeps it.
- Record the run in SESSION.md and PHONE-STATE.json. Put the bundle's start guard back.

## Privacy

Serial numbers, live DTB/DTS files, backups and absolute paths that contain the user name stay in
`devices/<codename>.local.json` and `work/`. Never put them in shared profiles, issues or external services.
