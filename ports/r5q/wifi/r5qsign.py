"""r5qsign.py BINARY [ENTITLEMENTS.plist] [--ident ID] [--info Info.plist]: an ad-hoc code signature with entitlements for
a thin arm64 Mach-O, in place (ldid -S's job, for tools and applications built here; ldid is not on this machine).

The signature: a SuperBlob of a CodeDirectory (version 0x20400, SHA-256, 4 KiB pages, ad hoc, not "linker signed" --
a linker-signed binary carries no entitlements), an empty Requirements blob, the entitlements as XML (0xfade7171) and as
DER (0xfade7172, what iOS 15+ reads). Whatever signature the linker left is replaced; the load commands are updated
first (LC_CODE_SIGNATURE, __LINKEDIT's sizes) so the hashed header is the final one.
"""
import hashlib
import plistlib
import struct
import sys

PAGE = 4096
CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02
CSMAGIC_REQUIREMENTS = 0xfade0c01
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xfade7171
CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xfade7172
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1d
MH_EXECUTE = 2


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def der(tag, body):
    return bytes([tag]) + der_len(len(body)) + body


def der_value(v):
    if isinstance(v, bool):
        return der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        n = v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big', signed=True)
        return der(0x02, n)
    if isinstance(v, str):
        return der(0x0c, v.encode())
    if isinstance(v, (list, tuple)):
        return der(0x30, b''.join(der_value(x) for x in v))
    if isinstance(v, dict):
        pairs = [der(0x30, der(0x0c, k.encode()) + der_value(v[k])) for k in sorted(v)]
        return der(0xb0, b''.join(pairs))           # [CONTEXT 16] constructed: a dictionary
    raise TypeError(f'no DER for {type(v)}')


def der_entitlements(ents):
    return der(0x70, der(0x02, b'\x01') + der_value(ents))   # [APPLICATION 16] { version 1, dictionary }


def blob(magic, body):
    return struct.pack('>II', magic, 8 + len(body)) + body


RULES = {'^.*': True, '^.*\\.lproj/': {'optional': True, 'weight': 1000.0},
         '^.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0}, '^Base\\.lproj/': {'weight': 1010.0},
         '^version.plist$': True}
RULES2 = {'.*\\.dSYM($|/)': {'weight': 11.0}, '^(.*/)?\\.DS_Store$': {'omit': True, 'weight': 2000.0}, '^.*': True,
          '^.*\\.lproj/': {'optional': True, 'weight': 1000.0},
          '^.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0}, '^Base\\.lproj/': {'weight': 1010.0},
          '^Info\\.plist$': {'omit': True, 'weight': 20.0}, '^PkgInfo$': {'omit': True, 'weight': 20.0},
          '^embedded\\.provisionprofile$': {'weight': 20.0}, '^version\\.plist$': {'weight': 20.0}}


def seal_bundle(bundle, executable):
    """_CodeSignature/CodeResources for an application bundle: every file but the executable, Info.plist and the
    signature itself, by SHA-1 (files) and SHA-1 + SHA-256 (files2). Returns its path."""
    import os
    files, files2 = {}, {}
    for root, dirs, names in os.walk(bundle):
        dirs[:] = [d for d in dirs if not (root == bundle and d == '_CodeSignature')]
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, bundle).replace(os.sep, '/')
            if rel == executable:
                continue
            body = open(full, 'rb').read()
            sha1, sha256 = hashlib.sha1(body).digest(), hashlib.sha256(body).digest()
            files[rel] = sha1
            if rel not in ('Info.plist', 'PkgInfo'):
                files2[rel] = {'hash': sha1, 'hash2': sha256}
    os.makedirs(os.path.join(bundle, '_CodeSignature'), exist_ok=True)
    out = os.path.join(bundle, '_CodeSignature', 'CodeResources')
    with open(out, 'wb') as f:
        plistlib.dump({'files': files, 'files2': files2, 'rules': RULES, 'rules2': RULES2}, f, fmt=plistlib.FMT_XML)
    return out


