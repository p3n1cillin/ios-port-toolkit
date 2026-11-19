#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# build libr5q-app.dylib (and, with SBDIAG=1, libr5q-sbdiag.dylib) and the root volume with them injected; copy as
# testN-root.apfs.new. APPS overrides the app list (kern.exec_inject holds 2048 bytes from kernel t157).
set -e
N=$1
# paths: R5Q_PORT = this port (work/ios-r5q, as seen from WSL/Linux), IOS_WORK = the Linux work area (donor
# userland, SDK); the defaults are the S10 Lite machine's layout
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
IOS_WORK=${IOS_WORK:-${R5Q_WORK}}
GL=${GL-1}; RC=${RC-rc.r5q}; RC_JOBS=${RC_JOBS-rc.jobs.quiet}; EXTRA=${EXTRA-"obj/r5qd obj/r5q_bind"}; EXTRA_SHARE=${EXTRA_SHARE-"/var/tmp/r5qd-share/r5qd /var/tmp/r5q-ringtones/Ringtones"}   # (the normal image: no bring-up tests, r5qd)
# (Test 197) locationd: allowed, and run from /var/r5q/bin/locationd (31 MB, too big for the system image; put there over r5qd)
# (Test 198 tried fairplayd.H2 the same way for WeatherKit: it exits at once, respawned every 10 s -- left out)
export ALLOW=${ALLOW-locationd} RELOCATE=${RELOCATE-com.apple.locationd=/var/r5q/bin/locationd}
SDK=${SDK:-$IOS_WORK/sdks-source/iPhoneOS16.5.sdk}
W=$R5Q_PORT/wifi
# (Test 256) Bluetooth (BT=1, the default; BT= leaves it out): bluetoothd allowed, on the kernel's kctl
# com.apple.uart.bluetooth (JOBENV, added to mkjobs.py by patch_mkjobs.py), and com.apple.BlueTool answered by
# wifi/r5q_bluetool.c from /redwood/bin. Without the kernel's stage 4 (boot-arg bt=9) bluetoothd finds no
# /dev/uart.bluetooth and exits 0 at once, and stays down until it is asked for again.
BT=${BT-1}
python3 $R5Q_PORT/pipeline/patch_mkjobs.py $IOS_WORK/linux-source/userland/mkjobs.py
if [ -n "$BT" ]; then
	clang -target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O1 -fblocks -o $IOS_WORK/linux-source/userland/obj/r5q_bluetool \
		$W/r5q_bluetool.c 2>&1 | grep -v 'warning\|^ \|^$\|generated' || true
	EXTRA="$EXTRA obj/r5q_bluetool"
	# (Test 258) labelled com.redwood.BlueTool: launchd panics when a com.apple.* job's program is not Apple's
	export ALLOW="$ALLOW bluetooth" RELOCATE="$RELOCATE com.apple.BlueTool=/redwood/bin/r5q_bluetool"
	export RELABEL="${RELABEL:+$RELABEL }com.apple.BlueTool=com.redwood.BlueTool"
	export JOBENV="${JOBENV:+$JOBENV }com.apple.bluetoothd:BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,115200"
	export OSLOG="${OSLOG-com.apple.backboardd} com.apple.bluetoothd"
fi
APPS=${APPS:-"MobileSMS MobileSlideShow MobileMail MobileNotes Reminders MobileCal MobileTimer News AppleTV Podcasts AppStore Maps Health Passbook MobilePhone Music Weather FaceTime MobileSafari Preferences Camera Contacts Files Calculator Books Compass VoiceMemos Shortcuts Tips FindMy Magnifier Home Stocks PosterBoard"}
cd $IOS_WORK/linux-source/userland
clang -target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O1 -dynamiclib \
	-install_name /redwood/lib/libr5q-app.dylib -lobjc -framework CoreFoundation -framework CoreGraphics -Wl,-undefined,dynamic_lookup \
	-o obj/libr5q-app.dylib $W/inject_app.c 2>&1 | grep -v 'warning\|^ \|^$\|generated' || true
# (Test 168) libr5q-bbd.dylib into backboardd: CoreBrightness's 5 s ALS wait made 0 (wifi/inject_bbd.c)
clang -target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O1 -dynamiclib -install_name /redwood/lib/libr5q-bbd.dylib -fblocks -lobjc -framework CoreFoundation -framework IOKit -o obj/libr5q-bbd.dylib $W/inject_bbd.c 2>&1 | grep -v 'warning\|^ \|^$\|generated' || true
EXTRA_LINE=""
rm -f obj/libr5q-sbdiag.dylib
if [ -n "$SBDIAG" ]; then
	clang -target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O1 -dynamiclib -install_name /redwood/lib/libr5q-sbdiag.dylib \
		-lobjc -o obj/libr5q-sbdiag.dylib $W/inject_sbdiag.c 2>&1 | grep -v 'warning\|^ \|^$\|generated' || true
	EXTRA_LINE="/redwood/lib/libr5q-sbdiag.dylib ${SBDIAG_FOR:-SpringBoard}"
fi
grep -q 'libr5q-\*' build.sh || sed -i 's|^\t\[ -f obj/libr5q-app.dylib \] \&\& cp obj/libr5q-app.dylib root/lib/.*|\tfor f in obj/libr5q-*.dylib; do [ -f "$f" ] \&\& cp "$f" root/lib/; done      # (r5q: inside the stock programs)|' build.sh
# (Test 255) the Bluetooth controller's firmware (crbtfw21.tlv ROM patch, crnv21.bin NVM: the vendor partition's
# /firmware, dumped into ../fw-bt -- the vendor's, never in the repository) in the root's /fw, where the kernel's
# Bluetooth stage reads it (redwood_image_file("fw", ...)) as the GPU's firmware from ../fw-gpu
grep -q 'fw-bt' build.sh || sed -i 's|^if \[ -d \.\./fw-gpu \]; then|if [ -d ../fw-bt ]; then mkdir -p root/fw; cp ../fw-bt/* root/fw/; fi      # (r5q: Bluetooth firmware)\n&|' build.sh
INJECT="$(printf '%s\n' '/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system' '/redwood/lib/libredwood-sb.dylib SpringBoard' '/redwood/lib/libredwood-media.dylib mediaserverd' '/redwood/lib/libredwood-app.dylib Sileo' '/redwood/lib/libredwood-tweaks.dylib Preferences' "/redwood/lib/libr5q-app.dylib $APPS" "/redwood/lib/libr5q-bbd.dylib backboardd" "$EXTRA_LINE")"
echo "inject: $(echo "$INJECT" | tr '\n' ';' | wc -c) bytes"
env REST=1 PREBUILT=1 INJECT="$INJECT" ${GL:+GL="$GL"} ${EXTRA:+EXTRA="$EXTRA"} ${EXTRA_SHARE:+EXTRA_SHARE="$EXTRA_SHARE"} ${RC_JOBS:+RC_JOBS="$RC_JOBS"} ${RC:+RC="$RC"} ./build.sh > /tmp/userland-t$N.log 2>&1 || { tail -5 /tmp/userland-t$N.log; exit 1; }
ls root/lib | grep r5q | tr '\n' ' '; echo
cp root.apfs $R5Q_PORT/port/build/test$N-root.apfs.new
echo "root test$N built"
