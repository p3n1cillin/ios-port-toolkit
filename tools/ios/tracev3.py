"""tracev3.py LOGDIR [REGEX] [--pid N] [--sub SUBSYSTEM]: an iOS unified log (tracev3) read without Apple's tools.
LOGDIR holds diagnostics/ (Persist, Special, HighVolume, Signpost) and uuidtext/ as pulled from the phone
(/private/var/db/diagnostics and /private/var/db/uuidtext, e.g. with work/ios-r5q/wifi/r5q_pull.py).
Prints "time pid level subsystem: message" for every log entry whose message (format string filled in) matches REGEX.

What it knows (2026-11-20, written for the S10 Lite iOS port's Test 373: the baseband manager's info-level lines,
which os_log forwarding to the console never shows): chunks (tag, subtag, size, 8-aligned); the catalog (0x600b:
UUIDs, process entries with their main/dsc UUID and subsystems); chunksets (0x600d, LZ4 'bv41' blocks) holding
firehose chunks (0x6001) of tracepoints; format strings from the per-image uuidtext files ('\\x99\\x88\\x77\\x66') and
the shared cache's ('hcsd' v2). Log entries (record type 4) only; activities, signposts, oversize strings and
private data are skipped or shown as <private>/<?>. Times are the firehose continuous time in seconds, not wall time.
"""
import glob
import os
import re
import struct
import sys

# ---------- LZ4 block ----------


def lz4_block(src, size):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n or len(out) >= size:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    return bytes(out)


def chunkset(data):
    """'bv41' blocks (and 'bv4-' uncompressed) until 'bv4$'"""
    out = b''
    p = 0
    while p + 4 <= len(data):
        sig = data[p:p + 4]
        if sig == b'bv41':
            usize, csize = struct.unpack_from('<II', data, p + 4)
            out += lz4_block(data[p + 12:p + 12 + csize], usize)
            p += 12 + csize
        elif sig == b'bv4-':
            usize, = struct.unpack_from('<I', data, p + 4)
            out += data[p + 8:p + 8 + usize]
            p += 8 + usize
        else:
            break
    return out

# ---------- string files ----------


class UUIDText:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.ok = d[:4] == b'\x99\x88\x77\x66'
        self.ranges = []
        if not self.ok:
            return
        n, = struct.unpack_from('<I', d, 12)
        off = 16 + 8 * n
        for i in range(n):
            start, size = struct.unpack_from('<II', d, 16 + 8 * i)
            self.ranges.append((start, size, off))
            off += size
        self.d = d
        self.path = d[off:].split(b'\0')[0].decode(errors='replace')

    def string(self, offset):
        for start, size, data in self.ranges:
            if start <= offset < start + size:
                o = data + offset - start
                return self.d[o:self.d.index(b'\0', o)].decode(errors='replace')
        return None


class DSCText:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        self.ok = d[:4] == b'hcsd'
        if not self.ok:
            return
        major, minor, nr, nu = struct.unpack_from('<HHII', d, 4)
        self.ranges = []
        p = 16
        for _ in range(nr):
            roff, doff, size, uidx = struct.unpack_from('<QIIQ', d, p)
            self.ranges.append((roff, size, doff, uidx))
            p += 24
        self.ranges.sort()
        self.starts = [r[0] for r in self.ranges]
        self.images = []
        for _ in range(nu):
            toff, tsize = struct.unpack_from('<QI', d, p)
            uuid = d[p + 12:p + 28]
            poff, = struct.unpack_from('<I', d, p + 28)
            self.images.append((toff, tsize, uuid, d[poff:d.index(b'\0', poff)].decode(errors='replace')))
            p += 32

    def string(self, offset):
        import bisect
        i = bisect.bisect_right(self.starts, offset) - 1
        if i < 0:
            return None, None
        roff, size, doff, uidx = self.ranges[i]
        if not roff <= offset < roff + size:
            return None, None
        o = doff + offset - roff
        img = self.images[uidx][3] if uidx < len(self.images) else '?'
        return self.d[o:self.d.index(b'\0', o)].decode(errors='replace'), img

# ---------- printf ----------


FMT = re.compile(r'%(\{[^}]*\})?([-+ #0]*)(\d+|\*)?(\.(\d+|\*))?(hh|h|ll|l|q|z|t|j|L)?([diouxXeEfgGcsSpaA@%mP])')


