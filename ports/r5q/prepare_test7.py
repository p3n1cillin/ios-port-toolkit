"""Test 6: an early real XNU checkpoint, using measured ordinary RAM."""
import hashlib
import json
import pathlib
import shutil
import struct
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
from validate_memory_window import validate_window
validate_window(root, 0xd0000000, 0x20000000)
port = root / 'port'
test = port / 'test7'
test.mkdir(exist_ok=True)
for name in ['FirstBootManager.c', 'FirstBootManager.inf', 'BootShim.S', 'reboot-recovery.S']:
    text = (port / 'test5' / name).read_text().replace('[TEST5]', '[TEST7]').replace('TEST 5', 'TEST 7')
    (test / name).write_text(text)
memory = json.loads((root / 'hardware-map.json').read_text())['/memory']['reg']
cells = [int(x, 16) for x in memory]
banks = [((cells[i] << 32) | cells[i+1], (cells[i+2] << 32) | cells[i+3]) for i in range(0, len(cells), 4)]
lo, size = 0xD0000000, 0x20000000
assert any(base <= lo and lo + size <= base + length for base, length in banks)

kernel = pathlib.Path('/var/tmp/codex-ios-r5q/work/linux-source/prebuilt/mach.development.bcm2837').read_bytes()
assert hashlib.sha256(kernel).hexdigest() == '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
magic, cpu, subtype, kind, count, command_size, flags, reserved = struct.unpack_from('<8I', kernel)
assert magic == 0xfeedfacf and cpu == 0x100000c and kind == 2
assert 0 < count < 256 and 32 + command_size <= len(kernel)
offset = 32
segments = []
entry = None
for index in range(count):
    cmd, length = struct.unpack_from('<II', kernel, offset)
    assert length >= 8 and offset + length <= 32 + command_size
    if cmd == 0x19:
        assert length >= 72
        name, va, vs, fileoff, filesize = struct.unpack_from('<16sQQQQ', kernel, offset + 8)
        assert fileoff + filesize <= len(kernel) and filesize <= vs
        if vs:
            segments.append({'name': name.rstrip(b'\0').decode(), 'va': hex(va), 'size': vs,
                             'file_offset': fileoff, 'file_size': filesize})
    elif cmd == 5:
        assert length >= 16 + 33 * 8
        entry = struct.unpack_from('<Q', kernel, offset + 16 + 32 * 8)[0]
    offset += length
assert offset == 32 + command_size and entry is not None
assert kernel.count(b'XNULDRST') == 1
low = min(int(s['va'], 16) for s in segments)
high = max(int(s['va'], 16) + s['size'] for s in segments)
assert high - low + 0x8000000 < size
assert any(int(s['va'], 16) <= entry < int(s['va'], 16) + s['size'] for s in segments)
(port / 'build/test7-kernel-input.json').write_text(json.dumps({'kernel_sha256': hashlib.sha256(kernel).hexdigest(),
    'kernel_bytes': len(kernel), 'entry_va': hex(entry), 'segments': segments,
    'ram_banks': [{'base': hex(b), 'bytes': s} for b, s in banks],
    'uefi_free_window': {'base': hex(lo), 'bytes': size},
    'checkpoint': 1, 'userland': 'none; early assembly checkpoint only'}, indent=2) + '\n')
(port / 'build/test7-xnu.macho').write_bytes(kernel)
mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
board = mu / 'Platforms/Samsung/r5qtestPkg'
path = board / 'Library/MemoryMapLib/MemoryMapLib.c'
text = path.read_text()
marker = '  {"XNU Test RAM", 0xD0000000ULL, 0x20000000ULL, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK},'
if marker not in text:
    text = '\n'.join(line for line in text.split('\n') if '\"XNU Test RAM\"' not in line)
    text = text.replace('\n};', '\n' + marker + '\n};', 1)
path.write_text(text)
shutil.copyfile(path, test / 'MemoryMapLib.c')
for name in ['FirstBootManager.c', 'FirstBootManager.inf']:
    shutil.copyfile(test / name, board / 'Library/FirstBootManager' / name)
print(f'Validated pinned XNU Mach-O, {len(kernel)} bytes, entry {entry:#x}; measured 512 MiB RAM window')
