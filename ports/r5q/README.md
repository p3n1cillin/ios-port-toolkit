# Galaxy S10 Lite (r5q) — the native iOS port's working tree

This is the port the toolkit at the repository root was distilled from: iOS 16.5 running natively on the
Samsung Galaxy S10 Lite (SM-G770F, SM8150), installed in place of Android (iOS system volume on the phone's
UFS `userdata`, boot chain in `boot`). It keeps the sources, scripts and notes; no firmware, no Apple
binaries, no build outputs and no phone backups are in this repository (see [the NOTICE](../../NOTICE.md)).

## Where things are

| Path | What |
| --- | --- |
| `port/xnuloader/loader.c` | the EFI loader that starts XNU from the phone's UEFI (Mu-Silicium): live mode, fastboot maintenance commands, modem start (TrustZone PAS), the charger set-up, the clock |
| `port/r5qfull/`, `port/testNN/` | the Mu-Silicium platform pieces per test |
| `kernel-build/r5q-overlay/` | the r5q kernel drivers on top of the donor port's XNU: UFS, touch, buttons, cpufreq, framebuffer, battery and charger (S2MU106), the modem link (SMP2P, GLINK, QRTR, rmtfs, TFTP, pd-mapper, servreg) and Wi-Fi (WLFW) |
| `kernel-build/r5q-overlay/bsd/dev/redwood_wlfw_ath10k*.h` | Linux ath10k's WLFW message tables and copy-engine tables (ISC), generated |
| `phone.py` | everything done with the phone from the PC: `cycle` (flash, restart, follow the log), `args`, `logs`, `smem`, `screenshot`, … |
| `build_*.py`, `package_install_boot.py` | the build pipeline (kernel in a sandbox, loader, shim, BOOT image) |
| `wifi/WIFI-PLAN.md` | the Wi-Fi bring-up, step by step, with what every test showed |
| `SESSION.md` | the test-by-test log of the earlier stages |

## State (Test 110)

- iOS boots from the phone's own storage, without a PC; data persists; the clock comes from the PMIC RTC.
- Touch, display, buttons, all CPU cores, battery gauge.
- **Charging:** the S2MU106's watchdog (which suspended charging with no driver) is switched off by the loader;
  the kernel reads the charger's input type (wall / charging port / PC) and sets the input limit; iOS shows
  the charging bolt and the battery's capacity.
- **Modem:** booted through TrustZone; SMP2P, GLINK, QRTR, rmtfs, TFTP (Android's path rules), pd-mapper.
- **Wi-Fi:** the WCN3990 firmware runs (the vendor partition's `wlanmdsp.mbn`), the WLFW handshake is done
  (MSA memory assigned through TrustZone, board data sent), the firmware is ready and in mission mode.
  Next: the host's copy engines, HTC/WMI, and a network interface.

Setting up a machine to build and run it: [docs/SETUP.md](../../docs/SETUP.md). Licences: the loader and the kernel
drivers are Apple Public Source License 2.0 (modified from the donor port), the rest MIT; see
[NOTICE.md](../../NOTICE.md).
