#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test61-loader.efi --test61 | tail -1
cd $R && python3 -I build_test61_shim.py $R/port && python3 run_test61_checks.py $R 2>&1 | grep -E '"test"|"result"|"case"|Error|assert'
