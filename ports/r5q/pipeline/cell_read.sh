#!/bin/bash
# cell_read.sh N (2026-11-20, Tests 381-386): after pipeline/cell_engage.sh, pull the phone's unified log into
# port/build/oslog-N (uuidtext copied from the newest earlier oslog-* first, so only what changed comes over Wi-Fi),
# read CommCenter's entries with toolkit/tools/ios/tracev3.py into port/build/oslog-N/commcenter.txt, and print the
# cellular summary: parsed non-CTL responses, boot and IPC states, SIM and registration, resets and their reasons.
# The pulled logs hold private device data (IMEI, addresses): local only, never pushed.
set -e
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$R"
N=${1:?usage: cell_read.sh N}
D=port/build/oslog-$N
PREV=$(ls -td port/build/oslog-*/uuidtext 2>/dev/null | head -1)
rm -rf "$D"; mkdir -p "$D"
[ -n "$PREV" ] && cp -r "$PREV" "$D/"
export MSYS_NO_PATHCONV=1
timeout 900 python wifi/r5q_pull.py /private/var/db/diagnostics "$D/diagnostics" | tail -1
timeout 900 python wifi/r5q_pull.py /private/var/db/uuidtext "$D/uuidtext" | tail -1
PID=$(timeout 60 python wifi/r5qd_client.py run /bin/ps -Ao pid,comm | grep "CommCenter$" | awk '{print $1}')
echo "CommCenter: $PID"
RL=$(echo "$R" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')
wsl -e bash -c "cd /tmp && python3 -I $RL/../../toolkit/tools/ios/tracev3.py $RL/$D --pid $PID > $RL/$D/commcenter.txt"
F=$D/commcenter.txt
echo "non-CTL QMI responses/indications parsed: $(grep -a 'Resp MsgId\|Ind MsgId' $F | grep -avc CTL)"
grep -a 'boot state changing\|Reset requested\|IPC state changing\|registration status from\|SIM state is\|Settled physical SIM\|firmware version\|corrupt' $F |
	grep -av IMEI | cut -c1-220 | awk '{k=$0; sub(/^ *[0-9.]+ +[0-9]+ /,"",k); if(!s[k]++) print}' | head -${LINES_SHOWN:-30}
