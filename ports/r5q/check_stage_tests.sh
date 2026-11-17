#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
# Emulator-check the Tests 19-23 roundtrip and shim cases. EMULATION ONLY.
R=${R5Q_PORT}
cd "$R"
for n in 19 20 21 22 23; do
	out=$(python3 run_test${n}_checks.py "$R" 2>&1)
	echo "== test $n: $(echo "$out" | grep -o '"result": "[a-z]*"' | tr '\n' ' ') $(echo "$out" | grep -o '"cases": [0-9]*' | tr '\n' ' ')"
	echo "$out" | grep -v '^[{} ]' | grep -iv '"' | tail -6
done
