"""dsc_target.py CACHE ADDR...: where the bl/b at each unslid ADDR of a split dyld shared cache really goes -- through
stub islands (adrp x16 / add x16 / br x16) and __auth_stubs-style (adrp x16 / ldr x16 / br x16, pointer read from the
cache) -- printed with the image and offset it lands in.
(2026-11-20, Test 372: a bl offset added by hand went to the wrong capability function; compute it, never by hand.)"""
import struct, subprocess, sys

cache = sys.argv[1]
here = __file__.replace('dsc_target.py', '')


def words(addr, n):
    out = subprocess.run([sys.executable, '-I', here + 'dsc_words.py', cache, '%x' % addr, str(n)],
                         capture_output=True, text=True).stdout.strip().splitlines()[-1]
    return [int(w, 16) for w in out.split()]


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def branch(addr, w):
    if (w & 0x7c000000) == 0x14000000:          # b / bl
        return addr + sx(w & 0x3ffffff, 26) * 4
    return None


def stub(addr):
    """the final address of a stub at addr, or None if addr is not a stub"""
    w = words(addr, 3)
    if (w[0] & 0x9f00001f) != 0x90000010 or w[2] != 0xd61f0200:      # adrp x16 ... br x16
        return None
    imm = sx(((w[0] >> 29) & 3) | (((w[0] >> 5) & 0x7ffff) << 2), 21)
    page = (addr & ~0xfff) + (imm << 12)
    if (w[1] & 0xffc003ff) == 0x91000210:                             # add x16, x16, #imm
        return page + ((w[1] >> 10) & 0xfff)
    if (w[1] & 0xffc003ff) == 0xf9400210:                             # ldr x16, [x16, #imm]
        p = words(page + ((w[1] >> 10) & 0xfff) * 8, 2)
        return (p[0] | (p[1] << 32)) & 0xfffffffff                    # chained pointer: target in the low 36 bits
    return None


for a in sys.argv[2:]:
    addr = int(a, 16)
    t = branch(addr, words(addr, 1)[0])
    if t is None:
        print('%x: not a b/bl' % addr)
        continue
    hops = ['%x' % t]
    for _ in range(4):
        n = stub(t)
        if n is None:
            break
        t = n
        hops.append('%x' % t)
    where = subprocess.run([sys.executable, '-I', here + 'dsc_where.py', cache, '0', '%x' % t],
                           capture_output=True, text=True).stdout.strip().splitlines()
    print('%x -> %s   (%s)' % (addr, ' -> '.join(hops), where[-1] if where else '?'))
