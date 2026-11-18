"""Wrap Test 216 using the owner's verified Android-v2 recovery components."""
import base64
import hashlib
import json
import pathlib
import struct
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
from validate_memory_window import validate_window
validate_window(root, 0xe0000000, 0x10000000)
build = root / 'port/build'
stock = (root / 'backups/<SERIAL>/recovery.img').read_bytes()
stock_hash = hashlib.sha256(stock).hexdigest()
assert stock_hash == '332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd'
assert stock[:8] == b'ANDROID!' and struct.unpack_from('<I', stock, 40)[0] == 2
page = struct.unpack_from('<I', stock, 36)[0]
assert page == 4096
align = lambda n: (n + page - 1) & ~(page - 1)
ks, rs, ss = [struct.unpack_from('<I', stock, n)[0] for n in [8, 16, 24]]
assert ss == 0
dtbo_size, dtbo_offset = struct.unpack_from('<IQ', stock, 1632)
dtb_size, dtb_addr = struct.unpack_from('<IQ', stock, 1648)
assert dtbo_offset == page + align(ks) + align(rs)
ramdisk = stock[page + align(ks):page + align(ks) + rs]
dtbo = stock[dtbo_offset:dtbo_offset + dtbo_size]
dtb_offset = dtbo_offset + align(dtbo_size)
dtb = stock[dtb_offset:dtb_offset + dtb_size]
assert len(ramdisk) == rs and len(dtbo) == dtbo_size and len(dtb) == dtb_size
assert dtb[:4] == b'\xd0\x0d\xfe\xed'
old_kernel = stock[page:page + ks]
old_id = hashlib.sha1()
for component in [old_kernel, ramdisk, b'', dtbo, dtb]:
    old_id.update(component)
    old_id.update(struct.pack('<I', len(component)))
assert old_id.digest() == stock[576:596], 'Original header ID must validate before repacking'

kernel = (build / 'r5q-test216-kernel.bin').read_bytes()
assert kernel[56:60] == b'ARM\x64'
assert struct.unpack_from('<Q', kernel, 8)[0] == struct.unpack_from('<Q', old_kernel, 8)[0]
assert struct.unpack_from('<Q', kernel, 16)[0] == len(kernel)
header = bytearray(stock[:page])
struct.pack_into('<I', header, 8, len(kernel))
struct.pack_into('<Q', header, 1636, page + align(len(kernel)) + align(rs))
digest = hashlib.sha1()
for component in [kernel, ramdisk, b'', dtbo, dtb]:
    digest.update(component)
    digest.update(struct.pack('<I', len(component)))
header[576:608] = digest.digest() + bytes(12)
image = bytes(header)
for component in [kernel, ramdisk, dtbo, dtb]:
    image += component + bytes(align(len(component)) - len(component))
image += b'SEANDROIDENFORCE'
unsigned = build / 'r5q-test216-unsigned.img'
unsigned.write_bytes(image)
for name, component in [('expected-ramdisk', ramdisk), ('expected-dtbo', dtbo), ('expected-dtb', dtb)]:
    (build / name).write_bytes(component)

footer = struct.unpack_from('>4sIIQQQ', stock, len(stock) - 64)
assert footer[0] == b'AVBf'
vbmeta_offset = footer[4]
assert stock[vbmeta_offset:vbmeta_offset + 4] == b'AVB0'
rollback_index = struct.unpack_from('>Q', stock, vbmeta_offset + 112)[0]
rollback_location = struct.unpack_from('>I', stock, vbmeta_offset + 124)[0]

script = f'''set -eu
cp /inputs/r5q-test216-unsigned.img /scratch/recovery.img
python3 /android/avbtool.py add_hash_footer --image /scratch/recovery.img \\
  --partition_size {len(stock)} --partition_name recovery --algorithm NONE \\
  --hash_algorithm sha256 --salt {'00' * 32} --do_not_use_ab \\
  --rollback_index {rollback_index} --rollback_index_location {rollback_location}
python3 /android/avbtool.py verify_image --image /scratch/recovery.img >&2
python3 /android/avbtool.py info_image --image /scratch/recovery.img >&2
python3 /android/unpack_bootimg.py --boot_img /scratch/recovery.img --out /scratch/unpacked >&2
cmp /scratch/unpacked/kernel /inputs/r5q-test216-kernel.bin
cmp /scratch/unpacked/ramdisk /inputs/expected-ramdisk
cmp /scratch/unpacked/recovery_dtbo /inputs/expected-dtbo
cmp /scratch/unpacked/dtb /inputs/expected-dtb
base64 -w0 /scratch/recovery.img
'''
command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=120', '--nproc=1024',
           '--nofile=128', '--fsize=134217728', '--', '/usr/bin/bwrap', '--unshare-all',
           '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64']:
    command += ['--ro-bind', path, path]
command += ['--proc', '/proc', '--dev', '/dev', '--size', '268435456', '--tmpfs', '/scratch',
            '--ro-bind', str(build), '/inputs', '--ro-bind', str(root / 'android-tools'), '/android',
            '--chdir', '/scratch', '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin',
            '--setenv', 'HOME', '/scratch', '--setenv', 'TMPDIR', '/scratch',
            '/usr/bin/timeout', '120', '/bin/sh', '-c', script]
result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=135)
(build / 'r5q-test216-image-validation.txt').write_bytes(result.stderr[-50000:])
if result.returncode:
    sys.stderr.buffer.write(result.stderr[-10000:])
    raise SystemExit(result.returncode)
final = base64.b64decode(result.stdout, validate=True)
assert len(final) == len(stock) and final[:8] == b'ANDROID!' and final[-64:-60] == b'AVBf'
# Test 216 log slot: the firmware saves its RAM log here (header block + 2 MiB) only if it finds this magic
SLOT, SLOT_BYTES = 0x4000000, 513 * 4096
fmagic, fmaj, fmin, covered, vb_off, vb_size = struct.unpack_from('>4sIIQQQ', final, len(final) - 64)
assert fmagic == b'AVBf' and covered <= vb_off and vb_off + vb_size <= SLOT, 'slot must follow the signed data'
assert SLOT + SLOT_BYTES <= len(final) - 4096, 'slot must end before the AVB footer block'
assert final[SLOT:SLOT + SLOT_BYTES] == bytes(SLOT_BYTES), 'slot must lie in zero padding'
slot_header = b'R5QLOGSLOT/V1' + bytes(3) + hashlib.sha256(final).digest()[:16]
final = final[:SLOT] + slot_header + final[SLOT + len(slot_header):]
destination = build / 'r5q-test216-recovery.img'
destination.write_bytes(final)
report = {'image': destination.name, 'bytes': len(final), 'sha256': hashlib.sha256(final).hexdigest(),
          'stock_recovery_sha256': stock_hash, 'header_version': 2, 'page_size': page,
          'kernel_bytes': len(kernel), 'ramdisk_bytes': rs, 'dtbo_bytes': dtbo_size, 'dtb_bytes': dtb_size,
          'avb_algorithm': 'NONE', 'rollback_index': rollback_index, 'rollback_index_location': rollback_location,
          'log_slot_offset': SLOT, 'log_slot_bytes': SLOT_BYTES, 'log_slot_nonce': slot_header[16:].hex()}
(build / 'r5q-test216-image-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
