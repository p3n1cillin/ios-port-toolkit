# The test pipeline (from Test 140 on)

How a test image is made and run now. The early tests used `generator/` and the per-test `genNN.py` files; these
scripts are what has been used since. They were kept outside the tree until Test 243.

| Step | Command (Git Bash on Windows, WSL for Linux steps) | Makes |
| --- | --- | --- |
| Kernel | `wsl -e bash -lc "cd /tmp && python3 -I $R/build_xnu_linux.py $R $R/port/build/xnu-r5q-tN.macho"` (R = this port from WSL) | `port/build/xnu-r5q-tN.macho` (+ `.build.log`, `.receipt.json`) |
| Root volume | `wsl -e bash -c "bash $R/pipeline/build_root_app.sh N"` | `port/build/testN-root.apfs.new`: the donor userland's boot volume with our injected libraries (`wifi/inject_app.c`, `wifi/inject_bbd.c`), r5qd, the launchd jobs |
| Test image | `cp port/xnuloader/build-testA.sh port/xnuloader/build-testB.sh; bash pipeline/mktest.sh A B xnu-r5q-tK.macho testX-root.apfs.new "label"` | `pipeline/gen/genB.py` from `genA.py`, then the loader, shim and the 64 MiB `port/build/r5q-install-boot-B.img` |
| Run | `python phone.py cycle port/build/r5q-install-boot-B.img SECS [boot-args…]` | flash, restart, follow the log into `port/build/cycles/` |
| Live changes | `python wifi/r5qd_client.py reload LIB PROGRAM` / `put` / `kill` / `run` | no reboot (injected libraries, /var files, test tools) |

Notes:

- Do **not** `export MSYS_NO_PATHCONV=1` for `mktest.sh`; do set it for `phone.py sh` and `r5qd_client.py`.
- `phone.py cycle … ARGS`: plain `key=val` ARGS **replace** the stored boot-args. Use `+key=val`, `-key` or `std`
  for changes to the standard set in `phone-args.txt` (below).
- Kernel builds take about 10 minutes. Run them in the background.
- Every test's purpose and result goes into `wifi/WIFI-PLAN.md` as it is run.

## Phone helpers added 2026-11-19 (to save time)

