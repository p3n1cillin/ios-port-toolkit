#!/usr/bin/env python3
"""camfaketab.py OUTDIR: a synthetic camera sensor table for the kernel camera driver's cam=5 cam_fake=1 (2026-11-19).

The donor's camera driver (bsd/dev/redwood_camera.c) reads each sensor's modes from fw/camera-<sensor>.tab, which
tools/camsensor.py makes out of the vendor's sensor module file. With cam_fake=1 no register of a sensor is written:
only the modes are used, to size the colour chart that stands in for the sensor's frames. The S10 Lite has no such
files here (and the donor's sensors are not the S10 Lite's), so this writes a table with modes and no register writes,
in camsensor.py's format ("RWCTAB02"; see its docstring), under the names the driver's four sensors look for.

Nothing of any vendor is in it. Used by pipeline/build_root_app.sh (copied into the root's /fw).
"""
import os
import struct
import sys

# per mode: width, height, x, y, lanes, C-PHY, settle, output pixel clock, line length, frame length, fps x 1000,
# data type (0x2b = RAW10)
MODES = [
    (1920, 1080, 0, 0, 3, 1, 34, 480000000, 4000, 1100, 30000, 0x2b),
    (4000, 3000, 0, 0, 3, 1, 34, 480000000, 4500, 3100, 15000, 0x2b),
]
TABLES = ['init', 'res0', 'res1', 'on', 'off']


def table():
    out = b'RWCTAB02'
    # slave, address bytes, data bytes of the id read, id address, id, id mask, lane assignment, number of modes
    out += struct.pack('<8I', 0x34, 2, 2, 0, 0, 0, 0, len(MODES))
    for mode in MODES:
        out += struct.pack('<12I', *mode)
    for name in ('pwrup', 'pwrdn'):
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', 0)
    out += struct.pack('<I', len(TABLES))
    for name in TABLES:
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', 0)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    # (Test 268) every sensor the driver knows: iOS switches to the front camera at once, and a sensor without a
    # table made the driver give up for the rest of the run ("a sensor that cannot be used")
    for sensor in ('s5khm2sd', 'ov16a1q', 's5k4h7', 'ov02b10'):
        path = os.path.join(sys.argv[1], f'camera-{sensor}.tab')
        with open(path, 'wb') as f:
            f.write(table())
        print(f'{path}: {len(MODES)} modes, no register writes')


if __name__ == '__main__':
    main()
