#!/bin/bash
# cell_engage.sh [SECS] (2026-11-20, Tests 377-379): on a running cellular test image (bbstub=1 bbfake=1), put the
# logging preferences in (private data, Debug: port/build/logprefs), inject wifi/inject_abm.c's libr5q-abm.dylib into
# CommCenter (it engages the baseband manager's IPC router by hand 20 s after CommCenter starts), restart CommCenter,
# and follow the kernel log over USB for SECS (default 70) into port/build/live-cell.txt -- over USB, because Wi-Fi
# (r5qd) has stalled for a minute around engage failures. Prints the transport's lines.
# Build the library first: see wifi/inject_abm.c (clang ... -dynamiclib -o port/build/phone-tools/libr5q-abm.dylib).
set -e
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$R"
SECS=${1:-70}
c() { MSYS_NO_PATHCONV=1 timeout 60 python wifi/r5qd_client.py "$@"; }
for s in com.apple.telephony.abm com.apple.CommCenter com.apple.telephony.bb; do
	c put port/build/logprefs/$s.plist /var/r5q/$s.plist >/dev/null
	c run /var/r5q/bin/r5qfs cp /var/r5q/$s.plist /Library/Preferences/Logging/Subsystems/$s.plist >/dev/null
done
c put port/build/phone-tools/libr5q-abm.dylib /var/r5q/lib/libr5q-abm.dylib >/dev/null
G=$(c inject | head -1 | tr -d '\r')
echo "$G" | grep -q libr5q-abm || c inject "$G;/var/r5q/lib/libr5q-abm.dylib CommCenter" >/dev/null
c kill CommCenter
timeout $((SECS + 30)) python phone.py live "$SECS" > port/build/live-cell.txt 2>&1 || true
grep -a "RedwoodBaseband" port/build/live-cell.txt | grep -a "PCI\|qmux\|completed" | grep -v published |
	sed 's/call [0-9]* //' | awk '!s[$0]++' | head -${LINES_SHOWN:-60} | cut -c1-200
