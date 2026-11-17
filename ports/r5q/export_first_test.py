"""Publish a local, device-specific Test 1 bundle with a verified restore image."""
import hashlib
import io
import json
import pathlib
import shutil
import tarfile
import sys

workspace = pathlib.Path(sys.argv[1]).resolve(strict=True)
work = workspace / 'work/ios-r5q'
build = work / 'port/build'
out = workspace / 'outputs/S10-Lite-native-iOS-port/first-test'
out.mkdir(parents=True, exist_ok=True)

def copy(source, relative):
    assert source.is_file() and not source.is_symlink()
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return {'path': str(relative), 'bytes': target.stat().st_size,
            'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}

image = copy(build / 'r5q-test1-recovery.img', pathlib.Path('r5q-test1-recovery.img'))
original = copy(work / 'backups/<SERIAL>/recovery.img', pathlib.Path('restore/recovery-original.img'))
helper = copy(build / 'reboot-recovery', pathlib.Path('reboot-recovery'))
copy(work / 'Manage-Test1.ps1', pathlib.Path('Manage-Test1.ps1'))
for name in ['r5q-test1-image.json', 'r5q-test1-image-validation.txt', 'test_first_shim.json',
             'test_first_loader.json', 'test_first_helper.json']:
    copy(build / name, pathlib.Path('validation') / name)
copy(work / 'android-tools/sources.json', pathlib.Path('validation/android-tool-pins.json'))
for name in ['FirstBootManager.c', 'FirstBootManager.inf', 'BootShim.S', 'reboot-recovery.S']:
    copy(work / 'port/first-test' / name, pathlib.Path('source/first-test') / name)
for name in ['loader.c', 'efi.h', 'identity.h', 'build-first-test.sh']:
    copy(work / 'port/xnuloader' / name, pathlib.Path('source/xnuloader') / name)
for name in ['prepare_first_test.py', 'build_first_shim.py', 'package_first_test.py',
             'build_loader_linux.py', 'build_uefi_linux.py', 'run_first_checks.py',
             'test_first_shim.py', 'test_first_loader.py', 'test_first_helper.py']:
    copy(work / name, pathlib.Path('source/tools') / name)
copy(work / 'port/build-uefi.sh', pathlib.Path('source/build-uefi.sh'))
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
board = mu / 'Platforms/Samsung/r5qtestPkg'
for path in board.rglob('*'):
    if path.is_file():
        copy(path, pathlib.Path('source/mu-overlay/Platforms/Samsung/r5qtestPkg') / path.relative_to(board))
copy(mu / 'LICENSE', pathlib.Path('licenses/Mu-Silicium.txt'))
copy(work / 'port/LICENSE-upstream', pathlib.Path('licenses/loader-APSL-2.0.txt'))

tar_path = out / 'restore/recovery-original-odin.tar'
with tarfile.open(tar_path, 'w', format=tarfile.USTAR_FORMAT) as archive:
    blob = (out / 'restore/recovery-original.img').read_bytes()
    info = tarfile.TarInfo('recovery.img')
    info.size = len(blob)
    info.mode = 0o644
    info.mtime = 0
    archive.addfile(info, io.BytesIO(blob))
with tarfile.open(tar_path) as archive:
    assert archive.getnames() == ['recovery.img']
    assert hashlib.sha256(archive.extractfile('recovery.img').read()).hexdigest() == original['sha256']

manifest = {'status': 'ready for first on-device diagnostic attempt; not yet boot-tested',
            'model': 'SM-G770F', 'serial': '<SERIAL>', 'target_partition': 'recovery /dev/block/sda21',
            'image': image, 'original_recovery': original, 'reboot_helper': helper,
            'original_boot_sha256': '381e6aa72b0fee42a04c4f7be51d0c63bccaaed26e54f9ac30d3de6255e1bc1a',
            'original_vbmeta_sha256': '6e614ba7ab223c23548dc65734358f4786d4f4008107695dd9e8b1494791790c',
            'restore_tar_sha256': hashlib.sha256(tar_path.read_bytes()).hexdigest(),
            'flash_performed': False, 'phone_reboot_requested': False,
            'test': 'ARM64 Image -> bounded XNUP copy -> source-only UEFI -> diagnostic XNU-loader entry -> requested cold reset',
            'scope': 'No XNU kernel, iOS userland, storage driver or iOS installation in Test 1',
            'upstream_mu': 'e51cd877e31c8fbb322032dc4cb6715e83d25ad3',
            'upstream_loader': '946de0fed8e583606ca0470506c9e847f45e926f'}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'README.md').write_text('''# R5Q native boot — Test 1 ready

This image is prepared for the rooted test S10 Lite **SM-G770F, serial <SERIAL> only**. No firmware has been flashed and no phone reboot has been requested during preparation. This test checks the native boot path and loader; **it does not boot iOS yet**.

## What is ready

The Android-v2 recovery image preserves this phone's original header addresses, OS version, ramdisk, DTB and recovery overlay. The ARM64 kernel slot contains the native boot shim, a source-built UEFI and a bounded XNUP payload with the diagnostic loader. The firmware includes 22 standard/source-built DXE modules; vendor TZ, storage, USB, charger and touchscreen drivers are excluded. The first-test boot manager is separate from the earlier development firmware.

The diagnostic makes no persistent log or storage writes. The test operator writes **only recovery** to launch it. Android's boot, data, VBMeta, DTBO, XBL, modem and partition table are not installation targets. Ordinary Android startup remains the return route. Bootloader/reset behavior and actual panel output are what the first hardware attempt must establish; an automatic return is not yet proven.

## Checks completed

- UEFI and diagnostic EFI loader compiled in a no-network, resource-limited Ubuntu sandbox.
- Five tests executed the actual ARM64 boot-shim instructions: valid payload, bad magic, oversized length, short length and FD already in place.
- Four tests executed the compiled EFI diagnostic with stubbed UEFI services: normal load, relocated load, missing GOP and wrong display configuration. Emulated CPU/console output in those reports is **not evidence from the phone**.
- The recovery-entry helper's actual ARM64 syscall arguments were checked with SVC interception. It requests kernel recovery reboot without writing an Android recovery command to `misc`.
- AOSP avbtool verified the image's hash descriptor. The image uses algorithm NONE for the already unlocked test phone; it is not Samsung-signed. AOSP's independent unpacker recovered the exact kernel, ramdisk, recovery DTBO and DTB inputs.
- The original recovery image and one-file Odin restore tar are included, with hashes. The connected phone's current boot/recovery/VBMeta, root, model, battery and partition size are checked by the operator script.

## First test

Keep the phone connected, unlocked in Android, USB debugging allowed, and charged to at least 30%. A visible unlocked-bootloader warning may require its on-screen continue key. Watch the display during the attempt.

From this directory, the operator script supports:

```powershell
.\\Manage-Test1.ps1 -Action Check    # read-only preflight
.\\Manage-Test1.ps1 -Action Stage    # upload and hash-check temporary files only
.\\Manage-Test1.ps1 -Action Start    # write verified Test 1 to recovery and request its boot
.\\Manage-Test1.ps1 -Action Restore  # restore and verify the original recovery from Android/root
```

`Start` is the actual hardware test, not another preparation check. It refuses a wrong phone, locked bootloader, changed boot/VBMeta, unexpected recovery image, wrong partition size, low battery or pending BCB command. It writes and verifies the complete recovery image before using the small native reboot helper. It does not use ordinary `adb reboot recovery`, which can leave an Android recovery command pending for a non-Android diagnostic.

Expected visible progression:

1. `R5Q NATIVE TEST 1` / `TEST1 UEFI CONSOLE OK`.
2. `TEST1 PAYLOAD OK` / `TEST1 EFI IMAGE LOADED`.
3. `TEST1 XNU LOADER ENTRY OK`, configured display information, and `TEST1 HANDOFF OK`.
4. `TEST1 LOADER RESULT: Success`, followed by a requested cold reset after 20 seconds. Let Android finish booting, then restore the original recovery.

Report the furthest marker reached, whether the screen is readable or garbled, and whether Android returns. The display size printed comes from our GOP configuration; the visible panel output is the independent hardware observation. No USB diagnostic interface is expected in Test 1.

If the image hangs, hold **Volume Down + Side** until the screen turns black, then release both and allow normal Android startup. Continue an unlocked-bootloader warning using its displayed key. Run `Restore` once Android/ADB/root are available. If recovery is entered repeatedly, stop and restore rather than repeating the test.

If normal Android cannot be reached, enter Samsung Download Mode using the already established button/cable sequence. `restore/recovery-original-odin.tar` contains exactly the original `recovery.img` for an AP-slot restore with a compatible Samsung flashing tool. It has not been flashed as part of these checks. Keep the previously downloaded matching Samsung firmware available as the second fallback. The phone must remain unlocked throughout this development test.

## Exact results and provenance

See `manifest.json` and `validation/`. Source changes, ARM64 checks, packaging helpers and the separate Mu overlay are under `source/`; upstream licenses are under `licenses/`. Private device images in this local test bundle belong to this phone and are not a generic S10 Lite distribution.

AOSP tools were pinned from [AVB](https://android.googlesource.com/platform/external/avb/) and [mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg/); the precise revisions and file hashes are in `validation/android-tool-pins.json`. Bootloader verification flags and the phone's original images are preserved, not changed by these tools.
''')
print(f'Prepared Test 1 bundle: {out}')
print(f"Image SHA256: {image['sha256']}; original recovery SHA256: {original['sha256']}")
