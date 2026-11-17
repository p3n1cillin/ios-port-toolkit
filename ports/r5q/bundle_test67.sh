#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test67.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 67 --firmware-number 67 --mode diagnostic \
  --notes "Test 66 (user): time correct, Settings opens, still glitchy/a bit laggy, screen tearing, janky. Log: cpufreq took all three clusters to their top rows (1785/2419/2841 MHz); frames 10-22 a second, 5 ms each in the driver scaling 1125x2436 down to 1080x2400 by the processors, into the one buffer the panel reads 30 times a second (the flip was y2q-only). Test 67 = Test 66 + kernel MiniIOMobileFramebuffer overlay: the flip on r5q (front 0x9C400000, back 0x9D200000 in Display Reserved, a third from kernel memory under 4 GB; pipe address + CTL flush taken at a frame's start; autorefresh every TE), and cmdline fbscale=2 (750x1624 drawn, 2.25x fewer pixels, enlarged by the vig pipe's scaler)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 66 (2026-11-16 22:24)..........." | grep -E '"bundle"|"image"'
