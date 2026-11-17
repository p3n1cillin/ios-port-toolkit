#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test41.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 41 --firmware-number 41 --mode diagnostic \
  --notes "Test 40 (DEBUG full firmware) showed every APRIORI Qualcomm driver loading and the hang in RamManagerDxe: the r5q map omitted the live DT no-map secure carve-outs (hyp_mem 0x85700000, xbl_aop_mem, removed_regions 0x86200000-0x8B700000, pil_video tail, cdsp_sec, qseecom_region, modem_shared tail, ss_plog, ramoops, sec_debug_autocomment), so RamManagerDxe handed them out as RAM. Test 41 = Test 40 with those reserved and never mapped (HobOnlyNoCacheSetting, as other Samsung Silicium ports do for TZ memory). Same probe loader; DEBUG trail to panel and RAM ring." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 40 showed the hang in RamManagerDxe (2026-11-16 ~18:15)." | grep -E '"bundle"|"image"'
