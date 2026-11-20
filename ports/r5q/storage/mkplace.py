"""mkplace.py IMAGE OUT (2026-11-20): a formatted, sparse volume image (mkfs.hfsplus on the PC) as a placement for
the phone: every allocated range of the image (SEEK_DATA/SEEK_HOLE) as either zeros or 4 KiB blocks of content, for
/var/r5q/bin/hfsplace to write onto /dev/rufs_var (wifi/hfsplace.c). A 93 GB HFS+ volume is ~200 MB of metadata, almost
all zeros: the placement carries the zeros as ranges and only the blocks that hold something.
Format: b'HFSPLACE1' + u64 volume bytes, then records <u64 offset, u64 length, u32 kind (0 zeros, 1 content)> with
the content's bytes after a kind-1 record."""
import os
import struct
import sys

BLOCK = 4096
img, out = sys.argv[1], sys.argv[2]
size = os.path.getsize(img)
fd = os.open(img, os.O_RDONLY)
records = []
off = 0
while True:
    try:
        s = os.lseek(fd, off, os.SEEK_DATA)
    except OSError:
        break
    e = os.lseek(fd, s, os.SEEK_HOLE)
    pos = s
    zero_from = None
    while pos < e:
        os.lseek(fd, pos, 0)
        b = os.read(fd, BLOCK)
        if b.strip(b'\0'):
            if zero_from is not None:
                records.append((zero_from, pos - zero_from, 0, b''))
                zero_from = None
            records.append((pos, BLOCK, 1, b))
        elif zero_from is None:
            zero_from = pos
        pos += BLOCK
    if zero_from is not None:
        records.append((zero_from, e - zero_from, 0, b''))
    off = e
with open(out, 'wb') as f:
    f.write(b'HFSPLACE1' + struct.pack('<Q', size))
    for o, n, k, b in records:
        f.write(struct.pack('<QQI', o, n, k) + b)
zeros = sum(n for o, n, k, b in records if k == 0)
content = sum(n for o, n, k, b in records if k == 1)
print(f'{len(records)} records: {zeros >> 20} MB of zeros, {content // 1024} KB of content; {os.path.getsize(out)} bytes')
