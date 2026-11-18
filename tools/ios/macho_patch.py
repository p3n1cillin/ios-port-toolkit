"""macho_patch.py IN OUT OFFSET OLDWORD NEWWORD: a copy of a thin arm64 Mach-O with one 32-bit word changed and the
page hash of every CodeDirectory in its signature updated to match (an ad hoc signature stays valid page by page)."""
import hashlib, struct, sys
src, dst, off, old, new = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16), int(sys.argv[5], 16)
d = bytearray(open(src, 'rb').read())
assert struct.unpack_from('<I', d, 0)[0] == 0xfeedfacf
assert struct.unpack_from('<I', d, off)[0] == old, 'old word differs'
ncmds = struct.unpack_from('<I', d, 16)[0]
p = 32
cs = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, p)
    if cmd == 0x1d:
        cs = struct.unpack_from('<II', d, p + 8)
    p += size
assert cs, 'no LC_CODE_SIGNATURE'
struct.pack_into('<I', d, off, new)
base = cs[0]
magic, length, count = struct.unpack_from('>III', d, base)
for i in range(count):
    typ, boff = struct.unpack_from('>II', d, base + 12 + 8 * i)
    if struct.unpack_from('>I', d, base + boff)[0] != 0xfade0c02:
        continue
    cd = base + boff
    hashOffset, nCode, codeLimit, hashSize, hashType, pageSize = (struct.unpack_from('>I', d, cd + 16)[0],
        struct.unpack_from('>I', d, cd + 28)[0], struct.unpack_from('>I', d, cd + 32)[0], d[cd + 36], d[cd + 37], d[cd + 39])
    ps = 1 << pageSize
    page = off // ps
    hf = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}[hashType]
    slot = cd + hashOffset + page * hashSize
    end = min((page + 1) * ps, codeLimit)
    orig = bytearray(d[page * ps:end]); struct.pack_into('<I', orig, off - page * ps, old)
    assert hf(bytes(orig)).digest()[:hashSize] == bytes(d[slot:slot + hashSize]), 'original page hash differs'
    d[slot:slot + hashSize] = hf(bytes(d[page * ps:end])).digest()[:hashSize]
    print('CD type %#x hashType %d pageSize %d: page %d hash updated' % (typ, hashType, ps, page))
open(dst, 'wb').write(d)
