# How the phone's own bootloader turns the MMU and caches off (and what that means for us)

Reverse-engineered 2026-11-16 with REA (github.com/morluto/rea) driving Ghidra 12.2 on the stock ABL
`LinuxLoader` module (`research/fw/abl-linuxloader.efi`, PE32+ ARM64 debug build, sha256 `208dfb3e…`,
extracted from the stock BL package `BL_G770FXXS9HXA1`). REA works on this module because it is a PE image;
it does not accept the hypervisor's ELF on Windows. Raw REA evidence is in `research/abl-decomp/`.

ABL is the best reference we have: it runs at EL1 under the same Samsung uH hypervisor, on the same
SoC, and turns the MMU and caches off **on every single Android boot without fail**.

## The sequence (decompiled, exact order)

`BootLinux` (FUN_000206c8) does, after loading the kernel:

1. Shut down the "Bksecapp" TrustZone app.
2. Print "Shutting Down UEFI Boot Services" and call `ExitBootServices` (FUN_0001e4a8).
3. **`PreparePlatformHardware` (FUN_0001e5f8)** — the nine calls below.
4. Record a boot-stats timestamp, then jump to the kernel. It never comes back.

`PreparePlatformHardware`:

| # | Call | What it does |
|---|---|---|
| 1 | FUN_0003d2d4 `ArmDisableBranchPrediction` | empty on AArch64 |
| 2 | FUN_0003ce00 `ArmDisableInterrupts` | mask IRQ |
| 3 | FUN_0003cdb8 `ArmDisableAsynchronousExceptions` | mask SError |
| 4 | FUN_0004db98 `WriteBackInvalidateDataCache` → FUN_0003ccf8 `ArmCleanInvalidateDataCache` | **`dc cisw` over every set and way of every data-cache level up to LoC** (walker FUN_0003cc9c, per-line op at 0x3ce70) |
| 5 | FUN_0004db68 `InvalidateInstructionCache` | `ic iallu` |
| 6 | FUN_0003d0c0 `ArmDisableDataCache` | clear SCTLR.C alone, dsb, isb |
| 7 | FUN_0003d1a0 `ArmDisableInstructionCache` | clear SCTLR.I alone, dsb, isb |
| 8 | FUN_0003cf28 `ArmDisableMmu` | clear SCTLR.M, `tlbi vmalle1`, dsb, isb |
| 9 | FUN_0003d738 `ArmInvalidateTlb` | `tlbi vmalle1`, dsb, isb |

## How ours differed

| | Phone's ABL (works every boot) | Our loader (Tests 15/16/18 hung) |
|---|---|---|
| Data-cache flush | whole hierarchy, by set/way, clean + **invalidate** | selected address ranges (firmware map, stack, kernel image), mostly clean only |
| Cache state at MMU-off | empty | dirty lines possible outside the ranges; clean lines left everywhere |
| SCTLR | C, then I, then M, each its own write | M, C and I cleared in one write |
| Exit boot services | yes | no (UEFI must stay resident so we can return) |
| Comes back | never | yes, through `loader_return_stub` |

The ranges we flush depend on the firmware memory map describing every region we rely on, including
the loader's own saved context (see `r5q-handoff-rca.md`). The ABL method has no such dependency, and an
empty cache also removes the return-side risk: when our stub turns the MMU back on, no stale clean line
can shadow anything written while it was off (the RB0 tag, or a kernel's log).

## Test 30: do what ABL does

`loader.c` `R5Q_ABL_SEQUENCE` replaces `jump_to_kernel`'s cache/MMU switch-off with steps 2-9 above, in
assembly, with no memory writes between the flush and the cache switch-off. Test 30 = Test 18 + that flag.

Checks: 11 shim + 4 roundtrip Unicorn cases pass. Under the QEMU EL2 stand-in for uH, with QEMU's real
Cortex-A76 cache geometry (so the set/way loop runs over real CLIDR/CCSIDR values), the return-path
self-test and kernel checkpoints 5 and 11 pass in all 12 configurations, and the trapped SCTLR writes show
ABL's exact C → I → M order. Tests 16, 18, 24 and 29 rebuild byte-identical. Real cache behaviour is unproven
until it runs on the phone.

## What stays different on purpose

We do not call `ExitBootServices`: the loader has to return into UEFI, which then resets the phone through
its own PS_HOLD path. ABL's sequence is otherwise followed exactly.
