#!/bin/bash
# Locate the stock ABL code that shuts caches/MMU down before jumping to Linux (data only, never run).
cd /var/tmp/codex-ios-r5q/work || exit 1
OD=aarch64-linux-gnu-objdump
for f in abl_pe_ac.efi abl_pe_2100e8.efi abl_pe_4c111c.efi; do
	echo "=== $f ($(stat -c %s $f) bytes)"
	file $f | cut -c1-120
	strings -n 8 $f | grep -i -E 'exitbootservices|shutting down|preparePlatform|jump(ing)? to (the )?kernel|linuxloader|BootLinux|kernel load|Kernel Load' | head -6
	# raw disassembly of the whole file is enough to count the interesting instructions
	$OD -D -b binary -m aarch64 $f 2>/dev/null > /tmp/$f.dis
	printf '  msr sctlr_el1: %s   dc cisw/csw/isw (set/way): %s   dc civac/cvac: %s   ic iallu: %s   tlbi: %s   smc: %s\n' \
	  "$(grep -c 'msr\s*sctlr_el1' /tmp/$f.dis)" "$(grep -c -E 'dc\s*(cisw|csw|isw)' /tmp/$f.dis)" \
	  "$(grep -c -E 'dc\s*(civac|cvac)' /tmp/$f.dis)" "$(grep -c 'ic\s*iallu' /tmp/$f.dis)" \
	  "$(grep -c 'tlbi' /tmp/$f.dis)" "$(grep -c -E '\ssmc\s' /tmp/$f.dis)"
done
