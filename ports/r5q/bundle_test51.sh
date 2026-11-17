#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test51.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 51 --firmware-number 51 --mode diagnostic \
  --notes "Test 50 (GIC fix, root.apfs as md0): hardware markers confirmed, baseline verified. The kernel ran on the phone under the full firmware with the 6 GiB window at 0xF4000000, mounted md0 (APFS volume redwood, 61 files), tried /usr/appleinternal/sbin/launchd.development and /sbin/launchd (on the system volume) and panicked in init; the panic report came back. GIC: the kernel left PPI 19 on (disabled by the loader) and PPI 18 (UEFI's tick) off. Test 51 = the iOS system volume (system.apfs, 2492203008 bytes, crc32 cdc272da) copied by the PC onto the USB RAM drive (RAM disk at 0x1A0000000) BEFORE the loader runs, rebuilt by the firmware at the resident address 0xFC000000 (CRC checked), RAM disk and resident pages freed (PcdDebugPropertyMask 0x27: no clear-on-free, verified in the DXE core's AutoGen; CRC re-checked after the free), then the loader: system.apfs = md0 root (rdres 948c0000), root.apfs = md1, 240 s timed return; R5Q_GIC_REENABLE puts UEFI's PPIs back. Host-checked: the FAT walker rebuilt the real 2.49 GB image in 1 fragment, CRC match." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 50 (2026-11-16 20:00)......" | grep -E '"bundle"|"image"'
