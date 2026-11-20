"""camraw.py FRAME [OUT.png] (2026-11-20): a raw frame from wifi/camgrab.c made into a picture on the PC, and measured.
The frame is the kernel's struct redwood_camera_frame header (wifi/redwood_camera.h) and then MIPI RAW10 lines (four
pixels in five bytes: four high bytes, then a byte with the four pairs of low bits), `stride` bytes apart. The picture is
half size: each 2x2 Bayer cell (order from the header) becomes one RGB pixel, black level taken off, scaled by its 99th
percentile. Printed: the header, how alike neighbouring lines are, and each column band's mean -- flat streaks or
noise at an edge in the Camera app say whether the sensor's frame is at fault or the rendering.
Needs numpy and Pillow. Frames hold what the camera saw: local only."""
import struct
import sys

import numpy as np
from PIL import Image

data = open(sys.argv[1], 'rb').read()
(magic, hb, w, h, stride, fmt, bayer, black, seq, t, exp_lines, line_ns, gain, unity) = struct.unpack_from(
    '<8s7I4x2Q4I', data, 0)
print(f'{magic!r} {w}x{h} stride {stride} format {fmt} bayer {bayer} black {black} frame {seq} '
      f'exposure {exp_lines} lines x {line_ns} ns, gain {gain}/{unity}')
raw = np.frombuffer(data, np.uint8, count=stride * h, offset=hb).reshape(h, stride)
groups = raw[:, :w // 4 * 5].reshape(h, w // 4, 5).astype(np.uint16)
px = np.empty((h, w // 4, 4), np.uint16)
for k in range(4):
    px[:, :, k] = (groups[:, :, k] << 2) | ((groups[:, :, 4] >> (2 * k)) & 3)
img = px.reshape(h, w).astype(np.float32) - black
print('levels: min %d, mean %.1f, max %d (of 1023)' % (px.min(), px.mean(), px.max()))
# neighbouring lines of the same colour (two apart): how much alike
d = np.abs(img[2:] - img[:-2]).mean(axis=1)
print('mean |line - line two above|: median %.1f, min %.1f, max %.1f' % (np.median(d), d.min(), d.max()))
cols = img.mean(axis=0)
bands = [c.mean() for c in np.array_split(cols, 10)]
print('column bands (left to right):', ' '.join('%.0f' % b for b in bands))
rows = img.mean(axis=1)
print('row bands (top to bottom):', ' '.join('%.0f' % c.mean() for c in np.array_split(rows, 10)))
off = {0: (0, 1, 2, 3), 1: (1, 0, 3, 2), 2: (2, 3, 0, 1), 3: (3, 2, 1, 0)}[bayer]   # positions of R, G1, G2, B
cells = [img[0::2, 0::2], img[0::2, 1::2], img[1::2, 0::2], img[1::2, 1::2]]
r, g1, g2, b = (cells[off.index(i)] for i in range(4))
rgb = np.stack([r, (g1 + g2) / 2, b], axis=2)
rgb = np.clip(rgb / max(np.percentile(rgb, 99), 1) * 255, 0, 255).astype(np.uint8)
out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1] + '.png'
Image.fromarray(rgb).save(out)
print('picture', out, rgb.shape[1], 'x', rgb.shape[0], 'mean RGB', rgb.reshape(-1, 3).mean(axis=0).round())