| Command | Does |
| --- | --- |
| `phone.py cycle IMG SECS +key=val -key` | boot-args as **changes to the standard set** in `phone-args.txt` (`std` = the standard set as is). No more retyping the whole set; a plain `key=val` list still replaces it as before |
| `phone.py cycle …` (any) | if the run **ends the system**, says so with the 15 lines before it |
| `phone.py rescue` | back to a running iOS from anything: a stuck USB link ("no link") is cleared by restarting the phone from iOS over Wi-Fi (`r5q_reboot`); then the listening window, where the **log slots are saved** (the crash's record, before a new run overwrites it), then iOS with the standard boot-args |
| `bash pipeline/build_phone_tools.sh` (WSL) then `phone.py tools` | every on-phone tool built into `port/build/phone-tools/` and put into `/var/r5q/bin` in one go (after a crash `/var` may not have kept them) |
| `bash pipeline/test.sh B "label" [+args…]` | a whole test in one command: the kernel built (or `KERNEL=xnu-r5q-tNNN.macho` reused), the root (newest, or `ROOT=build`), the image from test B-1, the run (`FOLLOW=` seconds) |

`phone-args.txt` is the standard boot-arg set. Change it when a feature's arg becomes part of every boot.

## Test 436 on hardware (2026-11-23)

The installed Test 436 BOOT image was read back and matched SHA-256
`1d7b330d360a89d0a43f784b70a60469b216d66d7e30be3dde781c939349f979`.
It boots iOS 16.5 on the SM-G770F. This build combines `xnu-r5q-t436-final.macho`
and `test436-root-verified.apfs`. Build receipts, the image report, and the 11 passing
ARM64 shim cases are in `port/build/`. The hardware boot log is in
`port/build/cycles/20261123-114040-r5q-install-boot-436/live.txt`.
The combined patch for the two WSL donor userland files is
`userland-patches/test436-donor.patch` against donor revision `946de0f`.

- **Microphone.** AVAudioRecorder captured 96,000 samples in two seconds on each of
  the ADC1 and ADC3 selections, with RMS levels of −47.0 and −44.5 dBFS.
  The test restored the default ADC1 selection and removed its temporary WAV.
  This exercises the kernel route selector. Per-app data-source selection still needs a UI test.
- **ReplayKit.** A Control Center recording produced
  `/var/mobile/Media/DCIM/100APPLE/R5Q_Replay_RHEOUS.MP4`. The 2,674,109-byte file
  has bounded `ftyp`, `mdat`, and `moov` boxes. The export uses a temporary file and
  publishes the finished copy atomically. Photos library indexing and playback remain unverified.
- **GPS with `+gps=1`.** The modem's QMI LOC session returned 76 NMEA indications
  and 19 satellite reports, with zero valid position fixes. `locationd` opened a
  baseband connection. The boot log and Maps probe did not show a PDS client request
  or a bridge transaction. Trace the `locationd` QMUX channel startup before
  relying on the bridge, then test for a satellite fix with a clear view of the sky.

The phone rebooted on Test 436 with the standard args (`fbscale=3` and experimental
GPS disabled), and the MP4 persisted. Test 435 remains available as the hardware rollback image
(`bac3c13960c21896fa315372d030bb4971855f699b87aa6ce3e434874f59088c`).
The shim emulator now sizes its source regions from the input files. Isolated generator
checks for 435 to 436 and 436 to 437 verified that both honor the supplied kernel and root paths
and propagate the shim test.

## The standard image (2026-11-24, Test 451)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-451.img (`SHA-256 fd90e2191aff03aefb17a582bd450052dce6abddb2f4a44f42b5f7f24dbed0a2`): kernel `t451`, root `test451` (`test451-root.apfs.new`, rebuilt for libr5q-bbd), `phone-args.txt` unchanged.
On top of Test 450, the six minor findings of the review:
- **Camera frame timing (`redwood_camera.c`, `redwood_sec_ts.c`):** one rule for the shortest frame, `redwood_r5q_cam_min_frame_lines()` (1634 lines only while the full-frame IMX586 mode streams, else 3268), used by both the sensor and what iOS is told; with `cam_full=0` iOS had been told 1634 for a 3268-line mode.
- **OIS movie mode kept (`redwood_sec_ts.c`, `redwood_camera.c`):** the mode iOS last asked for survives a session restart (`main_ois_up` asks for movie mode again, logged "movie mode asked again"); closing the camera device clears it. Not exercised on hardware: the Camera app's lens button zooms instead of switching lenses while recording, so no session restart happens mid-recording from the UI.
- **GPS client race (`RedwoodBaseband.cpp`, `redwood_wifi.c`):** `clientClose` sets `fClosed` before its ports close; the PDS bridge checks `RedwoodBasebandClientClosed()` under its lock and no longer adopts a closing connection.
- **Compass (`wifi/inject_bbd.c`):** an SLPI bias at status 1 still seeds the offset, but only status >= 2 stops libr5q-bbd's own sphere fit, and status 0 starts it again. `cm_test 3`: accelerometer 30, gyro 15, magnetometer 25, device motion 30; no backboardd crash logs.
- **Build (`pipeline/build_init.sh`, new `pipeline/build_openh264.sh`):** without OpenH264 or the donor's `vt_h264_enc.c` it says what is missing and still builds init and var_store. `build_openh264.sh` rebuilds `libopenh264_enc.a` (v2.4.1, 81 objects, as Test 431's) from a clean clone; libredwood-media.dylib links against it.
- **Boot line (`port/xnuloader/loader.c`):** the loader logs `[LAB] boot-args: 590 of 607 bytes used`, or `FULL ... the kernel will NOT see "..."` (firmware ring, `phone.py logs`).
- **Verified on hardware:** boots, Wi-Fi joined, 0 panics; `record_test 3` twice (-46.3 / -46.1 dBFS); two Camera.app movies imported into Photos, `IMG_0038.MOV` (1080p H.264, 389 frames in 13.10 s = 29.7 fps, AAC 13.10 s) and `IMG_0039.MOV` (382 frames in 12.83 s = 29.8 fps, AAC 12.83 s), `cam: ois mode=1` / `=0` around a recording.
- **Seen on the way: a modem fatal error during a Wi-Fi pull (not reproduced).** ~3 minutes into the first run, while pulling a 24 MB movie over r5qd, every QMI send to the modem began failing with 55 (ENOBUFS: `glink_send` found no free modem intent within its wait), then servreg reported `msm/modem/wlan_pd` DOWN; our ack was logged but could not be sent, and the modem died: SMP2P `err-fatal`, item 421 "servreg_monitor.c:502:Servreg ack timedout:msm/modem/wlan_pd". After `phone.py restart`, ~90 MB of pulls left the modem healthy (`err-ready`). The same family as Tests 168 and 414 (wlan_pd restarting under sustained transfer), now with the modem itself lost because the ack path shares the starved GLINK channel. Open: send servreg acks on a path that cannot be starved (a reserved intent), and find what stops the modem from returning intents.

