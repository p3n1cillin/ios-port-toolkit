#!/bin/sh
# Test 53: read-only dump of the display pipeline (MDP top, CTL0, DMA0 pipe, ping-pong 0, INTF 0/1, DSI0) + underrun samples around one kick; under the MINIMAL firmware (picture right).
# Read-only lab loader: reports the Qualcomm USB device protocol, block devices and conventional RAM, then returns.
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1  -DR5Q_RESERVED_WINDOW=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_MDSS_DUMP=1 "-DR5Q_TEST_LABEL=\"TEST53\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
