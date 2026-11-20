"""dsc_ivar_users.py CACHE START_HEX LENGTH_HEX SLOT_HEX...: the code in [START, START+LENGTH) of a split dyld shared
cache that reads an ObjC ivar offset slot: an `adrp xN, page` followed (within 4 instructions) by `ldrsw xM, [xN, #off]`
where page+off is SLOT. Compiled ObjC reaches an ivar as base + *slot, so this lists every method that touches it.
(2026-11-20, Test 394: who fills BWAudioSourceNode's queue of audio buffers in CMCapture.)"""
import glob, struct, sys

path, start, length = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
slots = {int(s, 16) for s in sys.argv[4:]}


def mapping(sub):
    f = open(sub, 'rb')
    h = f.read(0x200)
    off, cnt = struct.unpack_from('<II', h, 0x10)
    f.seek(off)
    out = []
    for i in range(cnt):
        addr, size, fileoff = struct.unpack_from('<QQQ', f.read(32))
        out.append((addr, size, fileoff))
    return f, out


for sub in sorted(glob.glob(path + '*')):
    if sub.endswith('.symbols') or sub.endswith('.map'):
        continue
    f, maps = mapping(sub)
    for addr, size, fileoff in maps:
        lo, hi = max(addr, start), min(addr + size, start + length)
        if lo >= hi:
            continue
        f.seek(fileoff + lo - addr)
        data = f.read(hi - lo)
        words = struct.unpack_from('<%dI' % (len(data) // 4), data)
        for i, w in enumerate(words):
            if (w & 0x9f000000) != 0x90000000:          # adrp
                continue
            pc = lo + 4 * i
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20):
                imm -= 1 << 21
            page = (pc & ~0xfff) + (imm << 12)
            rd = w & 31
            for j in range(1, 5):
                if i + j >= len(words):
                    break
                v = words[i + j]
                if (v & 0xffc00000) == 0xb9800000 and ((v >> 5) & 31) == rd:     # ldrsw xT, [xN, #imm12*4]
                    target = page + ((v >> 10) & 0xfff) * 4
                    if target in slots:
                        print('0x%x (+0x%x): slot 0x%x' % (pc, pc - start, target))
                    break
