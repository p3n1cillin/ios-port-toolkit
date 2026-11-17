#!/bin/bash
# Make qemu/run-ios-kernel.sh in the donor tree: run-ios.sh with the kernel taken from $KERNEL
# instead of the fixed xnu-src path. EMULATION ONLY; the donor's own run-ios.sh is left unchanged.
set -eu
cd /var/tmp/codex-ios-r5q/work/linux-source
sed -e 's|^K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837$|K=${KERNEL:?set KERNEL to the kernel to boot}|' \
    -e 's|^\[ -f \$K \] .*|[ -f "$K" ] \|\| { echo "run-ios-kernel.sh: no kernel at $K" >\&2; exit 1; }|' \
    qemu/run-ios.sh > qemu/run-ios-kernel.sh
chmod +x qemu/run-ios-kernel.sh
diff qemu/run-ios.sh qemu/run-ios-kernel.sh || true
sed -n 1,25p qemu/screendump.sh
# ... and qemu/run-kernel.sh: run.sh with the loader taken from $LOADER when set; run-ios-kernel.sh uses it.
sed -e 's|^L=../xnuloader/loader.efi; \[ -f \$L \] \|\| L=../prebuilt/loader.efi$|L=${LOADER:-../xnuloader/loader.efi}; [ -f "$L" ] \|\| L=../prebuilt/loader.efi|' \
    -e 's|cp -u \$L esp/EFI/BOOT/BOOTAA64.EFI|cp "$L" esp/EFI/BOOT/BOOTAA64.EFI|' qemu/run.sh > qemu/run-kernel.sh
chmod +x qemu/run-kernel.sh
sed -i 's|exec qemu/run.sh |exec qemu/run-kernel.sh |' qemu/run-ios-kernel.sh
diff qemu/run.sh qemu/run-kernel.sh || true
grep -n 'run-kernel.sh' qemu/run-ios-kernel.sh
