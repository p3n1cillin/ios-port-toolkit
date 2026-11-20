# /private/var on the phone's flash (from 2026-11-20)

## Why

iOS showed "iPhone Storage Full", and the Camera's preview paused behind the alert. All of iOS's data lives in
`/private/var`:
- it is a 1.61 GB RAM filesystem (`bsd/miscfs/ramfs`, mounted by init), with about 0.5-0.6 GB free;
- `var_store` packs it into an archive in darwin (two 2 GiB slots at 8-12 GiB) and unpacks it at boot.

The phone has 6 GB of RAM, so the RAM disk cannot grow much. darwin (USERDATA) is 117.49 GB, and everything past
12 GiB is unused.

The kernel cannot write APFS: the donor's driver (`bsd/miscfs/apfs`) reads it, and `apfs_write.c` only overwrites an
existing file's bytes in place. Writing a full APFS would mean a new filesystem's worth of code.

## The plan: HFS+ built into the kernel, on darwin past 16 GiB

- **HFS (Apple's hfs-627.100.6, APSL, the macOS 13 release matching XNU 8796)** is built into the kernel instead of
  loaded as hfs.kext. iPhones used HFS+ until iOS 10.3; it is journaled and writes everything.
- **`/dev/ufs_var` / `/dev/rufs_var`** (kernel `redwood_ufs.c`, `ufs_var=1` with `ufs_write=1`): darwin from 16 GiB
  (block 4194304) to its end, 95,663 MB, read and written.
  - Every byte still goes through `ufs_read()` / `ufs_write()`, so the iOS volume and cryptexes (below block 2042368)
    and everything outside darwin stay unwritable.
  - The boot state (block 2091520) and var_store's slots (8-12 GiB) are below the region and are not touched.
- **The volume is formatted on the PC**: `mkfs.hfsplus -J -s -b 4096 -v Data` (hfsprogs 540, unpacked from its .deb
  into `/var/tmp/codex-ios-r5q/work/hfsprogs`, not installed) on a sparse image of exactly 100,310,953,984 bytes. The
  result is a 93 GB journaled, case-sensitive volume with an 8 MB journal.
  - `storage/mkplace.py` turns it into a placement: 21 records, 202 MB of zero ranges and 60 KB of content, a
    62 KB file (`port/build/var.place`).
  - `hfsplace` (wifi/hfsplace.c) writes it onto `/dev/rufs_var` and reads the content back. Apple's `newfs_hfs`
    needs macOS frameworks.
- **Next:**
  1. Mount it with `hfsmount` (wifi/hfsmount.c; mount(2), no mount_hfs on the phone), then write and read files.
  2. Copy the current `/private/var` across and mount the volume at `/private/var` from init in place of ramfs.
  3. var_store stays as the way back.

## Kernel build notes (bsd/hfs in the overlay)

- `bsd/hfs/core/*.c` (37 files) are listed in the overlay's `bsd/conf/files`: XNU's file with the donor's
  redwood.patch applied, plus these lines, all `standard`.
- **`kext-config.h`**, included first by every file:
  - `CONFIG_PROTECT 0` (no AppleKeyStore; the ramfs had no data protection either), `QUOTA 0`;
  - `HFS_COMPRESSION 1` (the code assumes it in places; `decmpfs` is standard in XNU);
  - `ubc_create_upl` mapped to `ubc_create_upl_external`, and a prototype for `sysctlbyname`, both of which a kext
    sees and a kernel file does not.
- **`hfs_iokit.cpp` is replaced by `hfs_builtin.c`:**
  - `hfs_builtin_start()` calls `vfs_fsadd` and `hfs_init_zones` once, from the end of `redwood_ufs_init()`;
  - the IOKit helpers are stubbed: not ejectable, no media by UUID, no serial number;
  - `hfs_sysctl_register()` is left out. In Test 406 it panicked: "Use sysctl_register_oid_early to register
    permanent nodes" (a kext may add sysctl nodes, code in the kernel image may not; they are tunables with
    defaults).
- **`hfs_encodings`** (the legacy encodings kext) is not built: its header is `!TARGET_OS_IPHONE`, and the core does
  without it on iOS.

## Tests

