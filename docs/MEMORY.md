# Memory: where it goes on a port, and getting it back

On the S10 Lite (6 GB) iOS ran at 13% memory free, and jetsam killed daemons. Two causes, both likely on any port
that boots iOS from a RAM disk. Full record: `ports/r5q/wifi/MEMORY-PLAN.md` (Tests 393-394, 2026-11-20).

## Seeing it

- `memorystatus_level` (sysctl `kern.memorystatus_level`): the percent jetsam thinks is free. Watch this, not
  "free pages".
- **Wired memory by owner.** `mach_memory_info()` (what `zprint` uses) needs the host privileged port and
  `com.apple.private.memoryinfo`, which a port's own tools don't have. So the kernel answers the same records
  itself: a root-only sysctl that calls `vm_page_diagnose_estimate()` and `vm_page_diagnose()` and copies out the
  `mach_memory_info_t` array (r5q: `kern.redwood.wired` in `bsd/dev/redwood_ufs.c`, read by `wiredq`).
  - The totals tell the story. "reserved (wired at boot)" is memory that never became usable pages.
  - Everything below the loader's `topOfKernelData` counts there: kernel, device tree, **RAM disks**.
- **A process by memory kind.** On this kind of port `task_for_pid` may return KERN_SUCCESS with a port that
  answers nothing. `proc_pidinfo(pid, PROC_PIDREGIONINFO, addr, ...)` as root works without one: it gives each
  region's tag, resident and dirty pages (`vmq`).
  - A bogus `phys_footprint` (tens of GB) with a sane resident size means huge untouched malloc regions.
- **Who allocates.** Inject a dylib that sets libmalloc's `malloc_logger` and libsystem_kernel's
  `__syscall_logger`. They are the hooks MallocStackLogging uses; libmalloc calls them for every allocation, VM
  ones included. Print a backtrace (`backtrace` + `dladdr`, with image offsets so `<redacted>` frames can be
  found in the shared cache) for big sizes (`libr5q-bigmalloc.dylib`). Wrapping a zone's function pointers missed
  them.

## The system image in RAM

A RAM-disk boot keeps the whole iOS system volume (2.3 GB here) wired for the whole run. If the phone's storage
holds the same image and the kernel can read it, give the RAM back after boot instead of rebooting from storage
(`bsd/dev/memdev.c` in the r5q overlay):

1. Boot from the RAM disk as before (`rd=md0`): nothing in userland changes.
2. When the storage driver is up, compare pieces of storage and RAM: the first and last MB and 256 random 16 KB.
   Refuse if anything differs.
3. Under the md device's rw-lock (readers in the strategy routine hold it shared while they copy from RAM), mark
   the disk as read-from-storage. Its strategy routine then reads storage at the same offset.
4. `ml_static_mfree(base, size)`: the RAM disk lives in the kernel's static region (`ml_static_ptovirt`), and the
   kernel frees its own boot-only segments the same way. The pages go to the VM's free list, and
   `vm_page_wire_count_initial` drops.
5. Make `mdevgetkernelrange()` refuse that disk, so drivers that read firmware in place from a RAM disk skip it.
   Their files must be on another RAM disk (r5q: md1).

r5q numbers: 16 KB random reads 405 µs from flash vs 5 µs from RAM, 264 MB/s sequential. Memory free went from 27%
to 65% (wired 3.7 → 1.3 GB). Apps, a /var save to the same flash, and the Camera all kept working. Measure first:
`blkbench` compares two devices at the same places and times both.
