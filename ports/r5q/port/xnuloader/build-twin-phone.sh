#!/bin/sh
# EMULATION ONLY for now: the production loader exactly as an r5q iOS attempt would carry it (no UART: the
# phone has none), plus the Samsung RAM-ring mirror of every loader line (R5Q_LAB_TRACE + R5Q_COHERENT_TRACE),
# so the kernel log the loader prints after the kernel returns survives the reset and reads back as
# last_kmsg; and the reverse-engineered ABL cache/MMU shutdown (R5Q_ABL_SEQUENCE, Test 30's candidate fix).
# Run on the twin: TWIN_LOADER=port/build/twin-ios/loader-twin-phone.efi python3 twin/run_twin.py ios N
# Not hardware-approved: the hand-off it relies on is what Tests 19-31 are still deciding.
set -eu
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -DR5Q_LAB_TRACE=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0x9B4DC000 \
  /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
