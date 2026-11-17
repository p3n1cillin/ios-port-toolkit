#!/bin/bash
# Summarize a QEMU iOS boot log (EMULATION ONLY). usage: qemu_ios_log_summary.sh LOG
L=$1
echo "lines: $(wc -l < "$L")   qemu running: $(pgrep -c qemu-system || true)"
echo "--- spawned programs (launchd)"
grep -a -o 'Successfully spawned [^ ]*' "$L" | sort | uniq -c | sort -rn | head -40
echo "--- SpringBoard / backboardd mentions"
grep -a -c 'SpringBoard' "$L"; grep -a -c 'backboardd' "$L"
echo "--- trouble"
grep -a -i -E 'panic|crashed|exited due to|exited with exit code [1-9]|abort|watchdog|deny|Sandbox' "$L" | cut -c1-180 | sort | uniq -c | sort -rn | head -25
echo "--- tail"
tail -15 "$L" | cut -c1-220
