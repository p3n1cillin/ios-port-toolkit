#!/bin/bash
# Fetch only the hypervisor/watchdog/reset-related paths of the pinned Samsung SM-G770F GPL kernel.
set -eu
D=/var/tmp/codex-ios-r5q/work/research-src/sm-g770f-kernel
COMMIT=153e588c8576dcf60cdc4d3a31765ee8721bfec3
mkdir -p "$D"; cd "$D"
[ -d .git ] || { git init -q; git remote add origin https://github.com/Androser420/sm-g770f-kernel.git; }
git sparse-checkout init --no-cone
git sparse-checkout set '/arch/arm64/configs/' '/drivers/soc/qcom/watchdog*' '/drivers/power/reset/' \
  '/drivers/samsung/debug/' '/include/linux/samsung/debug/' '/drivers/uh/' '/include/linux/uh*' '/include/linux/rkp*' \
  '/drivers/rkp/' '/arch/arm64/kernel/rkp*' '/arch/arm64/kernel/uh*' '/arch/arm64/include/asm/uh*' '/arch/arm64/include/asm/rkp*' \
  '/drivers/soc/qcom/scm*' '/drivers/soc/qcom/qtee*' '/drivers/firmware/psci*' '/arch/arm64/kernel/cpu-reset*' \
  '/arch/arm64/kernel/relocate_kernel*' '/arch/arm64/kernel/hyp-stub*' '/security/samsung/' '/drivers/soc/qcom/hyp*'
git fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git checkout -q FETCH_HEAD
git rev-parse HEAD
find . -path ./.git -prune -o -type f -print | sed 's|^\./||' | awk -F/ '{print $1"/"$2"/"$3}' | sort | uniq -c | sort -rn | head -30
ls arch/arm64/configs | grep -i 'r5q\|sm8150\|msmnile' | head