def render(fmt, args):
    out = []
    pos = 0
    ai = 0
    for m in FMT.finditer(fmt):
        out.append(fmt[pos:m.start()])
        pos = m.end()
        conv = m.group(7)
        if conv == '%':
            out.append('%')
            continue
        if m.group(3) == '*' and ai < len(args):
            ai += 1
        if m.group(5) == '*' and ai < len(args):
            ai += 1
        if ai >= len(args):
            out.append('<?>')
            continue
        kind, v = args[ai][0], args[ai][1]
        ai += 1
        if kind == 'private':
            out.append('<private>')
        elif kind == 'bytes':
            if conv in 's@':
                out.append(v.split(b'\0')[0].decode(errors='replace'))
            else:
                out.append(v.hex())
        elif kind == 'int':
            if conv in 'xX':
                out.append('%x' % v)
            elif conv == 'p':
                out.append('0x%x' % v)
            elif conv in 'di':
                bits = 8 * args[ai - 1][2] if len(args[ai - 1]) > 2 else 64
                out.append(str(v - (1 << bits) if v >> (bits - 1) & 1 else v))
            elif conv == 'c':
                out.append(chr(v & 0xff))
            elif conv in 'eEfgG':
                out.append(repr(struct.unpack('<d', struct.pack('<Q', v))[0]))
            else:
                out.append(str(v))
        else:
            out.append(str(v))
    out.append(fmt[pos:])
    return ''.join(out)

# ---------- the log ----------


LEVEL = {0: 'Default', 1: 'Info', 2: 'Debug', 0x10: 'Error', 0x11: 'Fault'}


