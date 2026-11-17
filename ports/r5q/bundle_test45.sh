#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test45.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 45 --firmware-number 45 --mode diagnostic \
  --notes "Test 44 (log slot) confirmed on the phone: the full firmware has the Qualcomm USB device protocol, 72 Block IO handles on 4 UFS units, 6859 MB free RAM (largest 6326 MB at 0xF4000000). Test 45 = Test 44 + a minimal fastboot device on Qualcomm EFI_USBFN_IO_PROTOCOL in the boot manager (VID 18D1 PID D00D, MS OS 2.0 descriptors so Windows binds inbox WinUSB with the Android interface GUID; getvar, download up to 1 GiB into RAM below 0x27F000000, oem crc, continue). Waits 90 s for the PC, 60 s idle limit; never writes storage beyond the log slot. usb_host_session.py on the PC pushes a 256 MiB random payload and checks the CRC32." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 44 (2026-11-16 19:08)." | grep -E '"bundle"|"image"'
