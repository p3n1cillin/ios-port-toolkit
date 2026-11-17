#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test64-loader.efi --test64 | tail -1
cd $R && python3 -I build_test64_shim.py $R/port && python3 run_test64_checks.py $R 2>&1 | grep -E '"test"|"result"|"case"|Error|assert'
