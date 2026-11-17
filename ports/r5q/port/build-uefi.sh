#!/bin/bash
set -euo pipefail
mkdir -p /scratch/home /scratch/tmp /scratch/mu
cd /scratch/mu
tar -C /source --exclude=.git -cf - . | tar -xf -
export WORKSPACE="$PWD"
export PACKAGES_PATH="$PWD/Platforms/Samsung:$PWD/Common/Mu:$PWD/Common/Mu_OEM_Sample:$PWD/Mu_Basecore:$PWD/Silicon/Qualcomm:$PWD/Silicon/Silicium:$PWD/Silicium-ACPI"
export EDK_TOOLS_PATH="$PWD/Mu_Basecore/BaseTools"
export BASE_TOOLS_PATH="$EDK_TOOLS_PATH"
export CONF_PATH="$PWD/Conf"
export PYTHON_COMMAND=/venv/bin/python
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python"
export PATH="$EDK_TOOLS_PATH/Source/C/bin:$EDK_TOOLS_PATH/BinWrappers/PosixLike:/venv/bin:/usr/bin:/bin"
export CLANG_BIN=/usr/bin/
mkdir -p "$CONF_PATH"
cp "$EDK_TOOLS_PATH/Conf/target.template" "$CONF_PATH/target.txt"
cp "$EDK_TOOLS_PATH/Conf/tools_def.template" "$CONF_PATH/tools_def.txt"
cp "$EDK_TOOLS_PATH/Conf/build_rule.template" "$CONF_PATH/build_rule.txt"
make -C "$EDK_TOOLS_PATH/Source/C" -j4 > /scratch/basetools.log 2>&1 || { tail -80 /scratch/basetools.log >&2; exit 1; }
board=${R5Q_BOARD:-r5q}
build -p "${board}Pkg/${board}.dsc" -a AARCH64 -b ${R5Q_TARGET:-RELEASE} -t CLANGPDB -n 4 \
  -D ENABLE_SECUREBOOT=0 -D FD_BASE=0x9FC00000 -D FD_SIZE=0x300000 \
  -D FD_BLOCKS=0x300 -D DEVICE_MODEL=0 > /scratch/uefi.log 2>&1 || { tail -100 /scratch/uefi.log >&2; exit 1; }
cat /scratch/uefi.log >&2
# evidence of the debug-property PCD the DXE core was compiled with (r5qfull Test 51: no clear-on-free)
grep -rh 'PcdDebugPropertyMask' Build/${board}Pkg/*/AARCH64/MdeModulePkg/Core/Dxe/DxeMain/ 2>&1 | grep -v '^Binary' | sort -u | head -6 | sed 's/^/DXECORE /' >&2 || true
ls Build/${board}Pkg/*/AARCH64/MdeModulePkg/Core/Dxe/DxeMain/DEBUG/ 2>&1 | head -5 | sed 's/^/DXECORE-LS /' >&2 || true
ls Build/${board}Pkg/*/FV/Ffs 2>&1 | grep -i 'r5q\|FirstBoot' | sed 's/^/FFS-LS /' >&2 || true
base64 -w0 "Build/${board}Pkg/${R5Q_TARGET:-RELEASE}_CLANGPDB/FV/SILICIUM_UEFI.fd"
