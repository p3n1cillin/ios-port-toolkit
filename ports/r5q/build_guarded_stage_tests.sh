#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
# Build the Tests 25-28 loaders: Tests 20-23 with the APSS watchdog armed before the MMU-off step.
set -eu
R=${R5Q_PORT}
cd /tmp
# 25=stage2(=20 guarded) 26=stage3(=21) 27=stage4(=22) 28=stage0(=23 full return)
for spec in "25 2" "26 3" "27 4" "28 0"; do
	set -- $spec
	echo "test $1 (guarded) stage $2: $(python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test$1-loader.efi --stage-test $1 $2 | tail -1)"
done
