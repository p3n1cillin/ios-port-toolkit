#!/bin/bash
# Emulator-check the Tests 19-23 roundtrip and shim cases. EMULATION ONLY.
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
for n in 19 20 21 22 23; do
	out=$(python3 run_test${n}_checks.py "$R" 2>&1)
	echo "== test $n: $(echo "$out" | grep -o '"result": "[a-z]*"' | tr '\n' ' ') $(echo "$out" | grep -o '"cases": [0-9]*' | tr '\n' ' ')"
	echo "$out" | grep -v '^[{} ]' | grep -iv '"' | tail -6
done