def main(argv):
    args = list(argv)
    ident = info = resources = None
    if '--ident' in args:
        i = args.index('--ident'); ident = args[i + 1]; del args[i:i + 2]
    if '--info' in args:
        i = args.index('--info'); info = args[i + 1]; del args[i:i + 2]
    if '--bundle' in args:                      # --bundle APP_DIR: the binary is APP_DIR's executable; seal the bundle
        import os
        i = args.index('--bundle'); bundle = args[i + 1]; del args[i:i + 2]
        plist = plistlib.load(open(os.path.join(bundle, 'Info.plist'), 'rb'))
        executable = plist['CFBundleExecutable']
        ident = ident or plist['CFBundleIdentifier']
        info = os.path.join(bundle, 'Info.plist')
        resources = seal_bundle(bundle, executable)
        args.insert(0, os.path.join(bundle, executable))
    path = args[0]
    ents = plistlib.load(open(args[1], 'rb')) if len(args) > 1 else {}
    data = bytearray(open(path, 'rb').read())
    magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', data, 0)
    if magic != 0xfeedfacf:
        raise SystemExit('not a thin 64-bit Mach-O (a fat binary: thin it first)')
    ident = ident or path.replace('\\', '/').split('/')[-1]

    # find __TEXT, __LINKEDIT and LC_CODE_SIGNATURE
    off, text, linkedit, cs = 32, None, None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b'\0')
            if name == b'__TEXT':
                text = off
            elif name == b'__LINKEDIT':
                linkedit = off
        elif cmd == LC_CODE_SIGNATURE:
            cs = off
        off += size
    if linkedit is None:
        raise SystemExit('no __LINKEDIT')
    if cs is None:
        if 32 + sizeofcmds + 16 > struct.unpack_from('<Q', data, text + 40)[0] + 0x4000 and False:
            pass
        # room after the load commands for one more (16 bytes)? (the linker leaves padding before the first section)
        cs = 32 + sizeofcmds
        if any(data[cs:cs + 16]):
            raise SystemExit('no LC_CODE_SIGNATURE and no room to add one')
        struct.pack_into('<IIII', data, cs, LC_CODE_SIGNATURE, 16, 0, 0)
        struct.pack_into('<II', data, 16, ncmds + 1, sizeofcmds + 16)
        dataoff = len(data)
    else:
        dataoff = struct.unpack_from('<I', data, cs + 8)[0]
    dataoff = (dataoff + 15) & ~15
    del data[dataoff:]
    data.extend(b'\0' * (dataoff - len(data)))

    xml = plistlib.dumps(ents, fmt=plistlib.FMT_XML)
    req_blob = blob(CSMAGIC_REQUIREMENTS, struct.pack('>I', 0))
    ent_blob = blob(CSMAGIC_EMBEDDED_ENTITLEMENTS, xml)
    der_blob = blob(CSMAGIC_EMBEDDED_DER_ENTITLEMENTS, der_entitlements(ents))
    n_code = (dataoff + PAGE - 1) // PAGE
    n_special = 7
    ident_b = ident.encode() + b'\0'
    cd_header = 88
    hash_off = cd_header + len(ident_b) + n_special * 32
    cd_len = hash_off + n_code * 32
    blobs_after_cd = [(2, req_blob), (5, ent_blob), (7, der_blob)]
    count = 1 + len(blobs_after_cd)
    super_len = 12 + 8 * count + cd_len + sum(len(b) for _, b in blobs_after_cd)
    sig_size = (super_len + 15) & ~15

    # the load commands as they will be: the signature's place and size, __LINKEDIT reaching its end
    struct.pack_into('<II', data, cs + 8, dataoff, sig_size)
    le_fileoff = struct.unpack_from('<Q', data, linkedit + 40)[0]
    le_filesize = dataoff + sig_size - le_fileoff
    struct.pack_into('<Q', data, linkedit + 48, le_filesize)
    struct.pack_into('<Q', data, linkedit + 32, (le_filesize + 0x3fff) & ~0x3fff)

    text_fileoff, text_filesize = struct.unpack_from('<QQ', data, text + 40)
    special = {1: hashlib.sha256(open(info, 'rb').read()).digest() if info else b'\0' * 32,
               2: hashlib.sha256(req_blob).digest(),
               3: hashlib.sha256(open(resources, 'rb').read()).digest() if resources else b'\0' * 32, 4: b'\0' * 32,
               5: hashlib.sha256(ent_blob).digest(), 6: b'\0' * 32,
               7: hashlib.sha256(der_blob).digest()}
    cd = struct.pack('>9I', CSMAGIC_CODEDIRECTORY, cd_len, 0x20400, 0x2, hash_off, cd_header, n_special, n_code, dataoff)
    cd += struct.pack('>BBBBI', 32, 2, 0, 12, 0)                  # SHA-256, platform 0, 4 KiB pages, spare2
    cd += struct.pack('>III', 0, 0, 0)                           # scatter, team, spare3
    cd += struct.pack('>QQQQ', 0, text_fileoff, text_filesize, 1 if ftype == MH_EXECUTE else 0)
    assert len(cd) == cd_header
    cd += ident_b
    for slot in range(n_special, 0, -1):
        cd += special[slot]
    for i in range(n_code):
        cd += hashlib.sha256(bytes(data[i * PAGE:min((i + 1) * PAGE, dataoff)])).digest()
    assert len(cd) == cd_len

    index, body, at = b'', b'', 12 + 8 * count
    for typ, b in [(0, cd)] + blobs_after_cd:
        index += struct.pack('>II', typ, at)
        body += b
        at += len(b)
    sig = struct.pack('>III', CSMAGIC_EMBEDDED_SIGNATURE, super_len, count) + index + body
    data += sig + b'\0' * (sig_size - len(sig))
    open(path, 'wb').write(data)
    print(f'r5qsign: {path}: signed ad hoc as {ident}, {len(ents)} entitlements, {n_code} pages')


if __name__ == '__main__':
    main(sys.argv[1:])
