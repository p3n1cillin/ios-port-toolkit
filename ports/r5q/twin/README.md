# r5q virtual twin — a QEMU stand-in for the S10 Lite's boot path (EMULATION ONLY)

A "virtual S10 Lite" for everything from the bootloader's hand-off onwards, built from reverse engineering
and the phone's own logs, so tests can be run, debugged and learned from without the phone. It runs the
**exact images the phone gets** (`port/build/r5q-test<N>-kernel.bin`: our BootShim + the r5q UEFI FD + the
payload with our loader) — nothing is rebuilt for the twin.

```
cd work/ios-r5q/twin            # in WSL
python3 run_twin.py 17 18 29 30 # any test numbers; results in port/build/twin/test<N>/
TWIN_REAL_FD=1 python3 run_twin.py 17   # run the REAL r5q UEFI instead of the stand-in (stops in DXE, see below)
TWIN_FRESH_RING=1 ...                   # start from an empty RAM log instead of the phone's
```

Each run writes `serial.log` (twin events + loader console), `ring.txt` (this boot's Samsung RAM log, as
`last_kmsg` would show it), `screen.png` (the framebuffer) and `receipt.json` (outcome + the phone runner's
ordered marker check).

## What it reproduces, and where each piece comes from

| Piece | Twin | Source |
|---|---|---|
| Hypervisor | EL2 stand-in: HCR_EL2 = RW\|VM\|PTW\|TVM, no DC; TVM writes passed through | decompiled stock `hyp.mbn` (`research/r5q-uh-tvm-decompile.md`) |
| CPU identity | MIDR 0x51df805e (Kryo 485 Gold) via VPIDR_EL2 | Test 17 phone log |
| ABL hand-off | EL1, MMU/caches off, SCTLR_EL1 0x00c00808, image at 0x80100000 | Test 17 phone log |
| Memory map | DRAM, framebuffer 0x9C400000 1080×2400, RAM log ring 0xC1600000, FD 0x9FC00000, payload 0xF0000000, reserved window 0xE0000000 | device tree, loader/shim sources, phone logs |
| RAM log ring | the phone's **actual** ring contents and wrapped index (1603 wraps) | Test 17 `last_kmsg.bin` |
| PS_HOLD 0x0C264000 | write = reset (run ends, ring kept) | Test 12 firmware source |
| APSS watchdog 0x17C10000 | register model; the runner enforces the bite | Samsung `watchdog_v2.c` |
| Display (MDSS) | the one pipe the shim probes for the framebuffer | BootShim source |
| Other hardware | trapped, logged, reads as zero | — |
| r5q UEFI firmware | a stand-in with exactly the UEFI services our loader uses and the firmware's own ring lines; or the real FD (`TWIN_REAL_FD=1`) | Unicorn harnesses, Test 17 phone log |

## Fidelity evidence

- **Test 17 (the last test that ran on the phone):** 25 of the twin's 26 log lines are byte-identical to
  the phone's; the one difference is the timer frequency (kept at QEMU's real 62.5 MHz on purpose, so
  every counter-timed wait stays correct).
- **Real firmware, real RAM:** with the phone's ring preloaded and `TWIN_REAL_FD=1`, the real r5q Mu UEFI's
  SEC stage writes its `[LABSEC]` lines at **ring offset 2002755 with identical bytes — exactly where the
  phone wrote them** (both reproduce the known SEC writer's 16-bytes-per-wrap drift).
- **Screen:** the BootShim's green / blue / white progress bands appear in the framebuffer — the
  green/blue the phone shows during tests.

## What it has already shown

- Every test 17-30 behaves exactly as designed on the twin: each stage test stops at its tag and resets,
  Test 24's watchdog bites, 25-28 match 20-23, and the full returns (18, 23, 28, 29, 30) complete.
- **Test 18 completes on the twin but hung on the phone.** The twin reproduces the hypervisor
  configuration, memory map, MMU, shim, loader and reset path, so the hang lies in what it does not model:
  CPU caches, the real Qualcomm UEFI's state, or Qualcomm hardware. That supports the cache hypothesis
  behind Tests 29 and 30.
- A stand-in gap found and fixed on the way: the loader needs FP/SIMD enabled (CPACR_EL1.FPEN) because it
  saves FPCR/FPSR — the real UEFI does this.

## Limits (be careful what you conclude)

- **No CPU cache model.** QEMU memory is always coherent, so the twin cannot show the suspected cache bug;
  passing on the twin is necessary, not sufficient.
- **The real r5q UEFI stops in DXE** at `SMEM Target info addr=0x00000000 memory mapping failed`: the
  shared memory that XBL fills in on the phone is not emulated. The full test path therefore uses the
  firmware stand-in after SEC.
- The real uH, TrustZone, PMIC and Qualcomm peripherals are not run; only their observable behaviour that
  our code depends on is modelled.

## Next fidelity steps

1. Emulate SMEM (header + the items the r5q DXE drivers read) so the real firmware gets further — ideally
   to the point of starting our loader itself.
2. For the cache question specifically: a cache-modelling simulator (e.g. gem5 full-system with EL2) could
   reproduce dirty/stale-line effects across the MMU-off round trip, which QEMU cannot.

## Cache model (`TWIN_CACHE=1`, capacity `TWIN_CACHE_PAGES`, default 640 x 4 KiB)

An approximate write-back data cache in the EL2 layer: stage-2 write tracking per 4 KiB page over the
phone's DRAM range, a RAM view per tracked page, trapped DC CVAC/CIVAC/IVAC (HCR_EL2.TPC) and set/way
(TSW), FIFO capacity eviction, uncached accesses seeing only the RAM view, dirty data reappearing when
caches come back on and lost at a reset. The RAM log ring and framebuffer are excluded, matching the real
firmware's uncached / write-through mappings. `TWIN_MAP=noloader` makes the stand-in leave the loader image
out of the memory map, to test that dependence. Not modelled: stale clean lines; per-line write tracking.

It reproduces the suspected hand-off mechanism: with the loader image left out of the map, Test 18's return
stub restores `TTBR0_EL1 = 0` from a saved context that never reached RAM, and hangs; Tests 29, 30 and 31
all return. With the real firmware's map, everything returns. See `research/r5q-handoff-rca.md`.

## iOS mode (`run_twin.py ios SECONDS`)

Boots the whole stack the way the phone would run it: production r5q loader, source-built XNU
(`port/build/xnu-source-rebuild.macho`), and iOS 16.5 (`root.apfs` in the payload, `system.apfs` preloaded at the
loader's resident address 0xFC000000 from the donor's sparse `qemu/ram.img`, mapped private and never written).
Built with `-DIOS_MODE`, linked at 0x40200000 (QEMU RAM below the phone's DRAM, above QEMU's DTB).
Differences from the test mode: RAM to 0x280000000 (`-m 9G`, 36-bit stage 2); the r5q GIC-500 at 0x17A00000/0x17A60000,
aliased by level-3 stage-2 pages onto QEMU's GICv3 and enabled by the stand-in the way Mu's ArmGicV3Dxe leaves it;
uH's real TVM behaviour (first trap clears TVM and re-executes); a production-shaped UEFI memory map (live DT
DRAM banks as conventional memory); AllocateAddress, Stall, ResetSystem; the BootShim skipped (the runner preloads
the payload); each distinct trapped hardware page logged. `TWIN_LOADER` picks the loader:
`loader-twin-ios.efi` (`--twin-ios`: production + the twin's UART) or `loader-twin-phone.efi` (`--twin-phone`: no
UART, loader output and the returned kernel log mirrored into the Samsung RAM ring, ABL shutdown sequence).
Results go to `port/build/twin-ios/<loader>/`. Findings: `research/r5q-twin-ios-boot.md`.

