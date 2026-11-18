"""dsc_callers.py CACHE IMAGE_NAME TARGET...: the bl/b instructions in an image's __text that branch to TARGET (hex VM addresses)."""
import struct, sys
sys.argv, cache, name, targets = sys.argv, sys.argv[1], sys.argv[2], [int(t, 16) for t in sys.argv[3:]]
exec(open(__file__.replace('dsc_callers.py', 'dsc_whose.py')).read().split("where = None")[0].replace("path, text = sys.argv[1], sys.argv[2].encode()", "path = cache"))
f = open(cache, 'rb')
h = f.read(0x200)
img_off, img_cnt = struct.unpack_from('<II', h, 0x18)
if img_cnt == 0:
    img_off, img_cnt = struct.unpack_from('<II', h, 0x1c0)
f.seek(img_off)
for _ in range(img_cnt):
    a, mt, ino, po, pad = struct.unpack('<QQQII', f.read(32))
    pos = f.tell(); f.seek(po); p = f.read(300).split(b'\0')[0].decode(); f.seek(pos)
    if not p.endswith('/' + name):
        continue
    hdr = mem(a, 0x8000)
    ncmds = struct.unpack_from('<I', hdr, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', hdr, off)
        if cmd == 0x19 and hdr[off + 8:off + 24].split(b'\0')[0] == b'__TEXT':
            for s in range(struct.unpack_from('<I', hdr, off + 64)[0]):
                so = off + 72 + 80 * s
                if hdr[so:so + 16].split(b'\0')[0] == b'__text':
                    va, sz = struct.unpack_from('<QQ', hdr, so + 32)
                    data = mem(va, sz)
                    for i in range(0, len(data), 4):
                        w = struct.unpack_from('<I', data, i)[0]
                        if (w & 0x7c000000) == 0x14000000:
                            imm = w & 0x3ffffff
                            if imm & (1 << 25):
                                imm -= 1 << 26
                            if va + i + imm * 4 in targets:
                                print('%s at %#x (image offset %#x) -> %#x' % ('bl' if w >> 31 else 'b', va + i, va + i - a, va + i + imm * 4))
        off += size
