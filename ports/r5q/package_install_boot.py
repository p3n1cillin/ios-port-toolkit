"""The install: the iOS start image for the BOOT partition (replaces Android's kernel; the owner approved the full
install 2026-11-16). Wraps port/build/r5q-test70-kernel.bin (shim + firmware fulldbg24 + loader/XNU payload) in the
phone's own stock boot image, as package_test69.py does for recovery: header v2, the stock ramdisk and DTB kept, the
header ID recomputed, an AVB hash footer (algorithm NONE) for a partition named boot. No log slot (the partition is 64 MB).

usage: python package_install_boot.py ROOT [N]    (ROOT = work/ios-r5q; N = the build whose kernel image goes in, default 70)
"""
import base64
import hashlib
import json
import pathlib
import struct
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
N = int(sys.argv[2]) if len(sys.argv) > 2 else 70
OUT = 'r5q-install-boot.img' if N == 70 else f'r5q-install-boot-{N}.img'
build = root / 'port/build'
stock = (root / 'backups/<SERIAL>/boot.img').read_bytes()
stock_hash = hashlib.sha256(stock).hexdigest()
assert stock_hash.startswith('381e6aa7'), stock_hash        # the baseline boot (run_stage_test_auto.ps1 baseline)
assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 2
page = struct.unpack_from('<I', stock, 36)[0]
assert page == 4096
align = lambda n: (n + page - 1) & ~(page - 1)
ks, rs, ss = [struct.unpack_from('<I', stock, n)[0] for n in [8, 16, 24]]
assert ss == 0
dtbo_size, dtbo_offset = struct.unpack_from('<IQ', stock, 1632)
dtb_size, dtb_addr = struct.unpack_from('<IQ', stock, 1648)
assert dtbo_size == 0 and dtbo_offset == 0                  # boot (unlike recovery) carries no DTBO
ramdisk = stock[page + align(ks):page + align(ks) + rs]
dtb_offset = page + align(ks) + align(rs)
dtb = stock[dtb_offset:dtb_offset + dtb_size]
assert dtb[:4] == b'\xd0\x0d\xfe\xed'
old_kernel = stock[page:page + ks]
old_id = hashlib.sha1()
for component in [old_kernel, ramdisk, b'', b'', dtb]:
    old_id.update(component)
    old_id.update(struct.pack('<I', len(component)))
assert old_id.digest() == stock[576:596], 'the stock header ID must validate before repacking'

kernel = (build / f'r5q-test{N}-kernel.bin').read_bytes()
assert kernel[56:60] == b'ARM\x64' and struct.unpack_from('<Q', kernel, 16)[0] == len(kernel)
header = bytearray(stock[:page])
struct.pack_into('<I', header, 8, len(kernel))
digest = hashlib.sha1()
for component in [kernel, ramdisk, b'', b'', dtb]:
    digest.update(component)
    digest.update(struct.pack('<I', len(component)))
header[576:608] = digest.digest() + bytes(12)
image = bytes(header)
for component in [kernel, ramdisk, dtb]:
    image += component + bytes(align(len(component)) - len(component))
image += b'SEANDROIDENFORCE'
assert len(image) + 2 * page < len(stock), 'the image must fit the 64 MB partition with its footer'
(build / 'r5q-install-boot-unsigned.img').write_bytes(image)
(build / 'expected-boot-ramdisk').write_bytes(ramdisk)
(build / 'expected-boot-dtb').write_bytes(dtb)

footer = struct.unpack_from('>4sIIQQQ', stock, len(stock) - 64)
assert footer[0] == b'AVBf'
vbmeta_offset = footer[4]
rollback_index = struct.unpack_from('>Q', stock, vbmeta_offset + 112)[0]
rollback_location = struct.unpack_from('>I', stock, vbmeta_offset + 124)[0]
script = f'''set -eu
cp /inputs/r5q-install-boot-unsigned.img /scratch/boot.img
python3 /android/avbtool.py add_hash_footer --image /scratch/boot.img \\
  --partition_size {len(stock)} --partition_name boot --algorithm NONE \\
  --hash_algorithm sha256 --salt {'00' * 32} --do_not_use_ab \\
  --rollback_index {rollback_index} --rollback_index_location {rollback_location}
python3 /android/avbtool.py verify_image --image /scratch/boot.img >&2
python3 /android/unpack_bootimg.py --boot_img /scratch/boot.img --out /scratch/unpacked >&2
cmp /scratch/unpacked/kernel /inputs/r5q-test{N}-kernel.bin
cmp /scratch/unpacked/ramdisk /inputs/expected-boot-ramdisk
cmp /scratch/unpacked/dtb /inputs/expected-boot-dtb
base64 -w0 /scratch/boot.img
'''
command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=120', '--nproc=1024', '--nofile=128', '--fsize=134217728', '--',
           '/usr/bin/bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64']:
    command += ['--ro-bind', path, path]
command += ['--proc', '/proc', '--dev', '/dev', '--size', '268435456', '--tmpfs', '/scratch',
            '--ro-bind', str(build), '/inputs', '--ro-bind', str(root / 'android-tools'), '/android',
            '--chdir', '/scratch', '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin',
            '--setenv', 'HOME', '/scratch', '--setenv', 'TMPDIR', '/scratch',
            '/usr/bin/timeout', '120', '/bin/sh', '-c', script]
result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=135)
(build / 'r5q-install-boot-validation.txt').write_bytes(result.stderr[-50000:])
if result.returncode:
    sys.stderr.buffer.write(result.stderr[-10000:])
    raise SystemExit(result.returncode)
final = base64.b64decode(result.stdout, validate=True)
assert len(final) == len(stock) and final[:8] == b'ANDROID!' and final[-64:-60] == b'AVBf'
destination = build / OUT
destination.write_bytes(final)
report = {'image': destination.name, 'bytes': len(final), 'sha256': hashlib.sha256(final).hexdigest(),
          'stock_boot_sha256': stock_hash, 'kernel_bytes': len(kernel), 'kernel_sha256': hashlib.sha256(kernel).hexdigest(),
          'ramdisk_bytes': rs, 'dtb_bytes': dtb_size, 'rollback_index': rollback_index}
(build / (OUT[:-4] + '-report.json')).write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
