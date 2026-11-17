#!/bin/sh
# Test 44: Test 43 + the boot manager saves the RAM log into a slot in recovery (survives the cold reset): probe of the full r5q firmware (r5qfullPkg: every Qualcomm DXE, Test 12 boot manager and memory map).
# Read-only lab loader: reports the Qualcomm USB device protocol, block devices and conventional RAM, then returns.
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1  -DR5Q_RESERVED_WINDOW=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_FULLFW_PROBE=1 "-DR5Q_TEST_LABEL=\"TEST44\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