class Reader:
    def __init__(self, logdir):
        self.logdir = logdir
        self.uuidtexts = {}
        self.oversize = {}
        dscs = glob.glob(os.path.join(logdir, 'uuidtext', 'dsc', '*'))
        self.dsc = {}
        for f in dscs:
            self.dsc[os.path.basename(f).upper()] = DSCText(f)

    def uuidtext(self, uuid):
        h = uuid.hex().upper()
        if h not in self.uuidtexts:
            p = os.path.join(self.logdir, 'uuidtext', h[:2], h[2:])
            self.uuidtexts[h] = UUIDText(p) if os.path.exists(p) else None
        return self.uuidtexts[h]

    def catalog(self, d):
        sub_off, proc_off, nproc, chunk_off, nchunk = struct.unpack_from('<HHHHH', d, 0)
        base = 24
        uuids = [d[base + 16 * i:base + 16 * i + 16] for i in range(sub_off // 16)]
        strings = d[base + sub_off:base + proc_off]
        procs = {}
        p = base + proc_off
        for _ in range(nproc):
            idx, unk, main_i, dsc_i = struct.unpack_from('<HHHH', d, p)
            pid1, pid2, pid, euid = struct.unpack_from('<QIII', d, p + 8)
            nuuid, = struct.unpack_from('<I', d, p + 32)     # (+28 and +36: unknown)
            p += 40
            p += 16 * nuuid
            nsub, = struct.unpack_from('<I', d, p)
            p += 8
            subs = {}
            for _ in range(nsub):
                ident, soff, coff = struct.unpack_from('<HHH', d, p)
                s = strings[soff:strings.index(b'\0', soff)].decode(errors='replace')
                c = strings[coff:strings.index(b'\0', coff)].decode(errors='replace')
                subs[ident] = f'{s}:{c}'
                p += 6
            p = (p + 7) & ~7
            procs[(pid1, pid2)] = dict(pid=pid, main=uuids[main_i] if main_i < len(uuids) else None,
                                      dsc=uuids[dsc_i] if dsc_i < len(uuids) else None, subs=subs)
        return procs

    def entries(self):
        files = []
        for sub in ('Persist', 'Special', 'HighVolume', 'Signpost'):
            files += sorted(glob.glob(os.path.join(self.logdir, 'diagnostics', sub, '*.tracev3')))
        for f in files:
            d = open(f, 'rb').read()
            p = 0
            procs = {}
            while p + 16 <= len(d):
                tag, sub, size = struct.unpack_from('<IIQ', d, p)
                body = d[p + 16:p + 16 + size]
                if tag == 0x600b:
                    try:
                        procs = self.catalog(body)
                    except (struct.error, ValueError, IndexError):
                        procs = {}
                elif tag == 0x600d:
                    try:
                        inner = chunkset(body)
                    except IndexError:
                        inner = b''
                    yield from self.chunks(inner, procs)
                p = (p + 16 + size + 7) & ~7

    def chunks(self, d, procs):
        p = 0
        hoses = []
        while p + 16 <= len(d):
            tag, sub, size = struct.unpack_from('<IIQ', d, p)
            body = d[p + 16:p + 16 + size]
            if tag == 0x6001:
                hoses.append(body)
            elif tag == 0x6002:
                # oversize: an entry's items too big for its firehose chunk, found by (process, data ref)
                pid1, pid2 = struct.unpack_from('<QI', body, 0)
                ref, pub, priv = struct.unpack_from('<IHH', body, 24)
                self.oversize[(pid1, pid2, ref)] = body[32:32 + pub]
            p = (p + 16 + size + 7) & ~7
        for body in hoses:
            yield from self.firehose(body, procs)

    def firehose(self, d, procs):
        pid1, pid2, ttl, collapsed, unk, pub_size, priv_off, unk2, unk3, base_time = struct.unpack_from('<QIBBHHHHHQ', d, 0)
        proc = procs.get((pid1, pid2), {})
        p = 32
        end = min(16 + pub_size, len(d))
        while p + 24 <= end:
            rtype, ltype, flags, loc, tid, dlo, dhi, dsize = struct.unpack_from('<BBHIQIHH', d, p)
            data = d[p + 24:p + 24 + dsize]
            t = base_time + (dlo | dhi << 32)
            if rtype == 0x4:
                yield self.log(proc, (pid1, pid2), ltype, flags, loc, tid, t, data)
            if rtype == 0:
                break
            p = (p + 24 + dsize + 7) & ~7

    # A log entry's data (found from the bytes, 20F66): [activity id 8, if flags 0x1] [private data offset+size 4,
    # 0x100] [caller pc 4] [large offset u16, 0x20 | uuid 16, formatter 0xa | u16, formatter 0x8 or 0xc]
    # [subsystem u16, 0x200] [ttl u8, 0x400] [oversize data ref u16, 0x800] then the items: u8 flags, u8 count,
    # each (u8 type, u8 size, value), then the strings they point into ((u16 offset, u16 size) values).
    def log(self, proc, key, ltype, flags, loc, tid, t, data):
        q = 0
        img = ''
        sub = None
        try:
            if flags & 0x1:
                q += 8
            if flags & 0x100:
                q += 4
            q += 4
            ftype = flags & 0xe
            large = 0
            uuid = None
            if flags & 0x20:
                large, = struct.unpack_from('<H', data, q)
                q += 2
            if ftype == 0xa:
                uuid = data[q:q + 16]
                q += 16
            elif ftype in (0x8, 0xc):
                extra, = struct.unpack_from('<H', data, q)
                q += 2
                if ftype == 0xc:
                    large = extra or large
            if flags & 0x200:
                sub, = struct.unpack_from('<H', data, q)
                q += 2
            if flags & 0x400:
                q += 1
            items_data = data[q:]
            if flags & 0x800:
                ref, = struct.unpack_from('<H', data, q)
                q += 2
                items_data = self.oversize.get(key + (ref,), data[q:])
            fmt = None
            offset = loc | (large << 31)
            if ftype in (0x4, 0xc) and proc.get('dsc') is not None:
                dsc = self.dsc.get(proc['dsc'].hex().upper())
                if dsc and dsc.ok:
                    fmt, img = dsc.string(offset)
            elif ftype == 0x2 and proc.get('main') is not None:
                u = self.uuidtext(proc['main'])
                if u and u.ok:
                    fmt = u.string(offset)
                    img = u.path
            elif ftype == 0xa and uuid is not None:
                u = self.uuidtext(uuid)
                if u and u.ok:
                    fmt = u.string(offset)
                    img = u.path
            args = []
            if len(items_data) >= 2:
                nitems = items_data[1]
                r = 2
                items = []
                for _ in range(nitems):
                    itype, isize = items_data[r], items_data[r + 1]
                    items.append((itype, isize, items_data[r + 2:r + 2 + isize]))
                    r += 2 + isize
                backing = items_data[r:]
                for itype, isize, val in items:
                    kind = itype & 0xf0
                    if kind in (0x20, 0x30, 0x40, 0x50, 0xf0) and isize == 4:
                        o, n = struct.unpack('<HH', val)
                        args.append(('private', None) if (itype & 0x1 and n == 0) else ('bytes', backing[o:o + n]))
                    elif kind in (0x00, 0x10) and isize in (1, 2, 4, 8):
                        args.append(('private', None) if itype & 0x1 else ('int', int.from_bytes(val, 'little'), isize))
                    else:
                        args.append(('bytes', val))
            msg = render(fmt, args) if fmt is not None else '<format %s 0x%x not found>' % (hex(flags), offset)
        except (struct.error, IndexError, ValueError) as e:
            msg = '<unreadable: %s>' % e
        subsystem = proc.get('subs', {}).get(sub, '') if sub is not None else ''
        return dict(time=t / 1e9, pid=proc.get('pid', -1), level=LEVEL.get(ltype, ltype), sub=subsystem,
                    image=os.path.basename(img or ''), msg=msg)


def main():
    args = sys.argv[1:]
    pid = sub = None
    if '--pid' in args:
        i = args.index('--pid')
        pid = int(args[i + 1])
        del args[i:i + 2]
    if '--sub' in args:
        i = args.index('--sub')
        sub = args[i + 1]
        del args[i:i + 2]
    logdir = args[0]
    pat = re.compile(args[1]) if len(args) > 1 else None
    for e in Reader(logdir).entries():
        if pid is not None and e['pid'] != pid:
            continue
        if sub is not None and not e['sub'].startswith(sub):
            continue
        line = '%12.3f %5d %-7s %s [%s] %s' % (e['time'], e['pid'], e['level'], e['sub'], e['image'], e['msg'])
        if pat is None or pat.search(line):
            print(line)


if __name__ == '__main__':
    main()
