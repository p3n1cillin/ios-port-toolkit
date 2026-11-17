# Notice

## Licenses in this repository

| Part | License |
| --- | --- |
| Everything not listed below: the toolkit (profiles, generator, runner, skills, tools, docs), the S10 Lite port's scripts, build pipeline, phone tool, Mu-Silicium platform pieces and notes | [MIT](LICENSE) |
| `ports/r5q/port/xnuloader/` (the EFI loader) | [Apple Public Source License 2.0](ports/r5q/port/xnuloader/LICENSE), changes in [MODIFICATIONS.md](ports/r5q/port/xnuloader/MODIFICATIONS.md) |
| `ports/r5q/kernel-build/r5q-overlay/` (the r5q kernel drivers) | [Apple Public Source License 2.0](ports/r5q/kernel-build/r5q-overlay/LICENSE), changes in [MODIFICATIONS.md](ports/r5q/kernel-build/r5q-overlay/MODIFICATIONS.md) |
| `ports/r5q/kernel-build/r5q-overlay/bsd/dev/redwood_wlfw_ath10k.h`, `redwood_wlfw_ath10k_ce.h` | ISC (generated from Linux's ath10k, Copyright (c) 2018 The Linux Foundation; see their headers) |

## Where the APSL parts come from

The loader and the kernel drivers are modified from
[G986N-iOS](https://github.com/leh0726/G986N-iOS) (commit 946de0fed8e5), a port of
[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG, whose kernel side is under the Apple
Public Source License 2.0 and whose user-space side is under the MIT License. The kernel itself is Apple's
open-source XNU (`xnu-8796.121.2`). The phone's UEFI comes from
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (BSD-2-Clause-Patent), which the `port/` platform
pieces build on.

## Not in this repository

Firmware images (Samsung, Qualcomm), Apple binaries and the iOS system image, SDKs, build outputs, decompiled or
disassembled vendor code, copies of other projects' sources, phone backups and logs, and per-device private files
(`devices/*.local.json`, live device trees). [docs/SETUP.md](docs/SETUP.md) says where each input comes from.
Device serial numbers and local user paths were removed from the files that are here.
