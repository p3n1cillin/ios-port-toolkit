#!/bin/bash
# Copy work products created outside the project folder (WSL workspace) into it. Idempotent.
set -eu
P=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
W=/var/tmp/codex-ios-r5q/work
mkdir -p "$P/research/samsung-kernel-subset" "$P/research/fw" "$P/research/fw/disasm" "$P/el2-model/built" "$P/port/build/qemu-boot" "$P/donor-additions/qemu"
# Samsung SM-G770F GPL kernel subset (sparse checkout, commit 153e588c), minus git metadata
rsync -a --exclude .git "$W/research-src/sm-g770f-kernel/" "$P/research/samsung-kernel-subset/"
# stock firmware images extracted as data + their disassembly
cp -u "$W/research-fw/hyp.mbn" "$W/research-fw/abl.elf" "$P/research/fw/"
cp -u "$W"/research-fw/*.dis "$W"/research-fw/hyp-seg*.bin "$P/research/fw/disasm/" 2>/dev/null || true
# EL2 stand-in build outputs and the device tree it boots with
cp -u "$W"/el2-model/shim-*.elf "$W"/el2-model/virt3g*.dts "$W"/el2-model/virt3g-mod.dtb "$P/el2-model/built/" 2>/dev/null || true
cp -u "$W/el2-model/run-cp999-vm/qemu-int.log" "$P/el2-model/built/debug-qemu-int.log" 2>/dev/null || true
# QEMU-built port loader and the full-iOS QEMU logs
cp -u "$W/port-loader-qemu.efi" "$P/port/build/"
for l in source prebuilt source-portloader; do cp -u "$W/qemu-ios-$l.log" "$P/port/build/qemu-boot/ios-$l.log" 2>/dev/null || true; done
cp -u "$W/ios165-fetch.log" "$W/userland-build.log" "$P/port/build/qemu-boot/" 2>/dev/null || true
# scripts generated into the donor tree
cp -u "$W/linux-source/qemu/run-ios-kernel.sh" "$W/linux-source/qemu/run-kernel.sh" "$P/donor-additions/qemu/"
du -sh "$P/research/samsung-kernel-subset" "$P/research/fw" "$P/el2-model/built" "$P/donor-additions"
