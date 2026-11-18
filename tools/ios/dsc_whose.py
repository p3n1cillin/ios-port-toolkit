"""dsc_whose.py CACHE TEXT: the VM address of TEXT (a C string) in a split dyld shared cache and the image whose __TEXT holds it,
then every adrp+add in that image that reaches it (the code that uses the string)."""
import glob, struct, subprocess, sys, tempfile

path, text = sys.argv[1], sys.argv[2].encode()
subs = []
for sub in sorted(glob.glob(path + '*')):
    if sub.endswith('.map') or sub.endswith('.symbols'):
        continue
    with open(sub, 'rb') as g:
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


where = None
for a, sz, fo, sub in subs:
    with open(sub, 'rb') as g:
        g.seek(fo)
        data = g.read(sz)
    i = data.find(text)
    if i >= 0:
        while i > 0 and data[i - 1] != 0:
            i -= 1
        where = a + i
        print('string at %#x: %r' % (where, data[i:data.find(b'\0', i)]))
        break
if where is None:
    sys.exit('not found')

f = open(path, 'rb')
h = f.read(0x200)
img_off, img_cnt = struct.unpack_from('<II', h, 0x18)
if img_cnt == 0:
    img_off, img_cnt = struct.unpack_from('<II', h, 0x1c0)
f.seek(img_off)
images = []
for _ in range(img_cnt):
    a, mt, ino, po, pad = struct.unpack('<QQQII', f.read(32))
    pos = f.tell()
    f.seek(po)
    p = f.read(300).split(b'\0')[0].decode()
    f.seek(pos)
    images.append((a, p))
for base, name in images:
    hdr = mem(base, 0x8000)
    if hdr is None:
        continue
    ncmds = struct.unpack_from('<I', hdr, 16)[0]
    off = 32
    text_seg = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', hdr, off)
        if cmd == 0x19:
            segname = hdr[off + 8:off + 24].split(b'\0')[0]
            vma, vms = struct.unpack_from('<QQ', hdr, off + 24)
            if segname == b'__TEXT':
                text_seg = (vma, vms)
                nsects = struct.unpack_from('<I', hdr, off + 64)[0]
                sects = []
                for s in range(nsects):
                    so = off + 72 + 80 * s
                    sects.append((hdr[so:so + 16].split(b'\0')[0], *struct.unpack_from('<QQ', hdr, so + 32)))
        off += size
    if text_seg and text_seg[0] <= where < text_seg[0] + text_seg[1]:
        print('image', name, 'base %#x' % base)
        code = [s for s in sects if s[0] == b'__text'][0]
        data = mem(code[1], code[2])
        page = where & ~0xfff
        for i in range(0, len(data) - 8, 4):
            w = struct.unpack_from('<I', data, i)[0]
            if (w & 0x9f000000) == 0x90000000:            # adrp
                rd = w & 31
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                if imm & (1 << 20):
                    imm -= 1 << 21
                pc = code[1] + i
                if (pc & ~0xfff) + (imm << 12) == page:
                    w2 = struct.unpack_from('<I', data, i + 4)[0]
                    if (w2 & 0xffc00000) == 0x91000000 and ((w2 >> 22) & 0x3ff) == 0 or (w2 & 0xff800000) == 0x91000000:
                        if ((w2 >> 10) & 0xfff) == (where & 0xfff):
                            print('  used at %#x (image offset %#x)' % (pc, pc - base))
        break
