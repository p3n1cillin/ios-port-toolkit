"""Export the reviewed source overlay and an explicit build-only receipt."""
import hashlib
import json
import pathlib
import shutil
import struct
import sys

workspace = pathlib.Path(sys.argv[1]).resolve(strict=True)
work = workspace / 'work/ios-r5q'
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
out = workspace / 'outputs/S10-Lite-native-iOS-port'
out.mkdir(parents=True, exist_ok=True)

def copy(source, relative):
    if source.is_symlink() or not source.is_file() or source.stat().st_size > 1024 * 1024:
        raise ValueError(f'Unexpected export input: {source}')
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)

for name in ['build_loader_linux.py', 'build_uefi_linux.py', 'prepare_uefi.py',
             'stage_firmware.py', 'finish_board.py']:
    copy(work / name, pathlib.Path('tools') / name)
for name in ['loader.c', 'efi.h', 'identity.h', 'build-sandbox.sh']:
    copy(work / 'port/xnuloader' / name, pathlib.Path('source/xnuloader') / name)
copy(work / 'port/build-uefi.sh', pathlib.Path('source/build-uefi.sh'))
copy(work / 'port/build/r5q-loader.efi', pathlib.Path('build-only/r5q-loader.efi'))
copy(work / 'port/LICENSE-upstream', pathlib.Path('licenses/loader-APSL-2.0.txt'))
copy(mu / 'LICENSE', pathlib.Path('licenses/Mu-Silicium.txt'))
board = mu / 'Platforms/Samsung/r5qPkg'
for source in board.rglob('*'):
    if source.is_file():
        copy(source, pathlib.Path('source/mu-overlay/Platforms/Samsung/r5qPkg') / source.relative_to(board))
copy(mu / 'Resources/Configs/r5q.toml', pathlib.Path('source/mu-overlay/Resources/Configs/r5q.toml'))
copy(mu / 'Binaries/r5q/extraction-manifest.json', pathlib.Path('firmware-extraction-manifest.json'))

