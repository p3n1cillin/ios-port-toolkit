#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R" && python3 -I build_test24_shim.py "$R/port" && python3 run_test24_checks.py "$R" 2>&1 | tail -40
