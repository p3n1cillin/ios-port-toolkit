# Notice

## Third-party material in this repository

- `ports/r5q/kernel-build/r5q-overlay/` holds modified copies of files from the donor iOS port
  [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG (its own code under the MIT License;
  the parts derived from Apple's XNU under the Apple Public Source License 2.0). Each file keeps its header.
- `ports/r5q/kernel-build/r5q-overlay/bsd/dev/redwood_wlfw_ath10k.h` and `redwood_wlfw_ath10k_ce.h` are
  generated from the Linux kernel's ath10k driver (`qmi_wlfw_v01.[ch]`, `snoc.c`), Copyright (c) 2018 The Linux
  Foundation, under the ISC License (the header keeps its text).
- The `port/` platform pieces build on [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
  (BSD-2-Clause-Patent).

## Not in this repository

Firmware images (Samsung, Qualcomm), Apple binaries and the iOS system image, build outputs, decompiled or
disassembled vendor code, copies of other projects' sources, phone backups and logs, and per-device private
files (`devices/*.local.json`, live device trees). Device serial numbers and local user paths were removed
from the files that are here.
