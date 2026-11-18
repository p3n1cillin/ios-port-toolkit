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
- `phone.py cycle … ARGS` **replaces** the stored boot-args with ARGS. Pass the whole set (see WIFI-PLAN for the
  current one), not just the new key.
- Kernel builds take about 10 minutes. Run them in the background.
- Every test's purpose and result goes into `wifi/WIFI-PLAN.md` as it is run.