loader = (work / 'port/build/r5q-loader.efi').read_bytes()
fd = (work / 'port/build/r5q-uefi.fd').read_bytes()
header_length = struct.unpack_from('<H', fd, 0x30)[0]
assert len(fd) == 0x300000 and fd[0x28:0x2c] == b'_FVH'
assert sum(struct.unpack('<' + 'H' * (header_length // 2), fd[:header_length])) & 0xffff == 0
pe = struct.unpack_from('<I', loader, 0x3c)[0]
assert loader[:2] == b'MZ' and struct.unpack_from('<H', loader, pe + 4)[0] == 0xaa64

receipt = {
    'status': 'BUILD ONLY: no native boot or iOS installation has been demonstrated',
    'board': 'Samsung SM-G770F / r5q / SM8150',
    'host': 'Windows PC with Ubuntu WSL, no Mac used',
    'pins': {'Mu-Silicium': 'e51cd877e31c8fbb322032dc4cb6715e83d25ad3',
             'G986N-iOS': '946de0fed8e583606ca0470506c9e847f45e926f'},
    'builds': {
        'loader': {'bytes': len(loader), 'sha256': hashlib.sha256(loader).hexdigest(),
                   'validation': 'AArch64 PE/COFF structure; successful sandboxed compile'},
        'uefi_fd': {'bytes': len(fd), 'sha256': hashlib.sha256(fd).hexdigest(),
                    'validation': 'successful sandboxed build; FV header checksum correct',
                    'location': 'work/ios-r5q/port/build/r5q-uefi.fd; not exported as an installer'}},
    'flashed': False,
    'native_ios_boot_test': 'not performed',
    'recovery_check': 'Boot and recovery partition hashes still match the original backups',
    'remaining': ['Review and adapt firmware driver compatibility fixes',
                  'Integrate bounded XNUP boot-shim / loader handoff',
                  'Validate recovery and an on-device UEFI console boot',
                  'Port XNU board peripherals, particularly Samsung sec_ts on I2C 0xc80000 / GPIO 87',
                  'Prepare Apple-sourced iOS userland and validate kernel boot, UI and persistence'],
}
(out / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
(out / 'README.md').write_text('''# S10 Lite native iOS port: incomplete development checkpoint

**iOS is not installed and has not booted on this phone. Android remains installed.**

This is a source overlay and a build receipt for SM-G770F/r5q development, not a flashable OS. The user requested direct boot replacing Android. That objective has not been achieved.

## Verified work

- Read the actual phone's hardware description and stock XBL configuration; backed up boot, recovery, DTBO, VBMeta and XBL.
- Matched 65 board-owned UEFI modules by GUID to SM8150 reference descriptions. Firmware is kept in the private work directory, not bundled here.
- Created a Mu-Silicium r5q package with the measured memory map, eight CPUs and 1080 x 2400 framebuffer. Removed reference Xiaomi panel/charger configurations and unrelated ACPI tables.
- Compiled a 3 MiB UEFI FD and an AArch64 XNU loader on Ubuntu. Builds ran without networking, host credentials or writable source, with bounded scratch space and resource limits.
- Added r5q loader selection, disabled persistent log writes, and kept the Darwin storage write bounds unset. The candidate loader's preferred load address is in measured ordinary RAM rather than the Samsung MLVM reservation.
- Rechecked the connected phone's boot and recovery hashes; both still match their backups. No iOS-related firmware writes were made.

The exact hashes and remaining work are in `build-receipt.json`. A successful compilation establishes neither that firmware can boot nor that iOS works.

## Missing before native iOS

The Samsung firmware modules still require compatibility review; extracted bytes under reference names containing "patched" are **stock, unpatched bytes**. The UEFI FD still uses the normal Mu boot manager: the XNUP handoff has not been integrated. The R5Q touchscreen is on a different serial engine and GPIO from the donor S20 driver. No S10 Lite kernel boot, iOS root filesystem installation, SpringBoard session, charging driver, GPU or networking validation has occurred. This checkpoint must not be flashed as a completed OS.

The underlying projects make native-iOS claims for other hardware; those claims were not independently reproduced here. The S20 donor also contains a port-in-progress description in `common/devices/y2q.conf`. No ready S10 Lite image was established from these sources.

## Sources and reconstruction

- [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium/tree/e51cd877e31c8fbb322032dc4cb6715e83d25ad3)
- [Linux XNU/iOS donor](https://github.com/leh0726/G986N-iOS/tree/946de0fed8e583606ca0470506c9e847f45e926f)
- [Pixel 4/SM8150 research reference](https://github.com/NWylynko/ios-flame/tree/745c4341af86089efb0df851fa8a27fb8face624)

The source overlay belongs over the pinned Mu tree after its submodules and Python dependencies are staged. `tools/prepare_uefi.py`, `tools/stage_firmware.py` and `tools/finish_board.py` reconstruct board inputs from the owner's private XBL extraction. `tools/build_uefi_linux.py` uses `source/build-uefi.sh` in a resource-limited bwrap namespace. `tools/build_loader_linux.py` takes the `source/xnuloader` directory and an output path. Build helpers retain the original WSL workspace paths and require adjustment if moved.

The loader sources retain the donor's APSL 2.0 license; Mu files retain their original notices and license. No Apple iOS filesystem or phone identifiers are distributed in this checkpoint.
''')
manifest = json.loads((out / 'firmware-extraction-manifest.json').read_text())
readme = (out / 'README.md').read_text().replace('Matched 65 board-owned', f"Matched {len(manifest['modules'])} board-owned")
(out / 'README.md').write_text(readme)
print(f'Exported source checkpoint and build receipt: {out}')
print(f"Firmware matches: {len(manifest['modules'])}; boot test: not performed; flashed: false")
