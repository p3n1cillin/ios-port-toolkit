#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test56-loader.efi --test56 | tail -1
cd $R && python3 -I build_test56_shim.py $R/port && python3 run_test56_checks.py $R 2>&1 | grep -E '"test"|"result"|"case"|Error|assert'