Not modelled in iOS mode: the 19.2 MHz timer (62.5 MHz here), real caches, the phone's real TZ/uH code (only
their observable behaviour; see the TrustZone layer below for the GIC security and PSCI).

## TrustZone layer (`TWIN_SECURE=1`; on by default in iOS mode)

The phone's boot chain is TrustZone (EL3) → uH (EL2) → firmware/loader/kernel (EL1). With `TWIN_SECURE=1` the twin
runs QEMU `virt,secure=on` with its own boot ROM (`pen.S`, given as `-bios`, so QEMU has no PSCI of its own) and
starts at EL3, where it does what Qualcomm's TZ does for the non-secure world before dropping to the uH stand-in:

- **GIC security on (DS=0)**: ARE_S|ARE_NS, Group 0 + Group 1 S + Group 1 NS enabled; every SPI, SGI and PPI in
  Group 1 NS except the secure timer PPI 29; each core's redistributor woken by EL3 (GICR_WAKER is Secure-only now).
  EL1 sees only the non-secure GIC view, as on the phone (GICD_TYPER.SecurityExtn = 1).
- **PSCI at EL3** (SMCs from EL1 go straight there, HCR_EL2.TSC clear; the EL2 HVC forward lands there too):
  VERSION 1.1, FEATURES, CPU_ON, AFFINITY_INFO, CPU_OFF, CPU_SUSPEND (standby), MIGRATE_INFO_TYPE, SYSTEM_OFF,
  SYSTEM_RESET (flagged: our phone code must use PS_HOLD instead). Other SMCs answer NOT_SUPPORTED and are logged.
- **Secondary cores**: wait in the boot ROM pen; CPU_ON releases one into EL3 (its GICR) → EL2 (same stage 2 / HCR)
  → the kernel's entry at EL1 with the MMU off, context id in x0.

`TWIN_SMP=N` gives QEMU N cores (iOS mode). QEMU numbers cores in Aff0 while the phone uses Aff1, so the twin
loader (`R5Q_TWIN`) uses `core_shift = 0` (and QEMU's lock-timeout scale); the phone loader keeps Aff1.
Every phone test 17-31 gives the same result with `TWIN_SECURE=1` as without.
`make_timeline.py <run dir>` turns a run's frames (`TWIN_SHOT_EVERY=10`) into `boot.gif` and `timeline.png`.

## Driving iOS (`TWIN_INPUT=1`, `twin_ctl.py`)

`TWIN_INPUT=1 TWIN_SMP=4 TWIN_CMDLINE=cpus=4 python3 run_twin.py ios 2700 &` adds a virtual touch panel and
buttons (QEMU virtio-tablet + virtio-keyboard; iOS mode passes QEMU's virtio-mmio block 0x0A000000 through
stage 2; the r5q map has nothing there; the donor kernel polls it with `virtio_input=1`) and a QMP socket. Then
`python3 twin_ctl.py tap X Y | swipe X0 Y0 X1 Y1 [MS] | key home|lock|volup|voldown | shot NAME | wait S | log PAT`
(positions are fractions of the screen). Screenshots go to `<run out>/drive/`. `make_contact_sheet.py` builds an
overview. A 26-minute session is written up in `research/r5q-twin-ios-drive-session.md`.
