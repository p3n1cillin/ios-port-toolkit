# Modifications (Apple Public Source License 2.0, section 2.2(c))

This directory is derived from [G986N-iOS](https://github.com/leh0726/G986N-iOS) (commit 946de0fed8e5), itself a
port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) by OOSEX GANG, whose kernel side is under the
Apple Public Source License 2.0 ([LICENSE](LICENSE)). It was changed for the Samsung Galaxy S10 Lite (r5q)
native iOS port between November 1 and November 17, 2026; the git history of this repository has the details.

These files replace the donor's files of the same path in its `xnu/files/` tree when the kernel is built
(`kernel-build/build-xnu-sandbox.sh` copies them over it). What changed, broadly:

- `bsd/dev/redwood_ufs.c`: the r5q's UFS partitions, the `darwin` area in `userdata`, writes for the data volume,
  named partition reads for the modem's file servers.
- `bsd/dev/redwood_sec_ts.c`: the r5q's touch controller and buttons bus, the S2MU106 fuel gauge, the S2MU106
  charger's status and input limit over QUP0 SE4, battery reporting to iOS.
- `bsd/dev/redwood_buttons.c`, `redwood_cpufreq.c`, `iokit/Kernel/MiniIOMobileFramebuffer.cpp`,
  `osfmk/arm64/gic_v3_irq.c`: the r5q's buttons, CPU frequency domains, panel and interrupt controller.
- `bsd/dev/redwood_wifi.c`: the link with the r5q's modem (SMP2P, GLINK, QRTR), its file servers (rmtfs, TFTP with
  Android's path rules), pd-mapper, servreg, DIAG and memshare probes, the WCN3990 supplies, and the Wi-Fi
  firmware's WLFW handshake (MSA assignment through TrustZone, board data, WLAN configuration and mission mode).

New in this directory (not the donor's): `bsd/dev/redwood_wlfw_ath10k.h` and `redwood_wlfw_ath10k_ce.h`, generated
from Linux's ath10k (ISC, see their headers).
