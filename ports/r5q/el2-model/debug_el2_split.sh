#!/bin/bash
# Split the stand-in boot failure: does AAVMF boot with virtualization=on (at EL2) with/without our DTB?
W=/var/tmp/codex-ios-r5q/work/el2-model
d=$W/run-split; rm -rf "$d"; mkdir -p "$d"; cd "$d"
boot() { # boot LABEL extra-args...
	local label=$1; shift
	cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
	timeout 25 qemu-system-aarch64 -M virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a76 -smp 1 -m 3G \
	  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd -drive if=pflash,format=raw,file=vars.fd \
	  "$@" -nic none -display none -serial stdio -monitor none -no-reboot 2>&1 | tr -d '\r' > $label.log
	printf '%-22s %s\n' "$label" "$(grep -a -o -m1 'Synchronous Exception at 0x[0-9A-Fa-f]*\|BdsDxe: [a-z]*\|Shell>\|UEFI Interactive Shell' $label.log | head -1)"
}
boot el2-plain
boot el2-moddtb -dtb $W/virt3g-mod.dtb
boot el2-shimpc-nodtb -device loader,file=$W/shim-vm.elf,cpu-num=0
grep -a -m3 'EL2\|Synchronous' el2-shimpc-nodtb.log
