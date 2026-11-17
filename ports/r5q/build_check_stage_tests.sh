#!/bin/bash
# Assemble and emulator-check the Tests 19-23 shim images. EMULATION ONLY; nothing is flashed.
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
for n in 19 20 21 22 23; do
	python3 -I build_test${n}_shim.py "$R/port"
	python3 run_test${n}_checks.py "$R" | python3 -c "import sys,json; t=sys.stdin.read(); print('test $n checks:', [ (d['test'], d['result'], d['cases']) for d in json.loads('['+t.replace('}\n{','},{')+']') ])"
done
