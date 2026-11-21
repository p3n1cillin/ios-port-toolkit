"""camgate.py RAW [RAW...]: the IMX586's "gate" in camgrab frames (CAMERA-PLAN.md, after Test 414).

Each RAW is what wifi/camgrab.c writes: struct redwood_camera_frame, then the frame in RAW10 (4 pixels in 5 bytes).
The header's size, width, height and stride are read from the header itself (its first words, as camgrab prints them).
For each frame: the black level, the share of pixels above it, the lowest value above it (the gate: on a cold sensor
every pixel was either exactly 64 or above ~370), and how many pixels sit just above black (65-100), which a
working sensor has plenty of in a dim scene.
"""
import struct
import sys

import numpy as np


def frame(path):
    b = open(path, 'rb').read()
    # struct redwood_camera_frame (sys/redwood_camera.h): magic[8], header_bytes, width, height, stride, format,
    # bayer, black, then the rest
    hb, width, height, stride, fmt, bayer, black = struct.unpack_from('<7I', b, 8)
    raw = np.frombuffer(b, np.uint8, stride * height, hb).reshape(height, stride)[:, :width * 5 // 4]
    g = raw.reshape(height, width // 4, 5).astype(np.uint16)
    px = np.empty((height, width // 4, 4), np.uint16)
    for i in range(4):
        px[:, :, i] = (g[:, :, i] << 2) | ((g[:, :, 4] >> (2 * i)) & 3)
    return px.reshape(height, width), black, width, height


for path in sys.argv[1:]:
    p, black, w, h = frame(path)
    bl = black or 64
    above = p[p > bl]
    low = ((p > bl) & (p <= bl + 36)).mean() * 100
    print(f'{path}: {w}x{h}, black {black}; above black {len(above) * 100 / p.size:.2f}%, '
          f'lowest above black {int(above.min()) if len(above) else "-"}, median {int(np.median(p))}, '
          f'just above black (to +36) {low:.2f}%, mean {p.mean():.1f}')
