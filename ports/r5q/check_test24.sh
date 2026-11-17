#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
R=${R5Q_PORT}
cd "$R" && python3 -I build_test24_shim.py "$R/port" && python3 run_test24_checks.py "$R" 2>&1 | tail -40
