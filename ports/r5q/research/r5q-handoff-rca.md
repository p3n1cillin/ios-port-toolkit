# Root-cause analysis: the MMU-off hand-off hang (Tests 15/16/18), and a candidate fix

Host-side analysis, 2026-11-16. No hardware was used; the conclusions are predictions to be confirmed
by Tests 19-29 once the phone is recovered.

## The decisive correlation

| Test | Did the MMU-off → return-stub → MMU-on round trip? | Hardware result |
|---|---|---|
| 5, 9, 13 (`R5Q_PREPARE_ONLY`) | **No** — prepared XNU, "CPU/MMU handoff not attempted" | returned to Android, passed |
| 17 (diagnostic + firmware PS_HOLD) | **No** — firmware resets after the app, no stub round trip | returned to Android, passed |
| 15 (real checkpoint 1) | **Yes** — kernel ran, turned its MMU off, branched to the stub | **did not return** |
| 16, 18 (return self-test) | **Yes** — jumped straight to the stub with the MMU off | **did not return** |

Every test that performed the MMU-off → `loader_return_stub` → MMU-on sequence hung; every test that
avoided it returned. The fault is in that sequence, which the stage tests 19-28 bisect and Test 29 tries
to fix. The hypervisor is cleared as the cause (see `r5q-uh-tvm-decompile.md` and `el2-model/`).

## The mechanism: a saved-context coherency gap on the single-core path

`loader_return_stub` (loader.c) runs with the MMU and data cache OFF and reloads the firmware's
translation context from the global `loader_ctx[]` array — TTBR0_EL1, TCR_EL1, MAIR_EL1, VBAR_EL1,
CPACR_EL1, SP, then SCTLR_EL1 to turn the MMU back on. Those reads are **non-cacheable** (MMU off), so
the values must already be at the point of coherency (in RAM), not sitting dirty in this core's cache.

`loader_save_ctx()` writes `loader_ctx[]` with the cache ON. Before the jump, the code cleans:
- the firmware memory-map regions of type 1-6/9/10 (loop in `run_kernel`),
- the current stack (`sp ± 0x2000`),
- the kernel image (`phys_base..top`).

`loader_ctx[]` is a loader global, not on the stack and not in the kernel image. On the **multi-core**
path (`run_on_other_cpu`) it is cleaned explicitly. On the **single-core** path used by the r5q lab
profile (`cpus=1`: `jump_to_kernel` is called directly), it is cleaned **only if** the firmware memory
map reports the loader's own image as a type-1-6 region the loop happens to walk. That is not guaranteed
on the phone, where the loader runs from the Test 12 firmware's FD copy and its runtime memory type is
not something we control. If those `loader_ctx[]` lines are still dirty when the MMU goes off, the stub
reads stale RAM, restores a bad TTBR0/TCR/SCTLR, and the MMU-enable faults or hangs — exactly the
observed symptom, invisible to Unicorn (no cache model) and to QEMU TCG (coherent memory).

This is a hypothesis, not a proof: the map-clean loop *may* cover the image on the phone, in which case
the real culprit is a different coherency effect (e.g. the Qualcomm system cache, or dirty lines the
stub itself leaves). The stage tests settle it.

## Predictions for the stage tests (confirm on hardware)

- **Test 19** (stops before the MMU-off jump): returns. Equivalent to the passing prepare-only tests.
- **Test 20 / 25** (stop just after the MMU/caches go off, at RB0): the first point past the jump. If the
  jump itself or the immediate post-jump state is the problem, this is where it first fails.
- **Test 21 / 26** (after TTBR0/TCR/MAIR reload + TLB/I-cache maintenance, RB2) and
  **Test 22 / 27** (after SCTLR restore = MMU back on, RB3): if the MMU *re-enable* with a stale/odd
  context is the problem, the last tag seen will be RB2 and RB3 will be missing.
- **Test 23 / 28** (full return): should match Test 18 — hang — unless an earlier stage already localized it.
- **Test 29** (Test 18 + explicit `loader_ctx`/stack clean to PoC): if the saved-context gap above is the
  cause, Test 18 hangs and **Test 29 returns**. That A/B pair is the single most informative run.

## Recommended hardware order

1. **19** — baseline, must return.
2. **24** — confirm the APSS watchdog reboots the phone by itself (then 25-28 are self-recovering).
3. **29** — the candidate fix, A/B against the known-hanging Test 18.
4. If 29 still hangs: **20→23** (or **25→28** if 24 worked) to localize the exact failing step.

