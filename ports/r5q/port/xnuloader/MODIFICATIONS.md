# Modifications (Apple Public Source License 2.0, section 2.2(c))

This directory is derived from [G986N-iOS](https://github.com/leh0726/G986N-iOS) (commit 946de0fed8e5), itself a
port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG, whose kernel side is under the
Apple Public Source License 2.0 ([LICENSE](LICENSE)). It was changed for the Samsung Galaxy S10 Lite (r5q)
native iOS port between November 1 and November 18, 2026; the git history of this repository has the details.

`loader.c`, `efi.h` and `identity.h` come from the donor's `xnuloader/`. What changed, broadly: live mode (the loader
keeps serving fastboot on one core while iOS runs on another), the maintenance commands (logs, partition reads and
allowlisted writes, boot-args, clock, peeks, screenshots, touch gestures), the PMIC RTC clock, the r5q modem's start through
TrustZone (PAS) with rmtfs memory and the AOP, the S2MU106 charger set-up over QUP0 SE4 at every start, loading the
Wi-Fi, audio and sensors DSPs (the sensors DSP held for the kernel to release), and the build-time test labels. The `build-*.sh` scripts here are new.
- Test 289: a 4 MB block below 4 GB for the camera's image engine, passed to the kernel as `camdma_pa=`.
