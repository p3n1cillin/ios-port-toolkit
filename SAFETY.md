# Safety rules

These rules held for every hardware test of the Galaxy S10 Lite port (Tests 1-68). The phone always came back
to Android with its original recovery, boot and vbmeta. They apply to every device ported with this toolkit.
`runner/safety.py` checks them in code (`check_bundle`, `assert_shell_safe`, `assert_download_restore`,
`assert_wipe_allowed`). The operator template refuses writes outside recovery. The skill
`skills/ios-port-safety` carries the judgment part for Claude Code sessions.

| # | Rule | Enforced by |
|---|---|---|
| R1 | A test writes **only the recovery partition**. Never boot, vbmeta, dtbo, xbl, abl, misc, param, efs, persist, userdata or super. | `safety.assert_shell_safe` (refuses `of=` anything else, `>` into block devices, fastboot/sgdisk/mkfs); the operator's `Root` guard; `check_bundle` scans the operator |
| R2 | Every run **restores the original recovery automatically** when Android returns, and verifies recovery, boot and vbmeta against the baseline hashes, before and after. | operator `Check`/`Restore`; `run_test.py` / `run_native_test.py` `finally:` block; `check_bundle` (restore image = baseline) |
| R3 | A test is entered with the **reboot-recovery helper** (reboot(2) RESTART2 "recovery"). No misc/BCB write, no fastboot. The operator refuses if misc already has a pending command. | operator `Start`; `assert_shell_safe` refuses `reboot bootloader/download/edl` and boot-control setprops |
| R4 | **No PSCI reset and no PMIC writes** from our code. Resets are the firmware's PS_HOLD. PMICs are read through the SPMI observer channel only. | review rule for kernel/firmware code (skills `ios-port-peripherals`); `assert_shell_safe` refuses regmap/PMIC writes from Android |
| R5 | A **Download Mode restore flashes only the original recovery** (hash-checked). No PIT, no repartition, no other partition. | `safety.assert_download_restore`; skill `ios-port-safety` |
| R6 | **One run per authorization**: the owner's explicit OK for that run, or a standing authorization they gave in words, recorded in PHONE-STATE.json and the run's receipt. One test at a time, never retried or chained automatically. Bundles stay start-guarded otherwise. | bundle `start_blocked_reason` + operator; `run_test.py --authorized-by` (required) |
| R7 | **Wiping Android** (iOS onto userdata, repartitioning) needs a **downloaded stock firmware for the exact model** first **and** the owner's **explicit confirmation** for that step. No toolkit tool wipes. **Unlocking a bootloader wipes the phone on nearly every brand**, so it falls under the same rule: it is the owner's step. | `safety.assert_wipe_allowed` for any future installer; `assess_device.py` says so per brand |
| R1f | **fastboot-boot phones** (`flash.method: fastboot-boot`, BRANDS.md): a test only ever RAM-boots. Allowed: `fastboot boot <image>`, read-only `getvar`, `devices`, a plain `reboot`. Never flash, erase, format, set_active, oem, flashing or update. | `safety.assert_fastboot_safe` (every command `runner/fastboot_boot.py` sends); `assert_profile_safe` refuses a fastboot-boot profile that names a test partition |

## Before a run

1. `python tools/selftest.py` passes (or at least the bundle's own `check_test<N>.sh`).
2. The bundle passes `check_bundle`. `run_test.py` does this before writing anything.
3. The operator's `Check` passes on the phone: model, root, unlocked bootloader, recovery node and size, boot and
   vbmeta hashes, battery.
4. Nobody else is running a hardware test on the phone.
5. You and the owner know the way back: the forced-restart keys, the Download Mode keys, and the original
   recovery image path.

## When the phone does not come back

The runner exits 2. Tell the owner what to do:

1. Force a restart.
2. If the phone loops or hangs, enter Download Mode.
3. Stop any restore watcher.
4. Flash **only** the original recovery and check its hash first.
5. Verify the baseline in Android.
6. Pull `/proc/last_kmsg` raw.
7. Record everything.
8. Put the bundle's start guard back.

## Privacy

Serial numbers, backups, the live DTB/DTS and absolute paths with the user name stay in
`devices/<codename>.local.json` and `work/`. They never go into shared profiles, issues or external services.
