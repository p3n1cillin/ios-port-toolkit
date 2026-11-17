# iOS on the r5q virtual twin (EMULATION ONLY)

The twin (`work/ios-r5q/twin`) can now boot the whole iOS stack the way the phone would run it:
**production r5q loader → source-built XNU → iOS 16.5 userland**. It runs on the phone's memory map, behind
the uH hypervisor stand-in, with the r5q GIC-500 at the phone's addresses and the phone's 1080×2400
framebuffer. Nothing here touches the phone or is approved for it.

```
cd work/ios-r5q/twin                      # in WSL
python3 run_twin.py ios 660               # boot for 660 s; results in port/build/twin-ios/<loader>/
TWIN_LOADER=../port/build/twin-ios/loader-twin-phone.efi python3 run_twin.py ios 300   # the phone-identical loader
```

Outputs: `serial.log` (twin events + loader console + kernel console), `screen-<N>s.png` (the r5q framebuffer
at N seconds), `ring.txt` (this boot's part of the Samsung RAM log, i.e. what `last_kmsg` shows after the reset),
and `receipt.json` (milestones with times, every r5q hardware page touched, last lines).

## What iOS mode models, and how

| Piece | iOS mode | Why it matches the phone |
|---|---|---|
| Hypervisor | HCR_EL2 = RW\|VM\|PTW\|TVM; the first TVM trap **clears TVM and re-executes**, nothing more | exactly uH's handler `FUN_8576f6a8` before RKP_START (`r5q-uh-tvm-decompile.md`) |
| DRAM | guest RAM to 0x280000000; the firmware stand-in reports Conv 0xC0000000-0xC1400000 and 0xC1C00000-0x280000000 | live DT `memory` banks (0x80000000+0x3bb00000, 0xC0000000-0x280000000) |
| Fixed regions | Kernel 0x80000000 reserved, loader at 0x9B4DC000, FD BsData, Display Reserved, SEC Debug | production `r5qPkg/MemoryMapLib.c` (the payload is not reserved there: the loader steps around it) |
| Kernel window | the loader picks **0xF4000000, 6 GiB** (largest free block, clamped), resident image at 0xFC000000 | the same arithmetic the phone's loader will do |
| GIC | stage-2 level-3 pages put QEMU's GICv3 at **0x17A00000 (GICD) / 0x17A60000 (GICR)**; the stand-in enables it as Mu's ArmGicV3Dxe would | r5q MemoryMapLib + live DT |
| CPU | MIDR 0x51df805e (Kryo 485 Gold) via VPIDR_EL2 | Test 17 phone log |
| Framebuffer | GOP 1080×2400 at 0x9C400000 → the loader selects **platform r5q** | phone's Display Reserved |
| BootShim | skipped: the runner preloads the payload at 0xF0000000 | the shim's copy is hardware-verified (Test 17) |
| Everything else | traps to EL2, read as zero, **each distinct page logged** | — |

Two loader builds, both production profile (not the R5Q_XNU_TEST lab profile):

- `loader-twin-ios.efi` (`build_loader_linux.py --twin-ios`): production + `R5Q_TWIN` (r5q platform gets the
  twin's PL011 at 0x09000000, so the kernel's console is on serial). Only twin-only difference.
- `loader-twin-phone.efi` (`--twin-phone`): what an r5q iOS attempt would actually carry: no UART, every
  loader line mirrored into the Samsung RAM ring (`R5Q_LAB_TRACE` + `R5Q_COHERENT_TRACE`), and Test 30's
  ABL cache/MMU shutdown (`R5Q_ABL_SEQUENCE`). After the kernel returns, the loader prints the kernel's whole
  log, so it lands in the ring too and survives the reset.

## Results (2026-11-16)

| Run | Loader | Outcome |
|---|---|---|
| `ios 660` | loader-twin-ios.efi | **iOS 16.5 home screen** (`systemShellDidFinishLaunching` at 223 s wall); kernel returned at 660 s with a 561,826-byte log; loader rebooted via PS_HOLD. No panic. `port/build/twin-ios/loader-twin-ios/screen-300s.png` |
| `ios 300` | loader-twin-phone.efi | **home screen** again; the whole 466 KB boot (loader lines + the returned kernel log, including `systemShellDidFinishLaunching`) is in the **Samsung RAM ring**, i.e. it would read back as `last_kmsg` after the reset |

Kernel console highlights: `platform r5q`, `display 1080 x 2400 at 9c400000`, `MIDR=51df805e`,
`phys window f4000000 size 180000000`, `resident image at pa fc000000 size 948c0000: it is the root (md0)`,
`BSD root: md0`, `MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1080x2400`.

## What this teaches about the phone

1. **Beyond the hand-off, iOS needs almost no r5q hardware.** In the whole boot to the home screen, the
   only r5q device page touched (other than the GIC) was PS_HOLD, by the loader's final reset. The kernel
   needs the GIC-500, the architected timer, the framebuffer the UEFI already scans out, and RAM. So once
   the hand-off stops hanging (Tests 19-31), the first phone attempt should not stall on a missing driver.
2. **The r5q addresses, memory window and CPU identity are consistent.** The kernel takes GICD/GICR at
   0x17A00000/0x17A60000, runs with the Kryo 485 Gold MIDR (implementer 0x51), and uses the window the
   production loader derives from the phone's DRAM banks: 0xF4000000 + 6 GiB, resident image at 0xFC000000.
3. **uH as decompiled does not get in the way.** With HCR_EL2 = RW|VM|PTW|TVM and the real handler's
   behaviour (the first TVM trap clears TVM), the loader, the MMU-off hand-off with the ABL sequence, and
   XNU's own MMU/TTBR traffic all run.
4. **There is a log route without a serial port.** The phone-identical build mirrors every loader line into
   the Samsung ring and prints the kernel's log after `loader_return_after`. On the twin the full boot
   log survives the PS_HOLD reset in the ring (466 KB of 2 MiB). On the phone that is `last_kmsg`, the same
   path Test 17 proved. Caveat: the loader also prints that log on the UEFI text console, which may be slow
   on the phone's GOP console. Consider `screen_quiet` for the kernel-log replay.
5. **Phone risk found while building this: the window overlaps two Linux-side reservations.** In the live DT,
   `qseecom_ta_region` (0xFF000000-0x100000000) and `sec_debug_rdx_bootdev` (0x100000000-0x10AD00000) both lie inside
   0xF4000000-0x274000000, under the resident system image (0xFC000000-0x1908C0000). Neither is `no-map`; both
   are Linux allocations (TrustZone app load buffer; Samsung RAM-dump boot device), so they are probably
   free at UEFI time. But if TZ/XPU or uH protects either, writing the system image there would fault on
   the phone. Cheap mitigation: a `physbase`/window above 0x10AD00000, or a resident offset past them.

## With the TrustZone layer: secured GIC, PSCI at EL3, multiple cores (2026-11-16)

The twin now has an EL3 TrustZone stand-in under the uH stand-in (twin README, "TrustZone layer"): GIC security
on, all interrupts Group 1 NS as TZ leaves them, redistributors woken by EL3, PSCI answered at EL3, secondary
cores held in a boot-ROM pen until CPU_ON.

| Run | Outcome |
|---|---|
| phone tests 17-31, `TWIN_SECURE=1` | every one unchanged (resets/returns/watchdog bite exactly as before) |
| `ios 330`, 1 core | **home screen at 211 s**; kernel: `gic: v3, distributor …, cpu 0's redistributor …, virtual timer on PPI 27`; no panic |
| `ios 330`, `TWIN_SMP=4`, `cpus=4` | loader: `cores for the kernel … 0 1 2 3`; XNU starts cores 1-3 by **PSCI CPU_ON over SMC** at EL3; `hw.ncpu 4, hw.activecpu 4`; **home screen at 249 s**; at the return the kernel stops its cores by PSCI CPU_OFF; no panic |

What it adds for the phone:

6. **XNU's GIC driver works with the non-secure view of a TrustZone-owned GIC.** It needs nothing from the
   Secure-only registers (group configuration, GICR_WAKER). On the phone those are Qualcomm TZ's job, and
   the kernel did not depend on writing them.
7. **The SMP bring-up path is the phone's**: the loader (r5q profile, `psci-method = smc`) probes cores with
   AFFINITY_INFO, and XNU starts them with CPU_ON by SMC straight to EL3. uH does not trap SMC (HCR_EL2.TSC
   clear), as decompiled. The difference on the phone: the core numbering is Aff1 (MPIDR 0x100, 0x200, …),
   which the phone loader already uses. The twin loader uses QEMU's Aff0 numbering.
8. **SYSTEM_RESET was not used** on any path (the loader resets by PS_HOLD), consistent with the rule that
   our code must not reset via PSCI on the phone.

## Not modelled (so a twin pass is necessary, not sufficient)

- CPU caches (see the cache model in the twin README), the 19.2 MHz timer (62.5 MHz here), the real Qualcomm
  UEFI (stand-in), and the real TZ/uH code (only their observable behaviour: what TZ leaves in the GIC, PSCI,
  uH's HCR/TVM). Qualcomm-specific SMCs (SCM) the phone's TZ would answer are logged as unsupported; none
  occurred on the iOS path.
