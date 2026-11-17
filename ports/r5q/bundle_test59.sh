#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test59.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 59 --firmware-number 59 --mode diagnostic \
  --notes "Display alignment. Test 58: the TE pin is right (gpio8 cfg 4 = mdp_vsync, 6 edges in 50 ms = 3 TE pulses at 60 Hz), so TE is not the cause. Remaining question: does the panel drop the first ~580 rows of each transfer, or does the pipe's fetch (through the SMMU) start ~580 rows past our buffer, the panel's bottom then showing memory after the framebuffer (the bootloader's splash)? Test 59: after the TE check and the DSI resync, a row ruler in the framebuffer (white line every 100 rows, 36 px per 100 rows long) and magenta in the 10 MB after the framebuffer (Display Reserved), autorefresh every 2nd TE, held 20 s." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 58 (2026-11-16 21:20).........." | grep -E '"bundle"|"image"'
