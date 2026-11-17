#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
S=/mnt/c/Users/user/AppData/Local/Temp/claude/860c44f7071fb004/a346f0bb-bd54-4c7d-a9bc-775292c34ce6/scratchpad
cd /tmp
python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test31-loader.efi --test31 | tail -1
for p in test16 test18 test29 test30; do python3 -I $R/build_loader_linux.py $R/port/xnuloader $S/m-$p.efi --$p > /dev/null; done
cd $R && python3 -I build_test31_shim.py $R/port && python3 run_test31_checks.py $R 2>&1 | grep -E '"test"|"result"|"cases"|Error|assert'
