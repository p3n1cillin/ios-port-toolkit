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
