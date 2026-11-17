"""Accept RAM only after excluding DT reservations and the measured UEFI map."""
import json
import pathlib
import re
import struct
import sys
from read_fdt import read_tree

def tuples(value, address_cells=2, size_cells=2):
    stride = (address_cells + size_cells) * 4
    if address_cells != 2 or size_cells != 2 or len(value) % stride:
        raise ValueError('Unexpected DT address/size cell layout')
    for offset in range(0, len(value), stride):
        high, low, size_high, size_low = struct.unpack_from('>4I', value, offset)
        yield (high << 32) | low, (size_high << 32) | size_low

def validate_window(root, base, size, firmware_map=None):
    root = pathlib.Path(root).resolve(strict=True)
    if base < 0 or size <= 0 or base + size > 1 << 64 or base % 0x2000000 or size % 0x2000000:
        raise ValueError('Window must be positive, non-wrapping and aligned to 32 MiB')
    dtb = root / 'backups/<SERIAL>/live.dtb'
    tree = read_tree(dtb)
    cells = tree['/']
    if struct.unpack('>I', cells['#address-cells'])[0] != 2 or struct.unpack('>I', cells['#size-cells'])[0] != 2:
        raise ValueError('Unsupported root memory cell layout')
    banks = []
    reservations = []
    for name, properties in tree.items():
        if properties.get('device_type', b'').rstrip(b'\0') == b'memory' and 'reg' in properties:
            banks.extend(tuples(properties['reg']))
        if name.startswith('/reserved-memory/') and 'reg' in properties:
            reservations.extend((name, lo, length) for lo, length in tuples(properties['reg']) if length)
    if not any(lo <= base and base + size <= lo + length for lo, length in banks):
        raise ValueError('Window is not fully inside one measured RAM bank')
    raw = dtb.read_bytes()
    total, offset = struct.unpack_from('>I', raw, 4)[0], struct.unpack_from('>I', raw, 16)[0]
    while offset + 16 <= total:
        lo, length = struct.unpack_from('>QQ', raw, offset)
        offset += 16
        if lo == 0 and length == 0:
            break
        reservations.append(('FDT reserve map', lo, length))
    else:
        raise ValueError('Unterminated FDT reserve map')
    if firmware_map is None:
        firmware_map = root / 'port/test5/MemoryMapLib.c'
        if not firmware_map.is_file():
            firmware_map = root / 'port/test6/MemoryMapLib.c'
    text = pathlib.Path(firmware_map).read_text()
    for name, address, length in re.findall(r'\{"([^"]+)",\s*(0x[0-9A-Fa-f]+)(?:ULL)?,\s*(0x[0-9A-Fa-f]+)(?:ULL)?', text):
        lo, length = int(address, 16), int(length, 16)
        if name != 'XNU Test RAM' and lo >= 0x80000000 and length:
            reservations.append(('UEFI: ' + name, lo, length))
    overlaps = [{'name': name, 'base': hex(lo), 'end': hex(lo + length)}
                for name, lo, length in reservations if base < lo + length and lo < base + size]
    if overlaps:
        raise ValueError('Reserved memory overlap: ' + json.dumps(overlaps))
    return {'result': 'passed', 'base': hex(base), 'bytes': size,
            'end': hex(base + size), 'reservation_count_checked': len(reservations),
            'checks': ['FDT RAM banks', 'reserved-memory fixed regions', 'FDT memreserve table',
                       'stock-derived UEFI map excluding the candidate itself', 'alignment and overflow']}

if __name__ == '__main__':
    print(json.dumps(validate_window(sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)), indent=2))
