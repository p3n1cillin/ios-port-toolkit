#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test46.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 46 --firmware-number 46 --mode diagnostic \
  --notes "Test 45: LocateProtocol(EFI_USBFN_IO_PROTOCOL) = NOT_FOUND on the full firmware (UsbfnDwc3Dxe loaded, UsbConfigDxe saw the cable attached; UsbfnIo is only installed for its own clients), USB session skipped, Android back, baseline verified. Test 46 = Test 45 with Qualcomm UsbMsdDxe (EFI_USB_MSD_PROTOCOL, as QcomPkg MassStorage uses it) serving ONLY a 3 GiB RAM disk that the firmware formats as FAT32 (label R5QRAM): Windows inbox mass storage, no driver. usb_msd_session.py copies a 256 MiB random TEST.BIN and DONE.TXT (name size crc); the firmware walks the FAT, rebuilds each file contiguously in RAM, checks size and CRC32 and logs MATCH/MISMATCH. Formatter and FAT walker host-checked (fsck.fat clean; mtools-copied files found, CRC match). No phone storage is assigned to USB." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 45 (2026-11-16 19:21)." | grep -E '"bundle"|"image"'