Each is host-built, emulator-checked and start-guarded; each hardware run needs an explicit OK.

## Addendum: Test 30, the phone's own bootloader sequence

Decompiling stock ABL with REA + Ghidra (`r5q-abl-shutdown-sequence.md`) showed how the phone itself turns
the MMU off at EL1 under uH on every boot: full set/way clean+invalidate of all data-cache levels, I-cache
invalidate, then C, I and M disabled in separate writes, then TLB invalidate. Ours flushed only selected
ranges and cleared M/C/I together. **Test 30** = Test 18 + that exact sequence (`R5Q_ABL_SEQUENCE`). It is a
superset of Test 29's fix: an empty cache can neither lose a dirty line nor return a stale one.

Updated hardware order: **19 → 24 → 29 and 30 (each A/B against the known-hanging 18) → 20-23 / 25-28**.
If 29 returns, the saved-context gap was the cause. If only 30 returns, the cause is broader cache state
(dirty or stale lines outside the flushed ranges). If neither returns, the stage tests localize it.

## Addendum: what the virtual twin's cache model shows, and Test 31

The twin (`work/ios-r5q/twin`) now has an approximate write-back cache model (`TWIN_CACHE=1`): stage-2
write tracking per page, trapped DC CVAC/CIVAC/IVAC (HCR_EL2.TPC) and set/way (TSW), a RAM view per page,
capacity eviction (about 2.5 MiB), dirty data hidden while caches are off and lost at a reset. The stand-in
firmware reports memory with the real firmware's types (Test 12 `MemoryMapLib.c`: RAM log uncached,
framebuffer write-through, payload and window reserved, FD boot-services data).

| Map given to the loader | Test 18 | Test 29 | Test 30 | Test 31 |
|---|---|---|---|---|
| loader image reported as loader code (what the firmware source implies: the loader runs in the "DXE Heap") | returns (only 384 B of unrelated scratch dirty) | returns | returns | returns |
| loader image **not** reported | **hangs: the stub restores TTBR0_EL1 = 0** (the 256-byte saved context at loader+0x7040 never reached RAM) | returns | returns (0 dirty lines) | returns |

So the saved-context mechanism is real and the fixes work against it, but with the firmware as written it
should not trigger, which makes it a less likely explanation of the phone's hang than assumed earlier.

New lead from re-reading which tests passed: **none of the passing tests (5, 9, 13, 17) ever ran the
loader's per-region cache-clean loop** (prepare-only returns before it; the diagnostic loader never reaches
it), and **every hung test did**. If cleaning some firmware-reported region faults or stalls on the phone
(for example a region the hypervisor protects), Tests 29 and 30 would hang too, because both still run it.
**Test 19** separates the two: it runs the loop and resets before the MMU switch.
**Test 31** (= Test 30 + `R5Q_SKIP_MAP_CLEAN`: map fetched and validated, nothing cleaned by address, the
set/way flush covers every line) is the ready fix if Test 19 hangs. It passes all checks and completes on
the twin in every configuration.

Decision tree for the hardware session: **19, then (if it hangs) 31, or (if it returns) 30 then 29; then 24;
then 25-28 or 20-23.**

## Addendum (2026-11-16 evening): resolved on hardware by Tests 19 and 31

- **Test 19** (per-region cache-clean loop over the firmware memory map, then a PS_HOLD reset *before* the MMU
  switch): **hung** (screen: BootShim blue band + UEFI console cursor; RAM log lost to the forced reset).
- **Test 31** (map fetched and validated but **no per-region clean**; ABL-style set/way clean+invalidate and
  ABL's C/I/M shutdown order; full MMU-off round trip): **passed**, with every marker in order, an automatic
  return to Android, and a warm reset.

So the hang in Tests 15/16/18/19 was the loader's per-region `DC` clean over firmware-reported regions, not the MMU
switch or the hypervisor. The likely mechanism (unverified) is a by-VA clean of a region that the firmware
reports as memory but that is not accessible from EL1 non-secure (stage-2 or TZ protected), which stalls instead
of faulting cleanly. The virtual twin did not reproduce this, because its stage 2 maps all of DRAM. The production
loader should use the Test 31 path (`R5Q_SKIP_MAP_CLEAN` + `R5Q_ABL_SEQUENCE`).
