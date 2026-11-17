---
name: ios-port-log-triage
description: Read the result of a native iOS port hardware test - runner exit codes, receipt.json, last_kmsg vs the firmware log slot, marker order and anchors, warm vs cold resets, the owner's photos/videos - and map symptoms to the known causes from the S10 Lite's Tests 1-68. Use right after a test run, or when a run hung or showed something unexpected.
---

# Reading a test's result

## Where the evidence is

- Each run writes `outputs/<port>/test<N>/validation/automatic-<time>/`. In it:
  - `receipt.json`: status, marker counts before and after, ordered sequence, anchor, baseline check
  - `last_kmsg.bin`: Samsung's RAM ring of the boot that just ended
  - `log-slot.bin` and `firmware-log.txt`: the full firmware's copy of the ring, saved into the test image's
    padding in recovery and read back before Restore
  - `start.txt` and `restore.txt`: the operator transcripts
- The owner's photo or video of the panel is evidence too. Ask for one whenever the outcome is visual or the
  log may be wiped.

## Runner exit codes

| Exit | Meaning | Next |
|---|---|---|
| 0 | markers confirmed in order, anchor new, baseline verified | record, plan the next test |
| 2 | Android did not return before the timeout | `ios-port-safety`: physical access, Download Mode, restore RECOVERY only |
| 3 | restore or baseline not verified | stop everything; verify the partitions by hand with the owner |
| 4 | Android came back, markers unproven | read the receipt: a missing marker, a wrong order, or an anchor already in the previous log |

## Warm vs cold reset

- A warm PS_HOLD reset (the minimal firmware) keeps the RAM ring, so `last_kmsg` holds the test.
- A cold reset wipes the ring. That happens with the full firmware's PS_HOLD, a forced key reset, or a hang ended
  by the owner. Only the log slot survives, and only if the firmware saved it before the reset.
- A warm-reset loop keeps the ring even through Download Mode (Test 37).

## Known patterns (S10 Lite)

| What you see | It was | Test |
|---|---|---|
| Hang after the loader prepared the kernel, before the MMU-off hand-off | per-region DC clean over the firmware memory map; fixed with ABL's set/way flush + SCTLR sequence | 15-19 → 31 |
| Kernel boots, then sleeps forever at its first timer wait | virtual timer wired to a Qualcomm PPI (INTID 19), XNU default 27 | 34-36 |
| "Spurious GIC interrupt 0x13" storm after the kernel returns | the kernel left its timer PPI enabled, so UEFI got a level storm; save and restore GICR enables, mask CNTV | 49-50 |
| Full firmware: ASSERT UncachedMemoryAllocationLib Out of Resources | Qualcomm DXEs on a 28 MiB DXE heap; add a second heap | 37-39 |
| Hang in RamManagerDxe | live-DT no-map regions missing from the UEFI memory map | 40-41 |
| No trace at all, Android back by itself | cold reset under the full firmware; add the log slot | 41-44 |
| UsbfnIo not found | signal the USB controller init event group first | 45-48 |
| iOS picture shifted | `ios-port-display` | 52-61 |

## Recording

Write a short SESSION.md entry: what ran, what the evidence says (quote the key log lines), the restore and
baseline state, what is concluded, and what comes next. Keep conclusions separate from guesses.
