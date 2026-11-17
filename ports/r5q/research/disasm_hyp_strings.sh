#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Print strings at addresses used near the TVM enable, and the caller of 0x8576ecf8. Data only.
cd ${R5Q_WORK}/research-fw || exit 1
str() { dd if=hyp-seg3.bin bs=1 skip=$(( 0x$1 - 0x85740000 )) count=120 status=none | tr '\0' '\n' | head -1; }
for a in 8578ed49 85778cb7 8577897d 85778a04; do printf '%s: %s\n' "$a" "$(str $a)"; done
echo "== all string refs inside 0x8576ecf8..0x8576fa80 (adrp+add pairs into the 0x8578e000/0x85778000 pages)"
awk '/^ *8576ecf8:/{on=1} /^ *8576fa80:/{on=0} on' hyp-seg3.dis | grep -E 'adrp\s+x[0-9]+, 0x857(78|8e)000' -A1 | grep -o 'add\s.*#0x[0-9a-f]*' | sort -u | head -30 > /tmp/refs.txt
awk '/^ *8576ecf8:/{on=1} /^ *8576fa80:/{on=0} on' hyp-seg3.dis | grep -E 'adrp\s+x[0-9]+, 0x857(78|8e)000' -A1 | paste - - - | awk '{for(i=1;i<=NF;i++){if($i ~ /^0x857(78|8e)000$/)pg=$i; if($i ~ /^#0x/)off=$i} gsub(/[#,]/,"",off); gsub(/,/,"",pg); if(pg!=""&&off!="") printf "%x\n", strtonum(pg)+strtonum(off)}' | sort -u | head -30 | while read a; do printf '  %s: %s\n' "$a" "$(str $a)"; done
echo "== caller context at 0x8576cde4"
n=$(grep -n "^ *8576cde4:" hyp-seg3.dis | cut -d: -f1); sed -n "$((n-30)),$((n+3))p" hyp-seg3.dis | cut -f1,3- | cut -c1-90
