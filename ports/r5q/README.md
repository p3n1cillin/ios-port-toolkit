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
| `kernel-build/r5q-overlay/bsd/dev/redwood_apple80211.c` | the Apple80211 ioctls iOS's wifid uses (scan, join, link rate) over the kernel's WLAN driver (Tests 443-444) |
| `wifi/inject_sb_r5q.m` | a library inserted into SpringBoard: Control Center and status-bar fixes for the GL path (Tests 442-444) |
| `wifi/vt_enc_probe.c` | on-phone VideoToolbox encoder benchmark (HEVC presets), Tests 431-434 |
| `userland-patches/` | patches to the donor's userland (media library, init, ReplayKit export) against the donor revision they name |

## Storage (Test 412, 2026-11-20)

`/private/var` is a 100 GB journaled HFS+ volume on the phone's flash (darwin past 16 GiB, `/dev/ufs_var`), mounted
by init (`var_hfs=1`) in place of the 1.6 GB RAM filesystem; iOS reports "3.7 GB of 103 GB Used". See
[wifi/STORAGE-PLAN.md](wifi/STORAGE-PLAN.md); tools: `storage/mkplace.py`, `wifi/hfsplace.c`, `wifi/hfsmount.c`,
`pipeline/build_init.sh` + `pipeline/macho_static.py` (static init from source with LLVM's linker),
`pipeline/panic_sym.py`.

## Since Test 430 (2026-11-21 to 11-24, Tests 431-450)

The standard image is listed in [pipeline/README.md](pipeline/README.md) with each step's evidence. In short:

- **Video recording with sound:** a NEON ISP and Cisco's OpenH264 as a VideoToolbox H.264 encoder: 1080p30 at
  30 fps with the microphone in sync, 1080p60 / 720p60 from a 1634-line IMX586 mode, OIS movie mode while recording
  (Tests 431-434). Exposure bias, AE/AWB lock and the back flash in the Camera app.
- **Three back-facing lenses in iOS:** the wide IMX586 (1x), the ultra-wide S5K3L6 (0.5x) switched live
  (`kern.redwood.cam_main`), and the front IMX616 (Tests 438-442).
- **Microphone:** reliable across sessions under camera load (RX barriers and sentinels, a symmetric SLIMbus
  teardown; Tests 432-433), and the bottom/top microphones as iOS data sources (Tests 435-436).
- **Display:** native @3x (`fbscale=3`, 1125x2436 points), a 4-thread compositor, Adreno 640 at 499 MHz, GL fixes
  for Control Center and translucent views (Tests 434-449).
- **Power:** a CPU governor (`cpufreq=2`), event-driven driver threads (>12,000 to ~350 wakeups a second), slower
  polling while the screen is off (Tests 438-441).
- **Sound:** the earpiece as iOS's "Receiver" route (Tests 441-442); ReplayKit screen recordings saved (Test 436).
- **Radios:** CommCenter in the standard boot; GPS (`gps=1`) through the modem's QMI LOC with a QMI PDS bridge for
  locationd -- satellites seen, no position fix yet (Tests 436-438). Bluetooth registered before launchd (Test 445).
- **Test 450:** fixes from a review of the above (the memory benchmark no longer starts with `cpufreq=2`, GPS stops
  when its client closes, the microphone choice is fixed per recording, bounded settle retries).

## State (Test 353)

**Works:**

- **Booting and storage:** iOS boots from the phone's own storage, without a PC. Data persists, and the clock comes
  from the PMIC RTC.
- **Basics:** touch, display (with brightness), buttons, all CPU cores, battery and charging.
- **Wi-Fi and the network:** the kernel's own WLAN driver and WPA2. Safari loads pages. Location
  works by Wi-Fi.
- **Sound and media:** speaker sound, ringtones, video playback.
- **Microphone** (since Test 424, standard): the main microphone in iOS ("iPhone Microphone", built in). The
  WCD9340 codec on SLIMbus (it had looked dead because our address query used the wrong message type, Test 419),
  its capture path set as Android's driver sets it, SLIMbus channel 134 to the DSP's port SLIMBUS_0_TX, a DSP
  record stream into `/dev/redwood-mic` and the iOS audio plug-in's input. Apps record through AVFoundation at
  48 kHz. wifi/WIFI-PLAN.md, Tests 419-424.
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

- **Camera:** the phone's own sensors in iOS's Camera app:
  - back camera: the main 48 MP IMX586 since Test 343/344 (CSI-PHY 0); the ultra-wide S5K3L6 stays available
    (`cam_main=0`);
  - front camera: the IMX616 since Test 340/341 (CSI-PHY 3);
  - since Test 350/351 iOS gets the Sony sensors' whole binned frames (main 2000x1500, front 1632x1224: their full
    field of view); their output clock lowered to ~1 Gb/s a lane, CRC-clean; the ultra-wide at 1028x772;
  - iOS's auto-exposure drives each sensor in its own terms;
  - each sensor has its vendor colour tuning (lens shading, white-balance lights; made locally from the phone's
    own files);
  - switching cameras works, photos are saved as JPEG (HEIC's software encoder runs out of memory), and the camera
    powers off when the app closes;
  - the IMX586 sends black as 0 (Test 345), the others 64: iOS is told each one's;
  - since Test 427 the main IMX586 sees in dim light from the first frame: two of its 1.8 V supplies had been set to
    0.7 V, which clipped everything dim to black until the sensor warmed;
  - since Test 429 the main camera focuses: its lens motor is driven by iOS's contrast autofocus;
  - since Test 430 its optical stabiliser runs (still mode);
  - not yet: the macro S5K5E9, the sensors' full-resolution (unbinned) modes.

  How it was made stable: wifi/CAMERA-PLAN.md, docs/CAMERA.md.
- **Compass:** the Compass app shows a heading, then freezes. Since Test 349 apps get motion data at ~84 a
  second and the compass at 50 (were 23 and 10); CoreMotion's magnetometer calibration still has to be made by
  turning the phone (owner test, outdoors best).

**Not yet:**

- **Metal:** there is no Metal device for iOS, so the map in Maps and live wallpapers are missing. The plugin entry
  point exists (wifi/METAL-FEASIBILITY.md); a Metal implementation over the GL path is a large project (AIR shaders).
- **Radios:** cellular: in the standard boot (since Test 391) iOS's CommCenter runs on the phone's own modem: the
  baseband manager boots (fake: the modem already runs), engages and stays ready; CommCenter's QMI goes to the modem
  through a kernel QMUX-to-QRTR bridge; Settings has its Cellular section; the SIM state comes from the modem ("no
  SIM" -- there is none in the phone). Registration, calls, SMS and data need a SIM (and data a packet interface).
  Bluetooth audio. (wifi/CELLULAR-PLAN.md, docs/CELLULAR.md)
- **FairPlay:** App Store apps can't run (their code is FairPlay-encrypted). The study (wifi/FAIRPLAY-FEASIBILITY.md)
  found that Apple's FairPlay kext makes the device's keys with the SoC's fused UID and GID AES keys, which only
  Apple silicon has. Apps without FairPlay encryption (Apple's own, ones built from source) are the route.

Setting up a machine to build and run it: [docs/SETUP.md](../../docs/SETUP.md). Licences: the loader and the kernel
drivers are Apple Public Source License 2.0 (modified from the donor port), the rest MIT; see
[NOTICE.md](../../NOTICE.md).
