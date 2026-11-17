#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test61.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 61 --firmware-number 61 --mode diagnostic \
  --notes "Display alignment. Test 60 log: the panel window commands never went out (13: NO DMA DONE in 20 ms). Cause found against upstream dsi.xml: Linux's msm driver shifts every DSI 6G register by +4 (DSI_6G_REG_SHIFT); our dump matches xml+4 everywhere (CTRL 0x04, CMD_DMA_CTRL 0x3C, DMA_BASE 0x48, TRIG_CTRL 0x84, TRIG_DMA 0x90, INTR_CTRL 0x110) but the reset was written to 0x114 instead of RESET 0x118 (so Tests 56-60 never reset the DSI), and the command DMA had its done-mask (INTR_CTRL bit 1) off and used our own buffer. Test 61: RESET at 0x118, MASK_CMD_DMA_DONE set, the bootloader's command buffer 0xFF30A000 (claimed; fallback 0x9E200000); then 13 / 2A / 2B, the clean frame, the ruler." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 60 (2026-11-16 21:38)............" | grep -E '"bundle"|"image"'
