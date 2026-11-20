"""dsc_global_writers.py CACHE START_HEX LENGTH_HEX GLOBAL_HEX: the code in [START, START+LENGTH) of a split dyld shared
cache that stores to a 64-bit global: an `adrp xN, page` followed (within 6 instructions) by `str xT, [xN, #off]` where
page+off is GLOBAL. Lists who sets a library's static (2026-11-20: who makes Metal's _deviceArray, which MTLAddDevice
asserts on)."""
import glob, struct, sys

path, start, length, glob_addr = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)

for sub in sorted(glob.glob(path + '*')):
    if sub.endswith('.symbols') or sub.endswith('.map'):
        continue
    f = open(sub, 'rb')
    h = f.read(0x200)
    off, cnt = struct.unpack_from('<II', h, 0x10)
    f.seek(off)
    maps = [struct.unpack_from('<QQQ', f.read(32)) for _ in range(cnt)]
    for addr, size, fileoff in maps:
        lo, hi = max(addr, start), min(addr + size, start + length)
        if lo >= hi:
            continue
        f.seek(fileoff + lo - addr)
        data = f.read(hi - lo)
        words = struct.unpack_from('<%dI' % (len(data) // 4), data)
        for i, w in enumerate(words):
            if (w & 0x9f000000) != 0x90000000:
                continue
            pc = lo + 4 * i
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20):
                imm -= 1 << 21
            page = (pc & ~0xfff) + (imm << 12)
            rd = w & 31
            for j in range(1, 7):
                if i + j >= len(words):
                    break
                v = words[i + j]
                if (v & 0xffc00000) == 0xf9000000 and ((v >> 5) & 31) == rd:      # str xT, [xN, #imm12*8]
                    if page + ((v >> 10) & 0xfff) * 8 == glob_addr:
                        print('0x%x: store at 0x%x' % (pc, pc + 4 * j))
                    break
