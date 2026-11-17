#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
R5Q_SCRATCH=${R5Q_SCRATCH:-${TMPDIR:-/tmp}}
R=${R5Q_PORT}
S=${R5Q_SCRATCH}
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test30-loader.efi --test30 | tail -1
for p in test16 test18 test29; do python3 -I $R/build_loader_linux.py $R/port/xnuloader $S/n-$p.efi --$p > /dev/null; done
python3 -I $R/build_loader_linux.py $R/port/xnuloader $S/n-test24.efi --stage-test 24 5 > /dev/null
cd $R && python3 -I build_test30_shim.py $R/port && python3 run_test30_checks.py $R 2>&1 | grep -E '"test"|"result"|"cases"|Error|assert'
