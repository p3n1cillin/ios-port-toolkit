# The test pipeline (from Test 140 on)

How a test image is made and run now. The early tests used `generator/` and the per-test `genNN.py` files; these
scripts are what has been used since. They were kept outside the tree until Test 243.

| Step | Command (Git Bash on Windows, WSL for Linux steps) | Makes |
| --- | --- | --- |
| Kernel | `wsl -e bash -lc "cd /tmp && python3 -I $R/build_xnu_linux.py $R $R/port/build/xnu-r5q-tN.macho"` (R = this port from WSL) | `port/build/xnu-r5q-tN.macho` (+ `.build.log`, `.receipt.json`) |
| Root volume | `wsl -e bash -c "bash $R/pipeline/build_root_app.sh N"` | `port/build/testN-root.apfs.new`: the donor userland's boot volume with our injected libraries (`wifi/inject_app.c`, `wifi/inject_bbd.c`), r5qd, the launchd jobs |
| Test image | `cp port/xnuloader/build-testA.sh port/xnuloader/build-testB.sh; bash pipeline/mktest.sh A B xnu-r5q-tK.macho testX-root.apfs.new "label"` | `pipeline/gen/genB.py` from `genA.py`, then the loader, shim and the 64 MiB `port/build/r5q-install-boot-B.img` |
| Run | `python phone.py cycle port/build/r5q-install-boot-B.img SECS [boot-argsÃ¢â‚¬Â¦]` | flash, restart, follow the log into `port/build/cycles/` |
| Live changes | `python wifi/r5qd_client.py reload LIB PROGRAM` / `put` / `kill` / `run` | no reboot (injected libraries, /var files, test tools) |

Notes:

- Do **not** `export MSYS_NO_PATHCONV=1` for `mktest.sh`; do set it for `phone.py sh` and `r5qd_client.py`.
- `phone.py cycle Ã¢â‚¬Â¦ ARGS`: plain `key=val` ARGS **replace** the stored boot-args. Use `+key=val`, `-key` or `std`
  for changes to the standard set in `phone-args.txt` (below).
- Kernel builds take about 10 minutes. Run them in the background.
- Every test's purpose and result goes into `wifi/WIFI-PLAN.md` as it is run.

## Phone helpers added 2026-11-19 (to save time)

| Command | Does |
| --- | --- |
| `phone.py cycle IMG SECS +key=val -key` | boot-args as **changes to the standard set** in `phone-args.txt` (`std` = the standard set as is). No more retyping the whole set; a plain `key=val` list still replaces it as before |
| `phone.py cycle Ã¢â‚¬Â¦` (any) | if the run **ends the system**, says so with the 15 lines before it |
| `phone.py rescue` | back to a running iOS from anything: a stuck USB link ("no link") is cleared by restarting the phone from iOS over Wi-Fi (`r5q_reboot`); then the listening window, where the **log slots are saved** (the crash's record, before a new run overwrites it), then iOS with the standard boot-args |
| `bash pipeline/build_phone_tools.sh` (WSL) then `phone.py tools` | every on-phone tool built into `port/build/phone-tools/` and put into `/var/r5q/bin` in one go (after a crash `/var` may not have kept them) |
| `bash pipeline/test.sh B "label" [+argsÃ¢â‚¬Â¦]` | a whole test in one command: the kernel built (or `KERNEL=xnu-r5q-tNNN.macho` reused), the root (newest, or `ROOT=build`), the image from test B-1, the run (`FOLLOW=` seconds) |

`phone-args.txt` is the standard boot-arg set. Change it when a feature's arg becomes part of every boot.

## The standard image (2026-11-21, Test 427)

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
the system: about 27% Ã¢â€ â€™ 65% memory free (wifi/MEMORY-PLAN.md). `kern.redwood.md_flash` reads 1 once it has happened.
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
