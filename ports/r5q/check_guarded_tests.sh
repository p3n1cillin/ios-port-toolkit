#!/bin/bash
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
for n in 25 26 27 28; do
	python3 -I build_test${n}_shim.py "$R/port" >/dev/null
	out=$(python3 run_test${n}_checks.py "$R" 2>&1)
	echo "== test $n: $(echo "$out" | grep -o '"result": "[a-z]*"' | tr '\n' ' ')$(echo "$out" | grep -iE 'Error|assert' | head -2)"
done
