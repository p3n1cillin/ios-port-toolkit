#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
R=${R5Q_PORT}
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test169-loader.efi --test169 | tail -1
cd $R && python3 -I build_test169_shim.py $R/port && python3 run_test169_checks.py $R 2>&1 | grep -E '"test"|"result"|"case"|Error|assert'
