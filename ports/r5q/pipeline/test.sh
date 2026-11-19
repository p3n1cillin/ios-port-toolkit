#!/bin/bash
# test.sh B "label" [CYCLE ARGS...]: one command for a whole test (Git Bash on Windows, WSL for the builds).
#   1. the kernel: KERNEL=build (default) builds xnu-r5q-tB.macho from the overlay; KERNEL=xnu-r5q-tNNN.macho reuses one
#   2. the root: ROOT=testNNN-root.apfs.new (default: the newest in port/build); ROOT=build makes testB-root.apfs.new
#   3. the test image from test B-1 (pipeline/mktest.sh), then the run (phone.py cycle, FOLLOW seconds, default 120)
#      with CYCLE ARGS (e.g. +bt=4 -torch, or std; none = the stored boot-args as they are)
# Example: bash pipeline/test.sh 250 "mic: SPI probe" +mic=8
set -e
B=$1; LABEL=$2; shift 2 || { echo 'usage: test.sh B "label" [cycle args...]'; exit 2; }
R=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
# A: the newest earlier test with a generator (pipeline/gen/genA.py); tests done without an image (a hot load) have none
A=$(ls "$R"/pipeline/gen/gen*.py 2>/dev/null | sed -E 's/.*gen([0-9]+)\.py/\1/' | awk -v b="$B" '$1 < b' | sort -n | tail -1)
[ -n "$A" ] || { echo "no earlier generator in pipeline/gen"; exit 1; }
RL=${R5Q_PORT_WSL:-$(echo "$R" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')}
cd "$R"
KERNEL=${KERNEL:-build}
if [ "$KERNEL" = build ]; then
	echo "== kernel t$B (about 10 minutes)"
	wsl -e bash -lc "cd /tmp && python3 -I $RL/build_xnu_linux.py $RL $RL/port/build/xnu-r5q-t$B.macho > /tmp/t$B.log 2>&1; grep '\"status\"' /tmp/t$B.log; grep -a ' error:' $RL/port/build/xnu-r5q-t$B.build.log | head -5" 2>&1 | grep -v 'Nmap'
	KERNEL=xnu-r5q-t$B.macho
fi
[ -f "port/build/$KERNEL" ] || { echo "no kernel port/build/$KERNEL"; exit 1; }
ROOT=${ROOT:-$(ls -t port/build/test*-root.apfs.new 2>/dev/null | head -1 | xargs -n1 basename)}
if [ "$ROOT" = build ]; then
	echo "== root test$B"
	wsl -e bash -c "bash $RL/pipeline/build_root_app.sh $B" 2>&1 | grep -v 'Nmap' | tail -2
	ROOT=test$B-root.apfs.new
fi
echo "== image: test $B from $A, kernel $KERNEL, root $ROOT"
cp "port/xnuloader/build-test$A.sh" "port/xnuloader/build-test$B.sh"
bash pipeline/mktest.sh "$A" "$B" "$KERNEL" "$ROOT" "$LABEL" 2>&1 | tail -2
echo "== run (${FOLLOW:-120} s)"
python phone.py cycle "port/build/r5q-install-boot-$B.img" "${FOLLOW:-120}" "$@"
