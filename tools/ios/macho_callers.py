"""macho_callers.py FILE VMADDR...: the bl/b instructions in __TEXT of a thin arm64 Mach-O that branch to each VMADDR."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
targets = {int(t, 16) for t in sys.argv[2:]}
ncmds = struct.unpack_from('<I', d, 16)[0]
p = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, p)
    if cmd == 0x19 and d[p + 8:p + 24].rstrip(b'\0') == b'__TEXT':
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', d, p + 24)
        t = (vmaddr, fileoff, filesize)
    p += size
w = struct.unpack_from('<%dI' % (t[2] // 4), d, t[1])
for k, a in enumerate(w):
    if (a & 0x7c000000) == 0x14000000:
        imm = a & 0x3ffffff
        if imm & (1 << 25):
            imm -= 1 << 26
        to = t[0] + 4 * k + 4 * imm
        if to in targets:
            print('%s at vm %#x (file %#x) -> %#x' % ('bl' if a & 0x80000000 else 'b', t[0] + 4 * k, t[1] + 4 * k, to))
