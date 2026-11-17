# Modifications (Apple Public Source License 2.0, section 2.2(c))

This directory is derived from [G986N-iOS](https://github.com/leh0726/G986N-iOS) (commit 946de0fed8e5), itself a
port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG, whose kernel side is under the
Apple Public Source License 2.0 ([LICENSE](LICENSE)). It was changed for the Samsung Galaxy S10 Lite (r5q)
native iOS port between November 1 and November 17, 2026; the git history of this repository has the details.

`loader.c`, `efi.h` and `identity.h` come from the donor's `xnuloader/`. What changed, broadly: live mode (the loader
keeps serving fastboot on one core while iOS runs on another), the maintenance commands (logs, partition reads and
allowlisted writes, boot-args, clock, peeks, screenshots), the PMIC RTC clock, the r5q modem's start through
TrustZone (PAS) with rmtfs memory and the AOP, the S2MU106 charger set-up over QUP0 SE4 at every start, and the
build-time test labels. The `build-*.sh` scripts here are new.
