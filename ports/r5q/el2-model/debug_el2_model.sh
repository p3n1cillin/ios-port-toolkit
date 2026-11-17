#!/bin/bash
# One VM-only boot under the stand-in with QEMU's exception log (EMULATION ONLY, debugging the model).
W=/var/tmp/codex-ios-r5q/work/el2-model
d=$W/run-cp999-vm
cd "$d" || exit 1
cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
timeout 40 qemu-system-aarch64 -M virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a76 -smp 1 -m 3G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd -drive if=pflash,format=raw,file=vars.fd \
  -dtb $W/virt3g-mod.dtb -device loader,file=$W/shim-vm.elf,cpu-num=0 \
  -nic none -display none -serial stdio -monitor none -no-reboot -d int -D qemu-int.log > /dev/null 2>&1
grep -n -m 12 -E 'Taking exception|ESR|ELR|FAR|from EL|to EL' qemu-int.log | head -40
