#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
# Build the Tests 19-23 loaders (return-path stage bisection), EMULATION-CHECKED ONLY until bundled.
set -eu
R=${R5Q_PORT}
cd /tmp
for spec in ${SPECS:-"19 1" "20 2" "21 3" "22 4" "23 0" "24 5"}; do
	set -- $spec
	echo "test $1 stage $2: $(python3 -I $R/build_loader_linux.py $R/port/xnuloader $R/port/build/r5q-test$1-loader.efi --stage-test $1 $2 | tail -1)"
done
