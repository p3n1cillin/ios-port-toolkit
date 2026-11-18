"""dsc_fileoff.py CACHE ADDR: which file of a split dyld shared cache holds ADDR (unslid), at what offset, and its 4 bytes."""
import glob, struct, sys
path, addr = sys.argv[1], int(sys.argv[2], 16)
for sub in sorted(glob.glob(path + '*')):
    if sub.endswith('.symbols') or sub.endswith('.map'):
        continue
    f = open(sub, 'rb')
    h = f.read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', h, 0x10)
    f.seek(mo)
    for i in range(mc):
        a, size, off, maxp, initp = struct.unpack('<QQQII', f.read(32))
        if a <= addr < a + size:
            fo = off + addr - a
            f.seek(fo)
            print(sub.split('/')[-1], hex(fo), f.read(4)[::-1].hex(), 'size', __import__('os').path.getsize(sub))