- **Test 406 (kernel t406, `+ufs_var=1`):** "ufs: var: /dev/ufs_var and /dev/rufs_var: darwin from 16 GiB to its
  end, 95663 MB". Then the sysctl panic above, in HFS's start, so iOS never came up. The loader kept the panic
  buffer (`phone.py logs`).
- **Test 407 (kernel t407, `+ufs_var=1`, sysctl registration left out):** booted, no panic. `/dev/rufs_var` is
  24,489,979 blocks of 4096 = 100,310,953,984 bytes, exactly the image's size.
  - `hfsplace`: "21 records, 15 content blocks, 202 MB written to /dev/rufs_var; read back: all match", in 10 s.
  - First mount (`hfsmount2 /dev/ufs_var /private/var/r5q/hfstest`): ENXIO. The kernel said
    "hfs_mountfs: DKIOCSETBLOCKSIZE failed". HFS switches the device to 512-byte blocks to read the volume header and
    back to 4096, and /dev/ufs_var took 4096 only.
- **Test 408 (kernel t408):** /dev/ufs_var's logical block size can be set (512-4096; DKIOCGETPHYSICALBLOCKSIZE stays
  4096). A write that is not whole 4096-byte blocks is read, patched and written back whole, under a mutex
  (`ufs_var_write`).
- **Test 408 result:** the mount got as far as HFS's first-mount set-up, then panicked:
  "__memmove_chk object size check failed (16 < 32) @subrs.c:725". The path was
  buildrecord ← cat_create ← hfs_privatedir_init ← hfs_MountHFSPlusVolume.
  - HFS copies the 32 bytes of Finder info across two adjacent 16-byte members in one bcopy. It was written for a
    kext, whose checked copies measure the whole object (`__xnu_bos_loose`).
  - Code built into the kernel gets member-strict checks (`osfmk/libsa/string.h`, `XNU_KERNEL_PRIVATE`).
- **Test 409 (kernel t409):** `kext-config.h` sets `_FORTIFY_SOURCE 0` for HFS, so HFS's copies are plain calls.
  - **Mounted:** `hfsmount: /dev/ufs_var on /private/var/r5q/hfstest: hfs, 100.10 GB free of 100.31 GB`. The kernel
    said "jnl: ufs_var: journal start/end pointers reset" and "hfs: mounted Data on device ufs_var".
  - **Crash on the first file write:** writing 50 MB into it panicked when the file was closed: "Kernel data abort",
    far 0, in memmove ← ufs_request ← ufs_write ← ufs_var_strategy ← spec_strategy ← cluster_io ← cluster_push ←
    hfs_fsync ← vn_close.
  - **Cause:** a cluster I/O buffer keeps its pages in a UPL and has no kernel address (`buf_dataptr` is 0), and
    `ufs_var_strategy` used `buf_dataptr` directly.
  - **New tool:** `pipeline/panic_sym.py KERNEL.macho firmware-now.txt` names the pc, lr and backtrace of the last
    panic from the kernel's symbol table.
- **Test 410 (kernel t410):** `ufs_var_strategy` takes the data address from `buf_map()` and calls `buf_unmap()`
  after. For an ordinary buffer `buf_map` gives back the buffer's own pointer.
  - **Result:** booted, no panic. The 50 MB random file was written into the mounted volume, the volume was
    unmounted and mounted again, and the file was pulled back byte for byte (same SHA-256). After the write the
    volume showed "100.05 GB free of 100.31 GB".
