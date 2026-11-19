#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
R=${R5Q_PORT}
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test272-loader.efi --test272 | tail -1
cd $R && python3 -I build_test272_shim.py $R/port && python3 run_test272_checks.py $R 2>&1 | grep -E '"test"|"result"|"case"|Error|assert'
