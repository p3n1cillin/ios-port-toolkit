"""Android boot images, header versions 0-4 (and vendor_boot v3/v4): read, report, and replace the kernel.

A test image is the stock image with only the kernel replaced (BootShim + FD + payload in the kernel slot):
everything else -- ramdisk, DTB, DTBO, header fields, page size, the boot id -- stays the phone's own, so
the bootloader treats it as it treats the original. This is what package_test<N>.py does for r5q (v2); this
module does it for every header version.

    python bootimg.py info recovery.img                       # header fields, components, AVB footer
    python bootimg.py repack stock.img --kernel new.bin -o test.img [--pad-to-stock]

Formats (AOSP system/tools/mkbootimg, include/bootimg/bootimg.h):
  v0-v2  'ANDROID!', page size in the header; kernel, ramdisk, second; v1 + recovery DTBO; v2 + DTB;
         sha1 "id" over the components and their sizes (the bootloader may check it; it is recomputed)
  v3-v4  'ANDROID!', fixed 4096-byte pages, kernel + ramdisk only (DTB/second in vendor_boot); v4 + boot
         signature size; no id
  vendor_boot v3/v4  'VNDRBOOT': page size, kernel/ramdisk/tags addresses, vendor ramdisk(s), DTB
An AVB footer ('AVBf' in the last 64 bytes), if the stock image has one, is reported; repack writes an
image without it: the packager adds a NONE-algorithm footer the way package_test<N>.py does.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

BOOT_MAGIC = b'ANDROID!'
VENDOR_MAGIC = b'VNDRBOOT'


def _align(n, page):
    return (n + page - 1) // page * page


def parse(data: bytes) -> dict:
    """Header fields and component (offset, size) of a boot or vendor_boot image."""
    if data[:8] == VENDOR_MAGIC:
        return _parse_vendor(data)
    if data[:8] != BOOT_MAGIC:
        raise ValueError('not an Android boot image (no ANDROID!/VNDRBOOT magic)')
    version = struct.unpack_from('<I', data, 40)[0]
    h = {'kind': 'boot', 'header_version': version}
    if version >= 3:
        ks, rs, osv, hsize = struct.unpack_from('<4I', data, 8)
        h.update(page_size=4096, kernel_size=ks, ramdisk_size=rs, os_version=osv, header_size=hsize,
                 cmdline=data[44:44 + 1536].split(b'\0')[0].decode(errors='replace'))
        page = 4096
        if version >= 4:
            h['signature_size'] = struct.unpack_from('<I', data, 44 + 1536)[0]
        off = page
        comps = [('kernel', off, ks)]
        off += _align(ks, page)
        comps.append(('ramdisk', off, rs))
        off += _align(rs, page)
        if version >= 4 and h['signature_size']:
            comps.append(('boot_signature', off, h['signature_size']))
            off += _align(h['signature_size'], page)
    else:
        (ks, kaddr, rs, raddr, ss, saddr, taddr, page, hv, osv) = struct.unpack_from('<10I', data, 8)
        h.update(page_size=page, kernel_size=ks, kernel_addr=hex(kaddr), ramdisk_size=rs, ramdisk_addr=hex(raddr),
                 second_size=ss, second_addr=hex(saddr), tags_addr=hex(taddr), os_version=osv,
                 name=data[48:64].split(b'\0')[0].decode(errors='replace'),
                 cmdline=(data[64:576] + data[608:1632]).split(b'\0')[0].decode(errors='replace'),
                 id=data[576:608].hex())
        off = page
        comps = [('kernel', off, ks)]
        off += _align(ks, page)
        comps.append(('ramdisk', off, rs))
        off += _align(rs, page)
        comps.append(('second', off, ss))
        off += _align(ss, page)
        if version >= 1:
            dtbo_size, dtbo_off, hsize = struct.unpack_from('<IQI', data, 1632)
            h.update(recovery_dtbo_size=dtbo_size, recovery_dtbo_offset=dtbo_off, header_size=hsize)
            comps.append(('recovery_dtbo', off, dtbo_size))
            off += _align(dtbo_size, page)
        if version >= 2:
            dtb_size, dtb_addr = struct.unpack_from('<IQ', data, 1648)
            h.update(dtb_size=dtb_size, dtb_addr=hex(dtb_addr))
            comps.append(('dtb', off, dtb_size))
            off += _align(dtb_size, page)
    h['components'] = [{'name': n, 'offset': o, 'size': s} for n, o, s in comps]
    h['end_of_components'] = off
    h.update(_avb(data))
    return h


def _parse_vendor(data):
    version = struct.unpack_from('<I', data, 8)[0]
    page, kaddr, raddr, vrs = struct.unpack_from('<4I', data, 12)
    cmdline = data[28:28 + 2048].split(b'\0')[0].decode(errors='replace')
    taddr = struct.unpack_from('<I', data, 2076)[0]
    name = data[2080:2096].split(b'\0')[0].decode(errors='replace')
    hsize, dtb_size, dtb_addr = struct.unpack_from('<IIQ', data, 2096)
    h = {'kind': 'vendor_boot', 'header_version': version, 'page_size': page, 'kernel_addr': hex(kaddr),
         'ramdisk_addr': hex(raddr), 'tags_addr': hex(taddr), 'cmdline': cmdline, 'name': name, 'header_size': hsize,
         'vendor_ramdisk_size': vrs, 'dtb_size': dtb_size, 'dtb_addr': hex(dtb_addr)}
    off = _align(hsize, page)
    comps = [('vendor_ramdisk', off, vrs)]
    off += _align(vrs, page)
    comps.append(('dtb', off, dtb_size))
    off += _align(dtb_size, page)
    if version >= 4:
        tsize, tentries, tesize, bcsize = struct.unpack_from('<4I', data, 2112)
        h.update(vendor_ramdisk_table_size=tsize, vendor_ramdisk_table_entries=tentries, bootconfig_size=bcsize)
        comps.append(('vendor_ramdisk_table', off, tsize))
        off += _align(tsize, page)
        comps.append(('bootconfig', off, bcsize))
        off += _align(bcsize, page)
    h['components'] = [{'name': n, 'offset': o, 'size': s} for n, o, s in comps]
    h['end_of_components'] = off
    h.update(_avb(data))
    return h


def _avb(data):
    if len(data) >= 64 and data[-64:-60] == b'AVBf':
        _, maj, mnr, orig, vb_off, vb_size = struct.unpack_from('>4sIIQQQ', data, len(data) - 64)
        return {'avb_footer': {'original_image_size': orig, 'vbmeta_offset': vb_off, 'vbmeta_size': vb_size,
                               'partition_size': len(data)}}
    return {'avb_footer': None}


def component(data, h, name):
    for c in h['components']:
        if c['name'] == name:
            return data[c['offset']:c['offset'] + c['size']]
    return None


def repack(stock: bytes, kernel: bytes) -> bytes:
    """The stock boot image (v0-v4) with its kernel replaced; every other byte of header and component kept.
    v0-v2: the sha1 id is recomputed (AOSP mkbootimg's rule). Returns the image WITHOUT an AVB footer or the
    stock image's trailing padding; add those for the target partition separately."""
    h = parse(stock)
    if h['kind'] != 'boot':
        raise ValueError('repack replaces a boot image kernel; vendor_boot has none')
    page = h['page_size']
    header = bytearray(stock[:page])
    struct.pack_into('<I', header, 8, len(kernel))
    parts = [(c['name'], component(stock, h, c['name'])) for c in h['components']]
    parts = [(n, kernel if n == 'kernel' else d) for n, d in parts]
    v = h['header_version']
    if 1 <= v <= 2:
        # the recovery DTBO's absolute offset follows the new kernel size
        off = page
        for n, d in parts:
            if n == 'recovery_dtbo':
                struct.pack_into('<Q', header, 1636, off)
            off += _align(len(d), page)
    if v <= 2:
        sha = hashlib.sha1()
        for n, d in parts:
            if n in ('kernel', 'ramdisk', 'second', 'recovery_dtbo', 'dtb'):
                sha.update(d)
                sha.update(struct.pack('<I', len(d)))
        header[576:608] = sha.digest() + bytes(12)
    out = bytes(header)
    for n, d in parts:
        out += d + bytes(_align(len(d), page) - len(d))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('info')
    i.add_argument('image', type=pathlib.Path)
    r = sub.add_parser('repack')
    r.add_argument('stock', type=pathlib.Path)
    r.add_argument('--kernel', type=pathlib.Path, required=True)
    r.add_argument('-o', '--out', type=pathlib.Path, required=True)
    a = ap.parse_args()
    if a.cmd == 'info':
        data = a.image.read_bytes()
        h = parse(data)
        print(json.dumps(h, indent=1))
    else:
        out = repack(a.stock.read_bytes(), a.kernel.read_bytes())
        a.out.write_bytes(out)
        print(f'wrote {a.out}: {len(out)} bytes (no AVB footer; pad and sign for the partition separately)')


if __name__ == '__main__':
    sys.exit(main())