- **Test 411 (kernel t410, root test411, `+ufs_var=1 +var_hfs=1`): init mounts the volume at `/private/var`**
  (userland `src/init.c`, `mount_writable`).
  - **Mounting:** with `var_hfs=1`, init mounts hfs from `/dev/ufs_var` at `/private/var`. If that fails, it says
    why and falls back to the memory filesystem.
  - **The skeleton (`etc/var.skel`) on a kept volume:** it only adds what is missing. Directories and files of an
    earlier boot are left as iOS left them, and so are logd's `.migrated` markers. The `/var/jb` link of an earlier
    boot counts as made.
  - **The first move across:** var_store's archive (the memory filesystem's last state) is loaded once, on the first
    boot with the volume. After that, `/private/var/.redwood-var-seeded` says the volume keeps itself.
  - **init's write checks** clear `/tmp/t` first, since it outlives the boot now.
  - **var_store** (`src/var_store.c`): when `/private/var` is hfs (statfs64), the end-of-run save becomes `sync()`,
    and the kernel's run-end handshake is answered as for a save. `var_store save` does nothing.
  - **Built from source now:** init and var_store are compiled from source (`pipeline/build_init.sh` into
    `port/build/userland/`), and `build_root_app.sh` puts them into the root. Until now the root carried the donor's
    prebuilt init from 2026-11-15, because build.sh runs with PREBUILT=1, so changes to `src/init.c` never reached
    the phone.
  - **The source init vs the prebuilt one:** the source init has one addition, Test 169's ringtones bind, which is
    used only with the root on the flash (`root_apfs`).
  - **First try: init would not start.** "initproc failed to start -- Library not loaded: /usr/lib/libSystem.B.dylib".
    The linker in WSL is LLVM's `ld64.lld` (`xnu-deps/host/bin/ld64` is a wrapper for it). It accepts `-static` but
    still writes a dynamic executable (LC_MAIN and LC_LOAD_DYLINKER), and process 1 cannot have dyld.
  - **The fix: `pipeline/macho_static.py`.** It does what ld64 -static does:
    - drops LC_LOAD_DYLINKER, LC_MAIN and the dyld info commands;
    - writes the entry point as an LC_UNIXTHREAD (ARM_THREAD_STATE64, pc = `_start`);
    - clears MH_PIE and MH_DYLDLINK.

    `build_init.sh` links with `-Wl,-no_fixup_chains -Wl,-headerpad,0x400`. With classic rebase opcodes every pointer
    in the file already holds its unslid address; lld keeps arm64 executables PIE even with `-no_pie`. The result has
    the same load commands as the donor's init.
  - **Result:** iOS came up on the flash volume. init printed:
    - "[ ok ] mount the flash volume (hfs, /dev/ufs_var) on /private/var";
    - "111 directories, 9 files";
    - var_store's archive moved across once: "8287 directories, 6647 files (795681340 bytes), 56 links";
    - "36 checks, 0 failed".

    Then `dfq` reported `/private/var /dev/ufs_var: 99.08 GB free of 100.31 GB`. The home screen and apps were all
    there, carried across.
  - **Still "iPhone Storage Full".** Settings > General > iPhone Storage said "2 GB of 2 GB Used": iOS sizes the
    phone by `/`, the 2.49 GB read-only root (`root_device`, free 0), not by `/private/var`.
- **Test 412 (kernel t412, root test411):** on an iPhone, `/` and `/private/var` share one APFS container, and
  statfs("/") answers with the container's size and its shared free space. The root volume (the donor's apfs driver,
  `apfs_vfs_getattr`, now in the overlay as `bsd/miscfs/apfs/apfs_vfsops.c`) does the same:
  - when it is the root (MNT_ROOTFS) and an hfs volume is mounted at `/private/var`, it reports its own blocks plus
    that volume's, and that volume's free space.
  - **Result:** this boot was also the first restart on the kept volume. init said "the data volume of the last
    boot: the flash volume keeps it itself" (the seed marker had survived), and 36 checks, 0 failed.
  - `dfq`: `/ root_device: 99.04 GB free of 102.80 GB` and `/private/var /dev/ufs_var: 99.04 GB free of 100.31 GB`.
  - No "iPhone Storage Full" alert. The Settings badge and its "iPhone Storage Full" row are gone. Settings >
    General > iPhone Storage says "3.7 GB of 103 GB Used".
  - The 50 MB test file from Test 410 was removed with the new `dfq rm`.
  - **Standard image:** r5q-install-boot-412.img, with `ufs_var=1 var_hfs=1` in `phone-args.txt`.

## Where it stands

- **What is kept:** `/private/var` is a 100 GB journaled HFS+ volume on the phone's flash. It is kept across boots
  and crashes (the journal), and iOS sees the whole phone.
- **The way back:** leave out `var_hfs=1`. init then uses the RAM filesystem and var_store's archive, which is the
  last RAM-filesystem state from Test 410.
- **Not done:**
  - Data protection (CONFIG_PROTECT 0, no AppleKeyStore), the same as the RAM filesystem.
  - No fsck on the phone: the journal is all there is.
  - `/dev/ufs_var` writes go through `ufs_write()` one request at a time; nothing has been measured for speed yet.
