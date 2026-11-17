#!/bin/bash
set -euo pipefail
mkdir -p /scratch/home /scratch/tmp /scratch/source /scratch/kernel
mkdir -p /scratch/theos/toolchain/linux /scratch/theos/sdks
cp -a /donor/xnu-env.sh /donor/xnu-build.sh /donor/xnu-deps /donor/xnu /scratch/source/
tar -C /apple --exclude=.git -cf - . | tar -C /scratch/kernel -xf -
git -C /scratch/kernel apply --check /donor/xnu/redwood.patch
git -C /scratch/kernel apply --whitespace=nowarn /donor/xnu/redwood.patch
cp -a /donor/xnu/files/. /scratch/kernel/
# r5q overlay (work/ios-r5q/kernel-build/r5q-overlay): our own changes on top of the donor's files
[ ! -d /scripts/r5q-overlay ] || cp -a /scripts/r5q-overlay/. /scratch/kernel/
for repository in AvailabilityVersions libdispatch libpthread mm_bootstrap_cmds tcsh unifdef; do
  cp -a "/cache/$repository" /scratch/source/xnu-deps/
done
tar -C /scratch/source/xnu-deps -xJf /cache/MacOSX13.3.sdk.tar.xz \
  --wildcards '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*'
test -d /scratch/source/xnu-deps/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit
ln -s /cctools /scratch/theos/toolchain/linux/iphone
ln -s /ios-sdk /scratch/theos/sdks/iPhoneOS16.5.sdk
export THEOS=/scratch/theos XNU_SRC=/scratch/kernel JOBS=4
source /scratch/source/xnu-env.sh
# setup.sh writes usr/local into its SDK copy. Its own `cp -rs` of the theos path would copy the
# symlink itself (pointing into the read-only /ios-sdk bind), so pre-make a real symlink tree here.
mkdir -p "$(dirname "$XNU_SDK_PATH")"
cp -rs /ios-sdk "$XNU_SDK_PATH"
test -d "$XNU_SDK_PATH/usr/include" && test ! -L "$XNU_SDK_PATH" && test ! -L "$XNU_SDK_PATH/usr"
# tools/lldbmacros installs scripts through `#!/usr/bin/env python`; the host only has python3.
mkdir -p /scratch/pybin
ln -s /usr/bin/python3 /scratch/pybin/python
export PATH=/scratch/pybin:$PATH
cd /scratch/source
if ! LIBS=-l:libcrypt.so.1 xnu-deps/setup.sh --stage1 > /scratch/stage1.log 2>&1; then
  tail -80 /scratch/stage1.log >&2
  for log in xnu-deps/mig-build.log xnu-deps/tcsh-build.log; do
    test ! -f "$log" || tail -40 "$log" >&2
  done
  test ! -f xnu-deps/mm_bootstrap_cmds/config.log || grep -A5 -B3 'error:\|cannot find\|undefined reference' xnu-deps/mm_bootstrap_cmds/config.log | tail -60 >&2
  exit 10
fi
if ! xnu-deps/setup.sh --stage2 > /scratch/stage2.log 2>&1; then
  tail -60 /scratch/stage2.log >&2
  test ! -f xnu-deps/exporthdrs.log || tail -80 xnu-deps/exporthdrs.log >&2
  exit 11
fi
set +e
./xnu-build.sh -j4 -k > /scratch/xnu-build.log 2>&1
build_status=$?
set -e
printf 'XNU_MAKE_EXIT=%s\n' "$build_status" >&2
# Primary failures are "Error 1" (or other non-2) lines; "Error 2" lines are make's recursion
# cascading them upward. The donor documents check_all_exports (config/Makefile:121) and the
# symbolsets (config/Makefile:108) as the only expected ones: post-link export-list checks.
grep -E '\*\*\* \[.*\] Error [0-9]+$' /scratch/xnu-build.log | grep -v ' Error 2$' \
  | grep -v -E 'config/Makefile:(108|121)' > /scratch/unexpected-failures.txt || true
printf 'XNU_UNEXPECTED_FAILURES=%s\n' "$(wc -l < /scratch/unexpected-failures.txt)" >&2
printf 'XNU_EXPORT_LIST_FAILURES=%s\n' "$(grep -c -E 'config/Makefile:(108|121).* Error 1$' /scratch/xnu-build.log || true)" >&2
printf 'XNU_EXPORT_COMPLAINTS=%s\n' "$(grep -c 'exported name not in import list' /scratch/xnu-build.log || true)" >&2
cat /scratch/unexpected-failures.txt >&2
echo '--- make errors (full log) ---' >&2
grep -n -B6 -E '\*\*\* |error:|Error [0-9]' /scratch/xnu-build.log | grep -v SyntaxWarning | head -200 >&2 || true
echo '--- tail ---' >&2
tail -40 /scratch/xnu-build.log >&2
kernel=/scratch/kernel/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
if [ ! -s "$kernel" ]; then
  exit 12
fi
# A linked artifact is retained even if post-link export checks fail. The parent
# records make's status separately; this is never a hardware-approved kernel.
base64 -w0 "$kernel"
