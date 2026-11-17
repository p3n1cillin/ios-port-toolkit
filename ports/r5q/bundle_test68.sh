#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test68.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 68 --firmware-number 68 --mode diagnostic \
  --notes "Test 67 (user): picture small in the top left, old screen around it, touch offset (taps on the blue open apps); the two main home pages smooth, the outer ones slower. Log: pipes 0x1 (vig0, ctl0), scaler 0x1004 set up (750 -> 1080), but at the first frame panelAlive() read GCC disp_ahb 0x80000003 (HW_CTL, idle-gated: CLK_OFF) as lost and gave up the flip; frames 0.73 ms each in the driver; no memory under 4 GB for a third buffer. Test 68 (one big test, user: get as much working as possible): kernel = flip with HW_CTL branches not counted as lost and the third buffer in Display Reserved (0x9C400000/0x9CE00000/0x9D800000); fbscale=2; battery: S2MU106 fuel gauge read (never written) by the touch thread on GPIO57/58 every 5 s -> iOS power source; buttons: r5q keys (PM8150 PON KPDPWR/RESIN, PM8150L GPIO12 volume up), reads only; run 900 s (loader R5Q_LAB_TIMED=900; five fingers held 1.5 s end it sooner)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 67 (2026-11-16 22:45)..........." | grep -E '"bundle"|"image"'
