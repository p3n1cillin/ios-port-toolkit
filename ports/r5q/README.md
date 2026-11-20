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
| `kernel-build/r5q-overlay/bsd/dev/redwood_adsp.c` | the audio DSP and the sensors DSP (SLPI: FastRPC, the file server, SEE), the microphone's SLIMbus work |
| `kernel-build/r5q-overlay/bsd/miscfs/apfs/apfs_vnops.c` | read-time patches to iOS's shared cache (with their code-signature page hashes) |
| `wifi/inject_bbd.c`, `wifi/inject_app.c` | libraries inserted into backboardd and the apps: sensors as HID services, rotation, auto-brightness, magnetometer calibration, app fixes |
| `wifi/r5qd.c`, `wifi/r5qd_client.py` | changes to the running iOS over Wi-Fi without a reboot (put, run, kill, reload a library) |
| `wifi/*_test.*`, `wifi/magrec.c`, `wifi/cm_rate.m` | on-phone probes: CoreMotion, CoreLocation, orientation, video, the magnetometer |
| `wifi/r5qsign.py`, `wifi/appinst.m`, `wifi/ipa_install.py` | signing and installing apps built here |
| `pipeline/` | how a test is built now: the root volume, the test image, the run ([pipeline/README.md](pipeline/README.md)) |
| `phone.py` | everything done with the phone from the PC: `cycle` (flash, restart, follow the log), `args`, `logs`, `smem`, `screenshot`, … |
| `build_*.py`, `package_install_boot.py` | the build pipeline (kernel in a sandbox, loader, shim, BOOT image) |
| `wifi/CAMERA-PLAN.md`, `wifi/CAMERA-FEASIBILITY.md` | the camera: how iOS 16.5 reaches it, the design, Tests 267-269 |
| `wifi/BLUETOOTH-PLAN.md`, `wifi/BLUETOOTH-IOS-RESEARCH.md` | Bluetooth, Tests 244-262, and how iOS 16.5 reaches its controller |
| `wifi/WIFI-PLAN.md` | everything since Test 111, step by step with what every test showed: Wi-Fi, sound, GPU, apps, sensors, CoreMotion, the compass, the microphone |
| `SESSION.md` | the test-by-test log of the earlier stages |

## State (Test 349)

**Works:**

- **Booting and storage:** iOS boots from the phone's own storage, without a PC. Data persists, and the clock comes
  from the PMIC RTC.
- **Basics:** touch, display (with brightness), buttons, all CPU cores, battery and charging.
- **Wi-Fi and the network:** the kernel's own WLAN driver and WPA2. Safari loads pages. Location
  works by Wi-Fi.
- **Sound and media:** speaker sound, ringtones, video playback.
- **Hardware:** vibration and the flashlight.
- **Apps:** apps built here are installed by iOS's own installer.
- **Sensors:** accelerometer, gyroscope, magnetometer, light and proximity, from the sensors DSP started by the
  kernel. In iOS:
  - rotation;
  - smooth auto-brightness;
  - CoreMotion's accelerometer, gyroscope, magnetometer and device motion in apps.

  How it's done, generically: [docs/SENSORS.md](../../docs/SENSORS.md),
  [docs/IOS-SENSORS.md](../../docs/IOS-SENSORS.md), [docs/SYSTEM-PATCHING.md](../../docs/SYSTEM-PATCHING.md).
- **Bluetooth:** the WCN3990 under iOS's own bluetoothd. Settings > Bluetooth works, with LE scanning and classic
  discovery, at 3 Mbit/s, named "iPhone". Pairing has not been tried yet. How it's done: [docs/BLUETOOTH.md](../../docs/BLUETOOTH.md).

**Partly:**

- **Camera:** the phone's own sensors in iOS's Camera app, both at 1028x772:
  - back camera: the main 48 MP IMX586 since Test 343/344 (CSI-PHY 0); the ultra-wide S5K3L6 stays available
    (`cam_main=0`);
  - front camera: the IMX616 since Test 340/341 (CSI-PHY 3);
  - both Sony sensors are cropped in the sensor to the middle 1028x772, with their output clock lowered to ~1 Gb/s a
    lane, and are CRC-clean;
  - iOS's auto-exposure drives each sensor in its own terms;
  - each sensor has its vendor colour tuning (lens shading, white-balance lights; made locally from the phone's
    own files);
  - switching cameras works, photos are saved as JPEG (HEIC's software encoder runs out of memory), and the camera
    powers off when the app closes;
  - the IMX586 sends black as 0 (Test 345), the others 64: iOS is told each one's;
  - not yet: the macro S5K5E9, full-field (uncropped) frames, and a picture of a lit scene.

  How it was made stable: wifi/CAMERA-PLAN.md, docs/CAMERA.md.
- **Compass:** the Compass app shows a heading, then freezes. Since Test 349 apps get motion data at ~84 a
  second and the compass at 50 (were 23 and 10); CoreMotion's magnetometer calibration still has to be made by
  turning the phone (owner test, outdoors best).

**Not yet:**

- **Microphone:** the WCD9340 codec never enumerates on SLIMbus and is silent over SPI too (Tests 265-266). What is ruled out is in WIFI-PLAN, "Tests 237-240" and after.
- **Metal:** there is no GPU driver for iOS, so the map in Maps and live wallpapers are missing.
- **Radios:** cellular; Bluetooth audio.
- **FairPlay:** App Store apps can't run (their code is FairPlay-encrypted). The study (wifi/FAIRPLAY-FEASIBILITY.md)
  found that Apple's FairPlay kext makes the device's keys with the SoC's fused UID and GID AES keys, which only
  Apple silicon has. Apps without FairPlay encryption (Apple's own, ones built from source) are the route.

Setting up a machine to build and run it: [docs/SETUP.md](../../docs/SETUP.md). Licences: the loader and the kernel
drivers are Apple Public Source License 2.0 (modified from the donor port), the rest MIT; see
[NOTICE.md](../../NOTICE.md).
