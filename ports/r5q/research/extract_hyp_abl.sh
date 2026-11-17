#!/bin/bash
# Extract hyp.mbn and abl.elf from the stock SM-G770F BL package as DATA (never executed).
set -eu
SRC=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/s10-relock/BL_G770FXXS9HXA1_G770FXXS9HXA1_MQB76443424_REV00_user_low_ship_MULTI_CERT.tar.md5
OUT=/var/tmp/codex-ios-r5q/work/research-fw
mkdir -p "$OUT"; cd "$OUT"
tar -xf "$SRC" hyp.mbn.lz4 abl.elf.lz4
command -v lz4 >/dev/null || { echo "lz4 missing"; exit 3; }
lz4 -d -f -q hyp.mbn.lz4 hyp.mbn
lz4 -d -f -q abl.elf.lz4 abl.elf
ls -la hyp.mbn abl.elf
sha256sum hyp.mbn abl.elf
file hyp.mbn abl.elf
readelf -h hyp.mbn | grep -E 'Class|Machine|Entry'
readelf -lW hyp.mbn | grep -E 'LOAD|Type' | head -20