## The standard image before (2026-11-24, Test 450)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-450.img (`SHA-256 f1ac4d6bf2c39936d878ef0550c2eb01bd816fd72b6ef69fa7198a744953e9f7`): kernel `t450`, root `test447` (`test447-root.apfs.new`), `phone-args.txt` unchanged.
On top of Test 449, fixes from a review of Tests 431-449 (three reviewers over the kernel, audio/camera and userland diffs):
- **`cpufreq=2` no longer starts the all-core memory benchmark (`redwood_cpufreq.c`).** `enabled` still held 2 from the `cpufreq` parse when `membench` was absent, so `bench_all` ran at every boot (8 threads, 2 x 16 MB wired each, seconds of memset/memcpy on every core during launchd and SpringBoard's start). Test 449's boot log shows `membench: all at once: core 7: writing 6651 MB/s ...`; Test 450's has no membench line and `cpufreq: dynamic governor active`.
- **GPS stops when its client closes (`redwood_wifi.c` `gps_pds_close`):** it cleared `gps_pds_want_on` without `wakeup`, so the QMI LOC session (GNSS on, 1 Hz) ran on with no client until another client disabled it. Not exercised on hardware yet (needs locationd's QMUX client to close).
- **The microphone is fixed per recording (`redwood_adsp.c`):** `r5q_cdc_mic_sel()` now returns the source latched when the capture path came up; on/on2/settle/down had each re-read `kern.redwood.mic_sel`, so a switch mid-recording mixed the ADC1/ADC3 paths or left ADC1 + MIC BIAS1 on.
- **Bounded unmute/settle retries (`redwood_mic.c`):** 3 tries each (a failing settle had been retried every loop, each try holding the SLIMbus link for seconds with its register snapshot).
- **`wifi/vt_enc_probe.c`:** a failed `CVPixelBufferCreate` no longer crashes the probe with its global VideoProcessing prefs set.
- **Verified on hardware:** boots to SpringBoard, Wi-Fi joined, 0 panics; `record_test 3` twice in a row: 144000 samples each, -46.4 / -46.8 dBFS RMS (room); a Camera.app video, `IMG_0037.MOV`, imported into Photos: `av_check.py`: h264 1920x1080, 634 frames in 21.13 s (30.0 fps), AAC 21.13 s, both from 0 (max volume -36 dB).
- **Left for later (minor, from the same review):** with `cam_full=0` the kernel reports 1634 frame lines while the cropped mode streams 3268 (`redwood_camera.c` vs `redwood_sec_ts.c`); OIS movie mode is not restored after a session restart; `gps_pds_client` can be stored after `clientClose` in a narrow race; `inject_bbd.c` takes an SLPI compass bias at status 1 and never returns to its own sphere fit; `build_init.sh` needs OpenH264 and `vt_h264_enc.c`, which are not in the repo; the boot line is 590 of 607 bytes (`toolkit/tools/bootargs_budget.py`: 17 left) and the loader cuts the end without a warning.

## The standard image before (2026-11-23, Test 449)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-449.img (`SHA-256 9c441569e75ecc229e6d45673339434e34d39a55d46376ded6c5ab7148b182ec`): kernel `t449` (`xnu-r5q-t449.macho`), root `test447` (`test447-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 447:
- **Fast 4-thread Gold/Prime cached display compositor (`r5q-overlay/iokit/Kernel/MiniIOMobileFramebuffer.cpp`):** Tuned default `fb_threads` from `3` → `4` (matching SM8150's 4 high-performance Gold/Prime cores without spilling onto slower Silver cores), staged horizontal bilinear output into an L1-cached per-thread row buffer with a single burst `memcpy` per row into write-combining panel memory (replacing 2.6M uncached 4-byte writes/frame), and kept the `a == b` fast-path for solid UI regions. Cuts active display-driver time from **8.8–9.4 ms → 3.83 ms** per frame (**2.4× faster**), delivering **59–60 fps** continuous scrolling (`268/295` frames under 18 ms, 95th percentile under 19 ms, `0` swaps waited).
- **Adreno 640 nominal GPU clock boost (`r5q-overlay/bsd/dev/redwood_adreno.c`):** Bumped `r5q` Adreno 640 default OPP from `427 MHz` (`192` SVS_L1) → **`499 MHz`** (`256` NOM on both `gfx.lvl` and `mx.lvl`), giving +17% GPU headroom during blur/scroll animations at cool 53–64 °C sensor temperatures.
- **Quieted brightness-fade log spam (`r5q-overlay/iokit/Kernel/MiniIOMobileFramebuffer.cpp`):** Capped routine `fb: backlight:` log lines from `300` → `12` to eliminate kernel ring-buffer contention during screen brightness fades.
- **Verified on hardware:** `dfq` reports `98.02 GB free of 100.31 GB` on `/private/var` (`/dev/ufs_var`), `0` CrashReporter logs, and all Test 447 visual fixes intact.

## The standard image before (2026-11-23, Test 447)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-447.img (`SHA-256 2646bce06bf15263505c08f31cc6c5348c27a77ed199c16ac847670ccad08f88`): kernel `t445` (`xnu-r5q-t445.macho`), root `test447` (`test447-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 445:
- **Adreno 640 (`FD640`) diagonal seam elimination (`userland/src/eagl.c`, `libredwood-gl.dylib`):** Defaulted `FD_MESA_DEBUG=noubwc` when unset in `libredwood-gl.dylib` (avoiding the 512-byte `kern.bootargs` limit and Adreno 640 UBWC compression artifacts on blurred/translucent quads). Completely eliminates the diagonal dotted pixel seam across Control Center, Spotlight, Today View, and App Library while maintaining 1.1–1.6 ms GPU frame times.
- **Hardware-blended `vibrantColorMatrix` fallback (`userland/src/eagl.c`, `libredwood-gl.dylib`):** Added `vibrant_fallback_fragment` for GPUs where `EXT_shader_framebuffer_fetch` is not offered (`FD640`), computing exact premultiplied-alpha hardware blend (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`) from the color matrix's row sums and bias (saving/restoring GL blend state per draw). Eliminates solid opaque grey App Library pods and Control Center discs while rendering App Library category pods as crisp frosted translucent glass and keeping `"Not Playing"` in Control Center and Lock Screen text sharp and visible.
- **Rootfs wiring (`pipeline/build_root_app.sh`):** Wired `build_root_app.sh` to install `port/build/userland/libredwood-gl.dylib` into `root/lib/libredwood-gl.dylib`. Verified on cold boot with `0` CrashReporter logs across Lock Screen, Home Screen, Control Center (blue Bluetooth + blue Wi-Fi on first swipe), Spotlight, Today View, and App Library.

## The standard image before (2026-11-23, Test 445)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-445.img (`SHA-256 806d1a553335bbdcfd966f6471b580bcd335629502be0a775bc2ff66cb86f19f`): kernel `t445` (`xnu-r5q-t445.macho`), root `test445` (`test445-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 444:
- **Early Bluetooth HCI registration (`r5q-overlay/bsd/dev/redwood_wifi.c`):** Added `bt_early_register()` called in `redwood_wifi_init()` when `bt=9` (before `launchd` starts) to publish `IOService "bluetooth"` (`local-mac-address`), `/dev/uart.bluetooth`, `/dev/btwake`, and `com.apple.uart.bluetooth` kctl immediately, blocking `bt_ctl_connect()` on `&bt_hw_ready` until `bt_stage4()` completes firmware bring-up. Eliminates all `bluetoothd: No bluetooth on this device.` cold-boot exits; verified live on the **very first cold-boot Control Center swipe** (no respring): Bluetooth button is vibrant blue (ON), Wi-Fi is blue (ON), Airplane Mode is translucent grey + crisp white airplane, and status-bar Wi-Fi is full-contrast 3 bars.
- **Fast Wi-Fi rejoin (`r5q-overlay/bsd/dev/redwood_wifi.c`):** Reduced `r5q_rejoin_thread` initial retry from `15 s` to `2 s` (then `5 s`, `15 s`, …) so AP deauths after Settings → Wi-Fi off-channel scans recover immediately.
- **Linear Compass calibration (`wifi/inject_bbd.c`):** Removed the non-linear `>58 µT` radial clamp that distorted the 3D magnetic sphere for CoreMotion's `CLCompassCalculator`, and tuned SLPI `mag_cal` (`status >= 1`, `0.60` initial lock) + IRLS sphere-fit convergence (`4 µT` spacing, `12` samples, `0.35` blend).
- **Full stock-app & Settings sweep:** Visually verified 25+ stock apps and major Settings panes (`Wi-Fi`, `Bluetooth`, `Notifications`, `Sounds & Haptics`, `Control Center`, `Battery` with live 24 h graph, `Privacy & Security`, `Focus`) with `0` CrashReporter logs.

## The standard image before (2026-11-23, Test 444)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-444.img (`SHA-256 4dcd2c13c107e898c839198f3dcab8344aadaea6cce45371df3c7eecbe8865d8`): kernel `t444` (`xnu-r5q-t444.macho`), root `test444` (`test444-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 443:
- **`Apple80211` `RATE` reporting & `wifid` ioctl cleanup (`r5q-overlay/bsd/dev/redwood_apple80211.c`):** Populated `request->req_val = answer.rate[0]` in `case 8` (`APPLE80211_IOC_RATE`) so `SCDynamicStore` `State:/Network/Interface/en0/AirPort` reports real link rate (`RATE = 866` on 5 GHz / `144` on 2.4 GHz instead of `0`), and cleanly accepted `wifid` set ioctls `365` and `383` (`return 0`), eliminating all repeating `apple80211: wifid sets 365/383: REFUSED` kernel log spam (`0` `REFUSED` lines across full boot and runtime).
- **13-app live sweep & SpringBoard polish (`wifi/inject_sb_r5q.m`):** Audited Home Screen, Calculator, Compass, Camera, Safari, Photos, Notes, Clock, Voice Memos, Files, Calendar, Settings, and Control Center at native `@3x` with `0` CrashReporter logs, `RATE = 866`, `0` `apple80211 REFUSED` lines, crisp translucent Control Center discs, and full-contrast 3-bar status-bar Wi-Fi on cold boot.

## The standard image before (2026-11-23, Test 443)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-443.img (`SHA-256 a71bb71e6391baf0411c0e943d8aa945de6ed9a74356968e981aee46dfd9c75b`): kernel `t442` (`xnu-r5q-t442.macho`), root `test443` (`test443-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 442:
- **Control Center white-circle fix & cold-boot status-bar Wi-Fi bars (`wifi/inject_sb_r5q.m` → `libr5q-sb.dylib` in `SpringBoard`):** Hooked `-[CCUIRoundButton layoutSubviews]` to replace `vibrantColorMatrix` on untextured solid-white `normalStateBackgroundView` discs with `colorWithWhite:1.0 alpha:0.16` (restoring crisp translucent discs + visible Airplane Mode / Bluetooth glyphs in Control Center), and added post-boot `SBWiFiManager` `_updateCurrentNetwork` / `_updateSignalStrength` refreshes at +10/20/35 s so the top-right status bar Wi-Fi icon displays full-contrast 3-bar signal strength automatically on cold boot. Visually verified on fresh cold-boot Control Center and Settings screenshots with 0 CrashReporter logs.

## The standard image before (2026-11-23, Test 442)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-442.img (`SHA-256 ea130a50d04aadb7c1217a7edb7a661134b7a5a8559df932a38822efb25ffa55`): kernel `t442` (`xnu-r5q-t442.macho`), root `test442` (`test442-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 441:
- **Live-verified D22 3-camera discovery (`camera_device.c` in `libredwood-media.dylib`):** Mapped the 3rd stream (`S5K3L6` ultra-wide) to `kFigCapturePortType_BackFacingTelephotoCamera` with D22-compatible `4032x3024` / `4224x2376` formats so `AVCaptureDeviceDiscoverySession` (`cam_list`) exposes all 3 devices live (`Back Camera` 20 formats, `Front Camera` 18 formats, `Back Telephoto Camera` 6 formats) while automatically toggling `kern.redwood.cam_main` (`1` ↔ `0`).
- **Live-verified `RedwoodMetal.bundle` capabilities (`redwoodmetal.m`):** Verified `metal_props on` + `metal_caps` passes to `caps: done` (`exit 0`, 0 asserts): `minimumLinearTextureAlignmentForPixelFormat:` returns `64` for formats `70`, `115`, and `252` (depth), and `respondsToSelector(gpuCoreCount)` returns `yes`.
- **Live-wired `"Receiver"` earpiece route attenuation (`redwood_sound.c`, `audio_plugin.c`):** `kern.redwood.spk_route=1` wakes `watch()` and applies `-16 dB` (`+128` eighths) safe earpiece attenuation live without stopping playback.

## The standard image before (2026-11-23, Test 441)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-441.img (`SHA-256 6a84c545e986339009fb918ee3cc823c75daa9f34f8fbdbbcea91d63e73db5f1`): kernel `t441` (`xnu-r5q-t441.macho`), root `test441` (`test441-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 439 (combining Tests 440–441):
- **Screen-off deep-sleep power gating (`MiniIOMobileFramebuffer.cpp`, `redwood_sec_ts.c`, `redwood_buttons.c`, `redwood_cpufreq.c`, `redwood_adsp.c`, `redwood_wifi.c`, `wifi/inject_bbd.c`):** Exported `redwood_display_asleep()` (`disp=on/off` in `kern.redwood.cpufreq_state`). While the OLED panel sleeps, touch polling backs off 10× (8 ms → 80 ms), buttons back off 4× (16 ms → 64 ms), CPUFreq tick calms to 500 ms, SLPI/ADSP GLINK and BT RX back off (25 ms / 10 ms / 50 ms), and `backboardd`'s sensor thread backs off 10× (5 ms → 50 ms). Verified live `disp=off` ↔ `disp=on` via lock button with 0% idle Silver CPU and 0 crashes.
- **Compass geomagnetic-shell normalization (`wifi/inject_bbd.c`):** Clamps uncalibrated magnetometer magnitude >58 µT onto the 48 µT Earth-field shell (preserving 3D direction) so CoreMotion/`CLCompassCalculator` does not trip its >65 µT magnetic-interference freeze (`cm_test` verified `48.0 µT`, 51 accel / 26 gyro / 42 mag / 51 device motion samples).
- **Native `0.5x` Ultra-Wide + `1x` Wide multi-lens camera (`camera_device.c` in `libredwood-media.dylib`):** Added 3rd stream `Back Ultra Wide Camera` (`kFigCapturePortType_BackFacingSuperWideCamera`, S5K3L6 0.5x) alongside `Back Camera` (IMX586 1x) and `Front Camera` (IMX616), automatically toggling `kern.redwood.cam_main` (`1` ↔ `0`) on stream start.
- **Native `"Receiver"` earpiece audio route (`audio_plugin.c` in `RedwoodAudio.driver`, `redwood_sound.c`):** Exposed `"Receiver"` (`OBJECT_RECEIVER` / `OBJECT_RECEIVER_OUTPUT`, `kAudioStreamTerminalTypeReceiver = 'rcvr'`) wired to `kern.redwood.spk_route` (`0` = Speaker, `1` = Receiver, `2` = Dual).
- **`RedwoodMetal.bundle` capability polish (`redwoodmetal.m`):** Added `minimumLinearTextureAlignmentForPixelFormat:` (64-byte Adreno 640 alignment, fixing the depth-format capability assert) and `gpuCoreCount` (2 SP cores for MetalFX queries).

## The standard image before (2026-11-23, Test 439)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-439.img (`SHA-256 d7a149653d6a228fe43a2657bf2151ae0feac14973005574eba89ac8ddcd1f3b`): kernel `t439` (`xnu-r5q-t439.macho`), root `test437` (`test437-root.apfs.new`), `phone-args.txt` with `fbscale=3`, `gps=1`, and `cpufreq=2`.
On top of Test 438:
- **Dynamic SM8150 OSM CPUFreq governor (`redwood_cpufreq.c`, `cpufreq=2` in `phone-args.txt`):** Scales all three SM8150 CPU clusters independently between minimum frequency/voltage (`row 0`: Silver `300 MHz`, Gold `710 MHz`, Prime `825 MHz`) when idle and full maximum clocks (`1785 MHz` / `2419 MHz` / `2841 MHz`) instantaneously under load, with live `kern.redwood.cpufreq_state` sysctl telemetry.
- **Kernel-wide idle wakeup elimination (>12,000 wakeups/sec → ~350 wakeups/sec):** Converted `redwood_haptic.c` (`r5q_serve`), `redwood_mic.c` (`for_ios`), `redwood_torch.c` (`serve`), and `redwood_wifi.c` (`gps_probe`) to 0-wakeup event-driven `msleep`/`wakeup`; converted `redwood_sound.c` (`feeder_thread` and `watch`) to event-driven `msleep` woken by DSP `ASM_DATA_EVENT_WRITE_DONE_V2` and `ios_sink`; added adaptive idle backoff + non-blocking FIFO drain in `redwood_wifi.c` (`bt_stage4`, 10,000/s → 50/s with 0 ms TX wake), `redwood_adsp.c` (`sl_thread` and `link_thread`), and `redwood_sec_ts.c` (`pc_touch_thread`). Hardware-verified with 0 CrashReporter logs, 0% idle Silver CPU, live `record_test 2` on both mics, `torchset`, `cm_test`, and `bt_test` (`CoreBluetooth: state 5`).

## The standard image before (2026-11-23, Test 438)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-438.img (`SHA-256 f1a69b4c78325cb07eb135db23288b968439e2e0d0fe8b94608904d2601dd906`): kernel `t438` (`xnu-r5q-t438.macho`), root `test437` (`test437-root.apfs.new`), `phone-args.txt` with `fbscale=3` and `gps=1`.
On top of Test 436 (combining Tests 437–438):
- **`CommCenter` + native on-demand hardware GNSS in standard boot (`build_root_app.sh`, `phone-args.txt`, `redwood_wifi.c`, `RedwoodBaseband.cpp`):** Re-enabled `CommCenter` in `ALLOW` (`bbkick=20` auto-engages the SM8150 modem cleanly, Settings → Cellular live, 0 resets, 0 crashes) and enabled `gps=1` by default in `phone-args.txt` with a desired-state `gps_pds_want_on` state machine (`gps=1` = on-demand `locationd` `QMI_PDS` `0x06` ↔ `QMI_LOC` `0x10` bridge without boot-time probe; `gps=2` = 20 s boot probe + bridge). Verified live `locationd` `QMI_PDS` handshake (`0x0000`, `0x0036`, `0x0020`, `0x0021`, `0x0031`, `0x003f`, `0x008b`) with 0 resets and 0 crashes.
- **Dynamic back camera lens switching (`redwood_camera.c`):** Added `kern.redwood.cam_main` sysctl (`1` = wide IMX586 1x, `0` = ultra-wide S5K3L6 0.5x) for live switching between camera sessions without rebooting.

## The standard image before (2026-11-23, Test 436)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-436.img (`SHA-256 a205a7f1a19b371a61ea8bab71effa7d3e4626b0507ddb28d198ae5d07d17597`): kernel `t436` (`xnu-r5q-t436.macho`), root `test436` (`test436-root.apfs.new`), `phone-args.txt` with native `@3x` Retina `fbscale=3`.
On top of Test 434 (combining Tests 435–436):
- **Native iOS ReplayKit screen recording (`wifi/inject_bbd.c`, `libredwood-media.dylib`, `com.apple.coremedia.plist`):** Hooked `CA::WindowServer::IOSurface::copy_surface` in `backboardd` for software RGB->NV12/32-bit virtual display writeback when `IOSurfaceAcceleratorCreate` is unavailable; enabled `wirelessdisplay_max_count = 1`; injected `libredwood-media.dylib` (OpenH264 `'avc1'` encoder + `+[RPPhotosUtility exportVideoToPhotosLibrary:completionHandler:]` DCIM/100APPLE export hook) into `replayd`. Control Center screen recordings encode H.264 High L4.2 + AAC 44.1 kHz stereo MP4s and copy them into `/private/var/mobile/Media/DCIM/100APPLE/IMG_XXXX.MP4`.
- **Dynamic multi-microphone source selection (`audio_plugin.c`, `redwood_mic.c`, `redwood_adsp.c`):** Added WCD9340 top sub-mic (`ADC3` + `MICB3`) alongside bottom main mic (`ADC1` + `MICB1`), exposed `'imic'` (Bottom), `'fmic'` (Front), and `'smic'` (Top) data sources on `RedwoodAudio`'s `OBJECT_CODEC_SOURCE` writing to `/dev/redwood-mic`, and added `kern.redwood.mic_sel` (0 = Bottom `ADC1`, 1 = Top `ADC3`, verified live with `record_test 2`).
- **Gated `gps=1` `QMI_LOC` (`0x10`) + `QMI_PDS` (`0x06`) GNSS bridge (`redwood_wifi.c`, `RedwoodBaseband.cpp`):** Behind boot-arg `gps=1`, probes the SM8150 modem's `QMI_LOC` (`0x10`) on QRTR (`0:82`, streaming 1 Hz `NMEA` / `SV_INFO` / `POS` indications with privacy-masked logging) and bridges `locationd`'s `QMI_PDS` (`0x06`) requests (`GET_GPS_SERVICE_VERSION` `0x36`, `GET_GPS_SERVICE_STATE` `0x20`, `SET_GPS_SERVICE_STATE` `0x21`) to `QMI_LOC` NMEA indications.

## The standard image before (2026-11-23, Test 434)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-434.img (`SHA-256 43be4a3347311b87fb5a3a32108b96025307341c3e84fccbb45a70c60ec2d6af`): kernel `t434` (`xnu-r5q-t434.macho`), root `test434` (`test434-root.apfs.new`), `phone-args.txt` with native `@3x` Retina `fbscale=3`.
On top of Test 433:
- **Camera `1080p60` / `720p60` (`Video-FES-60`), full-FOV lens shading, and MobileGestalt capabilities (`libredwood-media.dylib` in `test434-root.apfs.new`, `redwood_camera.c`):** Added `Video-FES-60` stream format (`FrontEndScalerOutputWidth=2304`, `FrontEndScalerOutputHeight=1296`, `4224x2376 @ 60`) with `1634`-line IMX586 60 fps readout, `isp_scale_nv12()` preview reuse from `PrimaryScaler`, `8x8` AE/AWB decimation at 60 fps, OpenH264 60 fps tuning (`LOW_COMPLEXITY`, CAVLC, `LEVEL_4_2`, `lf=2`, `scd=0`, `16/24 Mbps`), full-FOV `RWCISP02` lens shading (without `--crop`), and MobileGestalt `front-facing-camera`/`auto-focus-camera`/`camera-front-flash`.
- **Native `@3x` Retina display (`fbscale=3` in `phone-args.txt`):** `mgq` `1125x2436 @ 3`, edge-to-edge `1080x2400` panel rendering (`55.7–59.9 frames/s` on `fps_test` scrolling Settings, `2986 us` driver time, 0 GPU faults, 0 crash logs).
- **Sensors, power, and Bluetooth:** SLPI magnetometer calibration (`mag_cal 1`, `cm_rate` `m=50.0/s`), S2MU106 kernel fuel-gauge `phone.py charger` fix, OLED low brightness, and `BTServer` profile daemons enabled (`CoreBluetooth: state 5`, 0 BT crashes).

## The standard image before (2026-11-22, Test 433)

`r5q-install-boot-433.img`: kernel t433, root test431, `phone-args.txt` without `fbscale=3`.
On top of Test 430:
- **Camera 1080p H.264 video, fast NEON ISP, and camera controls** (`libredwood-media.dylib` in `test431-root.apfs.new`):
  NEON ISP (1080p ~138 fps), OpenH264 `'avc1'` VideoToolbox H.264 encoder with `CAMUserPreferenceCaptureEncodingBehavior = 1`, `ExposureBias`, `LockAE`/`UnlockAE`, `LockAWB`/`UnlockAWB`, non-blocking back `FlashMode`, and `VideoRecordingInProgress` -> `REDWOOD_CAMERA_OIS_MODE` (RUMBA S4 movie/still mode toggling: `cam: ois mode=1/0`, wifi/CAMERA-PLAN.md Tests 431-433).
- **WCD9340/SLIMbus multi-session mic reliability:** `dsb sy` barriers + `0xffffffff` BAM RX sentinel (Test 432) and symmetric SLIMbus/AFE startup-teardown + `DISCONNECT_PORT` `RECONFIG_NOW` commit (Test 433), fixing `-91 dB` silent mic captures under CSI DMA load and across repeated sessions (6/6 verified).
- **`phone.py` unstick fix:** resolved shadowed `r5qd` helper (`r5qd_quiet`) and `Write to device failed` retry so `unstick_usb()` triggers cleanly.

## The standard image before (2026-11-21, Test 430)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-430.img: kernel t430, root test415, `phone-args.txt` unchanged.
On top of Test 429: **the main camera's optical stabiliser** (RUMBA S4 on GPIO33/34) started in still mode with each
session (wifi/CAMERA-PLAN.md, Test 430). `cam_ois=0` turns it off.

## The standard image before (2026-11-21, Test 429)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-429.img: kernel t429, root test415, `phone-args.txt` unchanged.
On top of Test 427: **autofocus on the main camera**: its lens motor (on GPIO33/34, CCI 1 master 1) driven by iOS's
contrast autofocus through the camera device (wifi/CAMERA-PLAN.md, Tests 428-429). `cam_af=0` turns it off.

## The standard image before (2026-11-21, Test 427)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-427.img: kernel t427, root test415, `phone-args.txt` unchanged.
On top of Test 424:
- **The back camera sees in dim light:** the IMX586's custom1/custom2 supplies (S2MPB03 LDO3s) at 1.8 V; they were at
  0.7 V (a wrong voltage code since Test 342), which made the sensor clip everything below ~400 counts to black until
  it warmed (wifi/CAMERA-PLAN.md, Tests 426-427).
- Tools: `cam_verify=1` reads every sensor table register back; `cam586_mclk=24` runs the IMX586 on a 24 MHz MCLK
  (camcc PLL2); `wifi/camgate.py` measures raw frames.

## The standard image before (2026-11-21, Test 424)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-424.img: kernel t424, root test415, and `mic=7
mic_rec=4` added to `phone-args.txt` (before: port/build/phone-args.before-t424.txt). On top of Test 418:
- **The microphone works in iOS** ("iPhone Microphone"): the WCD9340 on SLIMbus, its capture path, channel 134 to the
  DSP's SLIMBUS_0_TX, /dev/redwood-mic for the audio plug-in (wifi/WIFI-PLAN.md, Tests 419-424). Gains: boot-args
  `mic_gain` (ADC1, default 8) and `mic_dgain` (dB, default 0). `record_test SECS [vibrate AT]` (phone tools)
  records as an app does and prints the levels.

## The standard image before (2026-11-21, Test 418)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-418.img: kernel t418, root test415, `phone-args.txt`
unchanged. On top of Test 416:
- **Wi-Fi joins again by itself** when a join is refused or the access point sends us away. The tries are 15 s, 30 s,
  ... at most 5 min apart. An access point that refused us is passed over for 5 minutes when another of the same
  network is heard. Switching Wi-Fi off, or iOS asking to leave, stops it. Boot-arg `wifi_rejoin=0` turns it off.
- `kern.redwood.wifi_drop` (1, or 2 to also pass over the current access point) leaves the network to try the rejoin:
  `r5q_sysctl kern.redwood.wifi_drop 1`.

## The standard image before (2026-11-21, Test 416)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-416.img: kernel t416, root test415, `phone-args.txt`
unchanged. On top of Test 413:
- **The compass works in the Compass app** (root test415): backboardd's virtual motion services keep the properties
  iOS sets on them, so locationd and CoreMotion no longer give up on the compass. After a figure-of-eight the
  app showed a heading (278° W). It is still jumpy: the hard-iron offset has drifted (wifi/WIFI-PLAN.md, Tests 415-416).
- **The IMX586's PLLs inside its own limits** (kernel t414+, `cam586_pre`, default 2): no visible change to the
  picture (wifi/CAMERA-PLAN.md).
- **The Wi-Fi join** ignores a deauthentication or disassociation the access point sends to another station
  (kernel t416).

## The standard image before (2026-11-20, Test 413)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-413.img: kernel t413, root test413, `phone-args.txt`
unchanged. On top of Test 412:
- **The kernel leak:** the camera's device mappings are kept and reused (`r5q_io_map_kept` in redwood_sec_ts.c).
  Each session leaked ~1-2 MB of the I/O map, and a few dozen sessions in one run panicked in io_map. 47 sessions in
  one run were fine.
- **The library from source:** libredwood-media.dylib is built from source (`pipeline/build_init.sh`). Its
  auto-exposure uses the whole ~33 ms frame before adding gain, never sits at gain code 0 (1x), and waits two frames
  after each change (wifi/CAMERA-PLAN.md, "The back camera's exposure in the Camera app").
- **Not yet seen in a lit room:** the room was dark by the time Test 413 ran. Before it:

## The standard image (2026-11-20, Test 412)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-412.img: kernel t412, root test411, and `ufs_var=1
var_hfs=1` added to `phone-args.txt`. `/private/var` is now a 100 GB HFS+ volume on the flash (darwin past 16 GiB),
mounted by init in place of the 1.6 GB RAM filesystem. It keeps itself across boots; var_store only syncs it.
Settings shows "3.7 GB of 103 GB Used", and the "iPhone Storage Full" alert is gone (wifi/STORAGE-PLAN.md, Tests
406-412).

New tools:
- `pipeline/panic_sym.py`: names a panic's backtrace;
- `pipeline/build_init.sh` and `pipeline/macho_static.py`: init and var_store built from source, static, into the
  root;
- `dfq rm FILE`.

### The way back

Without `var_hfs=1`, init uses the RAM filesystem and var_store's archive again. That archive is the state of the
last RAM-filesystem boot: Test 410. Before this image:

## The standard image (2026-11-20, Test 404)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-404.img: kernel t404, root test401. The back camera
(IMX586) gets its pixel PLL and output PLL right for the 19.2 MHz clock, line length 8000, and black level 64 for iOS:
the white flashing and the ramp with column stripes are gone (wifi/CAMERA-PLAN.md, "The lit-scene look", Tests
402-404). `phone.py cycle` now retries a flash refused with "a kernel is running". Before it:

## The standard image before (2026-11-20, Test 401)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-401.img: kernel t398, root test401 (= test400 +
libr5q-imagent.dylib in imagent: no "iMessage was signed out" "!" on Messages' icon; wifi/WIFI-PLAN.md). Before it:

## The standard image before (2026-11-20, Test 400)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-400.img: kernel t398, root test400 (= test395 + the Metal
driver in /redwood/lib + libr5q-poster.dylib in PosterBoard and the poster extensions: **the wallpapers show** on the
lock screen and home screen; wifi/METAL-FEASIBILITY.md, Tests 399-400). Unlock with `phone.py unlock`. Before it:

## The standard image before (2026-11-20, Test 398)

`port/build/r5q-install-boot-standard.img` = r5q-install-boot-398.img: kernel t398 (the IMX586's full frame at line
length 10752: its black level right again; wifi/CAMERA-PLAN.md, Tests 396-398), root test395. Before it, Test 395:

## The standard image before (2026-11-20, Test 395)

`port/build/r5q-install-boot-standard.img` is now a copy of r5q-install-boot-395.img: kernel t394, root test395. The
root is test370's tree with one change: `libr5q-media.dylib` is injected into mediaserverd and turns off the
Camera's audio silence filling (wifi/MEMORY-PLAN.md). It was made from the build tree with the Metal test files
removed and test370's CommCenter job, then compared with test370 file by file; build_root_app.sh now builds the
library too. Before that, kernel t394 with root test370 (Test 394):
`phone-args.txt` ends with `md_flash=90`. Ninety seconds after the flash driver is up, md0 (the 2.3 GB iOS system image
the firmware copies into RAM) is checked against USERDATA and read from the flash from then on, and its RAM goes back to
the system: about 27% → 65% memory free (wifi/MEMORY-PLAN.md). `kern.redwood.md_flash` reads 1 once it has happened.
Kernel t394 also has `kern.redwood.wired` (`wiredq`). The set before: port/build/phone-args.before-t394.txt; the image
before: port/build/r5q-install-boot-standard.before-t394.img (the Test 391 one below).

## The standard image before (2026-11-20, Test 391)

`port/build/r5q-install-boot-standard.img` (a copy of r5q-install-boot-391.img: kernel t389, root test370 -- CommCenter
allowed, the baseband firmware tree bound -- and the loader that makes the `baseband` and camera `isp` nodes in test
builds). `phone-args.txt` now ends with `bbstub=1 bbnode=1 bbfake=1 bbkick=20`: iOS's cellular stack comes up on the
modem by itself (wifi/CELLULAR-PLAN.md, Tests 389-391). The set before: port/build/phone-args.before-t391.txt; the
image before: r5q-install-boot-363.img. Back to the standard: `python phone.py cycle port/build/r5q-install-boot-standard.img 90 std`.
After changing device-tree nodes, MobileGestalt's cache in /private/var keeps the old answers: delete
`/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`,
run `/redwood/bin/var_store save`, reboot.
