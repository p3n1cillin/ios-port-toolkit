#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test58.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 58 --firmware-number 58 --mode diagnostic \
  --notes "Display alignment. Test 57 (user): the pattern looked the same in all three refresh modes (autorefresh off + one clean frame, every 2nd TE, every TE): ~580 rows of each frame never reach the panel. The stock DisplayDxe config for AMB667UM01 (strings): command mode, autorefresh on with FrameNumDiv 2, TE check with the dedicated TE pin, start position 2400, init 2400, 1176.5 Mbit/s; our MDSS matches. Untested piece: the TE pin itself (live DT: gpio8, function mdp_vsync, pull-down, 2 mA; SM8150 NORTH tile, function 1). The full firmware runs Qualcomm's TLMM driver; the minimal one (smaller shift) does not. Test 58 = Test 57 + R5qTePin: logs gpio8's config and counts its TE edges over 50 ms, sets it to mdp_vsync if not, counts again; then the resync and the three-mode pattern." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 57 (2026-11-16 21:12)........." | grep -E '"bundle"|"image"'
