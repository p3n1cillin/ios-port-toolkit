"""dsc_cdpatch.py SUBCACHE OFFSET OLDWORD NEWWORD: the code-signature changes that go with changing one 32-bit word of a
dyld subcache file (each CodeDirectory's hash of the page holding it), printed as content patches (file offset, old, new)."""
import hashlib, struct, sys
path, off, old, new = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
f = open(path, 'rb')
data_hdr = f.read(0x40)
cs_off, cs_size = struct.unpack_from('<QQ', data_hdr, 0x28)
f.seek(off); assert struct.unpack('<I', f.read(4))[0] == old, 'old word differs'
f.seek(cs_off); blob = f.read(cs_size)
magic, length, count = struct.unpack_from('>III', blob, 0)
print('superblob magic %#x length %d count %d at file %#x' % (magic, length, count, cs_off))
for i in range(count):
    typ, boff = struct.unpack_from('>II', blob, 12 + 8 * i)
    m = struct.unpack_from('>I', blob, boff)[0]
    if m != 0xfade0c02:
        print(' blob type %#x magic %#x (not a CodeDirectory)' % (typ, m)); continue
    (mg, ln, ver, flags, hashOffset, identOffset, nSpecial, nCode, codeLimit, hashSize, hashType, platform, pageSize) = \
        struct.unpack_from('>IIIIIIIIIBBBB', blob, boff)
    if ver >= 0x20400:
        codeLimit64 = struct.unpack_from('>Q', blob, boff + 0x38)[0]
    ps = 1 << pageSize
    page = off // ps
    f.seek(page * ps); content = bytearray(f.read(min(ps, codeLimit - page * ps)))
    hf = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}[hashType]
    slot = boff + hashOffset + page * hashSize
    have = blob[slot:slot + hashSize]
    calc = hf(bytes(content)).digest()[:hashSize]
    content[off - page * ps:off - page * ps + 4] = struct.pack('<I', new)
    newh = hf(bytes(content)).digest()[:hashSize]
    print(' CD type %#x version %#x hashType %d hashSize %d pageSize %d nCode %d codeLimit %#x; page %d hash %s (computed %s)'
          % (typ, ver, hashType, hashSize, ps, nCode, codeLimit, page, have.hex(), 'matches' if calc == have else 'DIFFERS ' + calc.hex()))
    fo = cs_off + slot
    print(' PATCH file %#x len %d old %s new %s (4K-page-straddle: %s)' % (fo, hashSize, have.hex(), newh.hex(),
          (fo // 4096) != ((fo + hashSize - 1) // 4096)))
