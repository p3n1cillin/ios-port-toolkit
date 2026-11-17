#!/bin/bash
# Build the Tests 19-23 loaders (return-path stage bisection), EMULATION-CHECKED ONLY until bundled.
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd /tmp
for spec in ${SPECS:-"19 1" "20 2" "21 3" "22 4" "23 0" "24 5"}; do
	set -- $spec
	echo "test $1 stage $2: $(python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test$1-loader.efi --stage-test $1 $2 | tail -1)"
done
