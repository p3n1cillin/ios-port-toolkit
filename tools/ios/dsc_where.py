"""dsc_where.py CACHE SLIDE ADDR...: the image (and offset in it) of each slid address."""
import struct, sys
cache = sys.argv[1]
exec(open(__file__.replace('dsc_where.py', 'dsc_whose.py')).read().split("where = None")[0].replace("path, text = sys.argv[1], sys.argv[2].encode()", "path = cache"))
slide = int(sys.argv[2], 16)
f = open(cache, 'rb'); h = f.read(0x200)
img_off, img_cnt = struct.unpack_from('<II', h, 0x18)
if img_cnt == 0:
    img_off, img_cnt = struct.unpack_from('<II', h, 0x1c0)
f.seek(img_off)
imgs = []
for _ in range(img_cnt):
    a, mt, ino, po, pad = struct.unpack('<QQQII', f.read(32))
    pos = f.tell(); f.seek(po); p = f.read(300).split(b'\0')[0].decode(); f.seek(pos)
    imgs.append((a, p))
texts = []
for base, name in imgs:
    hdr = mem(base, 0x8000)
    if not hdr:
        continue
    off = 32
    for _ in range(struct.unpack_from('<I', hdr, 16)[0]):
        cmd, size = struct.unpack_from('<II', hdr, off)
        if cmd == 0x19 and hdr[off + 8:off + 24].split(b'\0')[0] == b'__TEXT':
            vma, vms = struct.unpack_from('<QQ', hdr, off + 24)
            texts.append((vma, vms, base, name))
        off += size
for a in sys.argv[3:]:
    u = int(a, 16) - slide
    hit = [t for t in texts if t[0] <= u < t[0] + t[1]]
    print('%s -> %#x %s' % (a, u, ('%s+%#x' % (hit[0][3].split('/')[-1], u - hit[0][2])) if hit else '?'))
