#!/usr/bin/env python3
"""camtuning_v14.py: a camera's colour tuning, as libredwood-media's processor reads it ("RWCISP02",
userland/src/camera_isp.c isp_load_tuning; the donor's tools/camtuning.py writes the same), out of a
"QTI Chromatix Header / Parameter Parser V1.4.0" tuned file -- the Galaxy S10 Lite's
/vendor/lib64/camera/com.samsung.tuned.<maker>_<sensor>.bin (the vendor's: kept in fw-camera-r5q/ and never in
the repository; so is what this makes of it).

usage: camtuning_v14.py TUNED.bin OUT.isp     (camtuning_v14.py TUNED.bin -   lists what is there only)

The V1.4.0 layout (worked out 2026-11-19 on com.samsung.tuned.lsi_s5k3l6.bin; the module files' is in
camsensor_v14.py):
  - entry table at the u32 at 0x98 (count at 0x9c), each {id, offset, size} u64; entry 0 the symbols, entry 1 the
    pool; a symbol is {id u64, name[32], kind, parent, offset in the pool, size} (72 bytes);
  - the TREE records have 8-byte fields (a float in the low 4 bytes): mod_<m>_led_idx_data {flash index, regions,
    the light ranges' id}; mod_<m>_aec_data per region {lux from, lux to, two more, ranges, the CCT ranges' id}
    (48 bytes);
  - the DATA (mod_<m>_cct_data) is packed: per range {CCT from, CCT to: 8 bytes each} then the values as 4-byte
    floats: cc13 9 (a 3x3 matrix) and 5 more words; lsc34 4 x 17 x 13 (R, Gr, Gb, B gain grids);
  - refPtV1: six 8-byte words, then the reference lights' R/G and B/G as 8-byte floats.
On the S5K3L6 the cc13 matrices are all identity (Samsung's pipeline colour-corrects elsewhere): the lens shading
and the AWB's reference lights are what this file brings.
"""
import struct
import sys

GRID = 17 * 13
# the colour temperature each of the ten reference lights picks the tables by (the donor's camtuning.py)
REFERENCE_CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)


def symbols(data):
    table, count = struct.unpack_from('<II', data, 0x98)
    entries = [struct.unpack_from('<3Q', data, table + 24 * i) for i in range(count)]
    sym_at, sym_size = entries[0][1], entries[0][2]
    pool = entries[1][1]
    syms = {}
    for at in range(sym_at, sym_at + sym_size - 71, 72):
        sid = struct.unpack_from('<Q', data, at)[0]
        name = data[at + 8:at + 40].split(b'\0')[0].decode('latin1')
        _, _, ref, size = struct.unpack_from('<4q', data, at + 40)
        syms[sid] = (name, data[pool + ref:pool + ref + size])
    return syms


def f8(raw, i):
    """the float in the low half of the i-th 8-byte field"""
    return struct.unpack_from('<f', raw, 8 * i)[0]


def u8(raw, i):
    return struct.unpack_from('<Q', raw, 8 * i)[0]


def module(syms, name, floats):
    """[(lux from, lux to, [(cct from, cct to, values)])] of the module's first tree, without the flash"""
    led = next(v for n, v in syms.values() if n == 'mod_%s_led_idx_data' % name)
    entries = [(u8(led, 3 * k), u8(led, 3 * k + 1), u8(led, 3 * k + 2)) for k in range(len(led) // 24)]
    _, regions, aec_sid = next(e for e in entries if e[0] == 0)
    aec = syms[aec_sid][1]
    out = []
    for i in range(regions):
        rec = aec[48 * i:48 * i + 48]
        lux_from, lux_to, n, cct_sid = f8(rec, 0), f8(rec, 1), u8(rec, 4), u8(rec, 5)
        cct = syms[cct_sid][1]
        size = len(cct) // n
        ranges = []
        for j in range(n):
            r = cct[size * j:size * j + size]
            if floats == 4 * GRID:
                # lsc34: four grids of 222 floats each (221 = 17 x 13, and one more): 3568 = 16 + 4 x 222 x 4
                stride = (len(r) - 16) // 4 // 4
                grids = []
                for c in range(4):
                    grids += struct.unpack_from('<%df' % GRID, r, 16 + 4 * stride * c)
                ranges.append((f8(r, 0), f8(r, 1), tuple(grids)))
            else:
                ranges.append((f8(r, 0), f8(r, 1), struct.unpack_from('<%df' % floats, r, 16)))
        out.append((lux_from, lux_to, ranges))
    return out


def references(syms):
    """[(R/G, B/G, CCT)] of the AWB's reference lights, or [] where they are not the ten known"""
    v = next((v for n, v in syms.values() if n == 'refPtV1'), None)
    if v is None:
        return []
    f = [f8(v, i) for i in range(6, len(v) // 8)]
    points = [(f[i], f[i + 1]) for i in range(0, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
    if len(points) != len(REFERENCE_CCTS):
        print('camtuning_v14.py: %d reference lights, not the ten known: none kept' % len(points))
        return []
    return [(rg, bg, cct) for (rg, bg), cct in zip(points, REFERENCE_CCTS)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header') or b'Parameter Parser V1.4.0' not in data[:0x60]:
        sys.exit('camtuning_v14.py: not a V1.4.0 tuning file')
    syms = symbols(data)
    ccm = module(syms, 'cc13', 9)
    lsc = module(syms, 'lsc34', 4 * GRID)
    refs = references(syms)

    for what, tree in (('colour matrix', ccm), ('lens shading', lsc)):
        for lux_from, lux_to, ranges in tree:
            for cct_from, cct_to, v in ranges:
                if what == 'colour matrix':
                    shown = ' '.join('%.3f' % x for x in v)
                else:
                    corners = [v[c * GRID] for c in range(4)]
                    middle = [v[c * GRID + 6 * 17 + 8] for c in range(4)]
                    shown = 'corner R %.2f Gr %.2f Gb %.2f B %.2f, middle R %.2f Gr %.2f Gb %.2f B %.2f' % tuple(
                        corners + middle)
                print('%-13s lux index %4.0f-%-4.0f CCT %5.0f-%-5.0f %s' % (what, lux_from, lux_to, cct_from, cct_to,
                                                                           shown))
    for rg, bg, cct in refs:
        print('reference light R/G %.3f B/G %.3f, tables of %.0f K' % (rg, bg, cct))
    if sys.argv[2] == '-':
        return

    out = bytearray(b'RWCISP02')
    for tree in (ccm, lsc):
        out += struct.pack('<I', len(tree))
        for lux_from, lux_to, ranges in tree:
            out += struct.pack('<2fI', lux_from, lux_to, len(ranges))
            for cct_from, cct_to, v in ranges:
                out += struct.pack('<2f%df' % len(v), cct_from, cct_to, *v)
    out += struct.pack('<I', len(refs))
    for ref in refs:
        out += struct.pack('<3f', *ref)
    open(sys.argv[2], 'wb').write(out)
    print('%s: %d bytes' % (sys.argv[2], len(out)))


main()
