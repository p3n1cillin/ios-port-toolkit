"""dsc_strings.py CACHE IMAGE REGEX: C strings in an image's __TEXT (__cstring, and since 2026-11-20 __oslogstring: os_log formats) that match REGEX (split cache aware)."""
import glob, re, struct, sys

path, name, pattern = sys.argv[1], sys.argv[2], re.compile(sys.argv[3].encode(), re.I)
subs = []
for sub in sorted(glob.glob(path + '*')):
    g = open(sub, 'rb')
    hh = g.read(0x20)
    if not hh.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', hh, 0x10)
    g.seek(mo)
    for i in range(mc):
        a, sz, fo, mx, ini = struct.unpack('<QQQII', g.read(32))
        subs.append((a, sz, fo, sub))


def mem(addr, n):
    for a, sz, fo, sub in subs:
        if a <= addr < a + sz:
            with open(sub, 'rb') as g:
                g.seek(fo + addr - a)
                return g.read(min(n, a + sz - addr))
    return None


f = open(path, 'rb')
h = f.read(0x200)
img_off, img_cnt = struct.unpack_from('<II', h, 0x18)
if img_cnt == 0:
    img_off, img_cnt = struct.unpack_from('<II', h, 0x1c0)
f.seek(img_off)
base = None
for _ in range(img_cnt):
    a, mt, ino, po, pad = struct.unpack('<QQQII', f.read(32))
    pos = f.tell()
    f.seek(po)
    p = f.read(300).split(b'\0')[0].decode()
    f.seek(pos)
    if p.endswith('/' + name):
        base = a
        break
hdr = mem(base, 0x8000)
ncmds = struct.unpack_from('<I', hdr, 16)[0]
off = 32
seen = set()
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', hdr, off)
    if cmd == 0x19:     # LC_SEGMENT_64
        segname = hdr[off + 8:off + 24].split(b'\0')[0]
        nsects = struct.unpack_from('<I', hdr, off + 64)[0]
        for s in range(nsects):
            so = off + 72 + 80 * s
            sect = hdr[so:so + 16].split(b'\0')[0]
            addr, sz = struct.unpack_from('<QQ', hdr, so + 32)
            if sect in (b'__cstring', b'__objc_methname', b'__objc_classname', b'__oslogstring'):
                data = mem(addr, sz) or b''
                for st in data.split(b'\0'):
                    if pattern.search(st) and st not in seen:
                        seen.add(st)
                        print(sect.decode(), st.decode('latin1')[:160])
    off += size
