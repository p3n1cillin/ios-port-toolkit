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

## The standard image (2026-11-20, Test 395)

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
