#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}   # ports/r5q of this repository
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# EMULATION ONLY: run the port loader's return-path self-test (checkpoint 999) and chosen kernel
# checkpoints under an EL2 stand-in for Samsung uH (shim.S), in QEMU virt with virtualization=on.
# usage: run_el2_model.sh [CHECKPOINT ...]   (default: 999)
set -u
SRC=${R5Q_PORT}/el2-model
OUT=${R5Q_PORT}/port/build/el2-model
W=${R5Q_WORK}/el2-model
KERNEL=${R5Q_PORT}/port/build/xnu-source-rebuild.macho
LOADER=${LOADER:-${R5Q_WORK}/port-loader-qemu.efi}
PREFIX=${PREFIX:-}
mkdir -p "$W" "$OUT"; cd "$W"
CHECKPOINTS=${*:-999}
MACHINE="virt,gic-version=3,virtualization=on,highmem=off"

# 1. stand-in variants
declare -A HCR=([vm]=0x80000001 [vm-ptw]=0x80000005 [vm-ptw-tvm]=0x84000005)
for v in "${!HCR[@]}"; do
	clang --target=aarch64-none-elf -DHCR_VALUE=${HCR[$v]} -c "$SRC/shim.S" -o shim-$v.o
	ld.lld -Ttext=0xFC000000 -e _start -o shim-$v.elf shim-$v.o
done

# 2. device tree with the top 64 MiB (0xFC000000-0x100000000) hidden from UEFI
qemu-system-aarch64 -M "$MACHINE,dumpdtb=virt3g.dtb" -cpu cortex-a76 -m 3G -nographic -nic none >/dev/null 2>&1
dtc -q -I dtb -O dts virt3g.dtb | sed 's/reg = <0x00 0x40000000 0x00 0xc0000000>;/reg = <0x00 0x40000000 0x00 0xbc000000>;/' > virt3g-mod.dts
grep -q 'reg = <0x00 0x40000000 0x00 0xbc000000>' virt3g-mod.dts || { echo "memory node not patched"; exit 2; }
dtc -q -I dts -O dtb virt3g-mod.dts -o virt3g-mod.dtb

run() { # run LABEL CHECKPOINT [shim variant | none]
	local label=$1 cp=$2 v=$3 d=$W/run-$1
	rm -rf "$d"; mkdir -p "$d/esp/EFI/BOOT"
	cp "$LOADER" "$d/esp/EFI/BOOT/BOOTAA64.EFI"
	cp /usr/share/AAVMF/AAVMF_VARS.fd "$d/vars.fd"
	python3 -I - "$KERNEL" "$cp" "$d/payload.bin" <<'PY'
import struct, sys
kernel = open(sys.argv[1], 'rb').read()
item = lambda n, d: struct.pack('<16sII', n.encode(), len(d), 0) + d + bytes(-len(d) % 16)
body = item('kernel', kernel) + item('cmdline', b'-v\0') + item('checkpoint', (sys.argv[2] + '\0').encode()) + bytes(24)
open(sys.argv[3], 'wb').write(struct.pack('<II', 0x50554e58, len(body)) + body)
PY
	local extra=()
	[ "$v" != none ] && extra=(-dtb "$W/virt3g-mod.dtb" -device "loader,file=$W/shim-$v.elf,cpu-num=0")
	local machine=$MACHINE
	[ "$v" = none ] && machine="virt,gic-version=3,highmem=off"
	cd "$d"
	timeout 120 /usr/bin/bwrap --unshare-all --die-with-parent --ro-bind /usr /usr --ro-bind /bin /bin \
	  --ro-bind /lib /lib --ro-bind /lib64 /lib64 --ro-bind /etc/alternatives /etc/alternatives --proc /proc \
	  --dev /dev --tmpfs /tmp --tmpfs /var/tmp --ro-bind "$W" "$W" --bind "$d" "$d" --chdir "$d" \
	  /usr/bin/qemu-system-aarch64 -M "$machine" -cpu cortex-a76 -smp 1 -m 3G -accel tcg,tb-size=256 \
	  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
	  -drive if=pflash,format=raw,file=vars.fd -drive file=fat:rw:esp,format=raw,if=virtio \
	  -device loader,file=payload.bin,addr=0xF0000000,force-raw=on -device ramfb "${extra[@]}" \
	  -nic none -display none -serial stdio -monitor none -no-reboot > serial.log 2>&1
	cd "$W"
	tr -d '\r' < "$d/serial.log" > "$OUT/$label.log"
	local verdict
	if [ "$cp" = 999 ]; then
		grep -q 'return-path self test passed' "$OUT/$label.log" && verdict=PASS || verdict=FAIL
	else
		grep -q "STOPPED AT CHECKPOINT $cp\$" "$OUT/$label.log" && verdict=PASS || verdict=FAIL
	fi
	printf '%-28s %s  TVM-traps-printed=%s  EL2-unexpected=%s\n' "$label" "$verdict" \
	  "$(grep -c '\[EL2\] TVM' "$OUT/$label.log")" "$(grep -c 'UNEXPECTED\|SELF FAULT\|SError' "$OUT/$label.log")"
}
for cp in $CHECKPOINTS; do
	run "${PREFIX}cp$cp-no-hypervisor" "$cp" none
	for v in vm vm-ptw vm-ptw-tvm; do run "${PREFIX}cp$cp-$v" "$cp" "$v"; done
done
