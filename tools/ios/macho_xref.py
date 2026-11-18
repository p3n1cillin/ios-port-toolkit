"""macho_xref.py FILE TEXT: the vmaddr of a C string in a thin arm64 Mach-O, and every adrp+add that reaches it."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
text = sys.argv[2].encode()
ncmds = struct.unpack_from('<I', d, 16)[0]
p, segs = 32, []
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, p)
    if cmd == 0x19:
        name = d[p + 8:p + 24].rstrip(b'\0')
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', d, p + 24)
        segs.append((name, vmaddr, vmsize, fileoff, filesize))
    p += size
def f2v(o):
    for n, va, vs, fo, fs in segs:
        if fo <= o < fo + fs:
            return va + o - fo
def v2f(v):
    for n, va, vs, fo, fs in segs:
        if va <= v < va + fs:
            return fo + v - va
i = d.find(text)
assert i >= 0
while d[i - 1] != 0:
    i = d.find(text, i + 1)
target = f2v(i)
print('string at file %#x vm %#x' % (i, target))
tseg = [s for s in segs if s[0] == b'__TEXT'][0]
w = struct.unpack_from('<%dI' % (tseg[4] // 4), d, tseg[3])
for k in range(len(w) - 1):
    a = w[k]
    if (a & 0x9f000000) != 0x90000000:
        continue
    rd = a & 31
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    pc = tseg[1] + 4 * k
    page = (pc & ~0xfff) + (imm << 12)
    for j in range(k + 1, min(k + 4, len(w))):
        b = w[j]
        if (b & 0xff800000) == 0x91000000 and ((b >> 5) & 31) == rd:
            if page + ((b >> 10) & 0xfff) == target:
                print('used at vm %#x (file %#x)' % (pc, tseg[3] + 4 * k))
            break
