"""dsc_sel.py CACHE IMAGE OFFSET COUNT: disassemble an image's code in a split dyld shared cache, and annotate each
adrp+ldr/add pair with the string it reaches (a selector name through a selref, or a C string)."""
import glob, re, struct, subprocess, sys

path, name, off, count = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4])
subs = []
for sub in sorted(glob.glob(path + '*')):
    if sub.endswith('.map') or sub.endswith('.symbols'):
        continue
    g = open(sub, 'rb')
    hh = g.read(0x20)
    if not hh.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', hh, 0x10)
    g.seek(mo)
    for i in range(mc):
        a, sz, fo, mx, ini = struct.unpack('<QQQII', g.read(32))
        subs.append((a, sz, fo, sub))


def mem(addr, n):
    for a, sz, fo, sub in subs:
        if a <= addr < a + sz:
            with open(sub, 'rb') as g:
                g.seek(fo + addr - a)
                return g.read(n)
    return None


def cstr(addr):
    b = mem(addr, 120)
    if not b:
        return None
    s = b.split(b'\0')[0]
    return s.decode('latin1') if s and all(32 <= c < 127 for c in s) else None


f = open(path, 'rb')
h = f.read(0x200)
img_off, img_cnt = struct.unpack_from('<II', h, 0x18)
if img_cnt == 0:
    img_off, img_cnt = struct.unpack_from('<II', h, 0x1c0)
f.seek(img_off)
base = None
for _ in range(img_cnt):
    a, mt, ino, po, pad = struct.unpack('<QQQII', f.read(32))
    f.seek(po) if False else None
    pos = f.tell()
    f.seek(po)
    p = f.read(200).split(b'\0')[0].decode()
    f.seek(pos)
    if p.endswith('/' + name):
        base = a
        break
addr = base + off
data = mem(addr, count * 4)
open('/tmp/dsc_raw.bin', 'wb').write(data)
subprocess.run(['llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64', '--rename-section', '.data=.text,alloc,load,code',
                '/tmp/dsc_raw.bin', '/tmp/dsc_raw.o'], check=True)
out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--adjust-vma=%#x' % addr, '/tmp/dsc_raw.o'],
                     capture_output=True, text=True).stdout
pages = {}
for line in out.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s+(.*)', line)
    if not m:
        continue
    pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    note = ''
    if op == 'adrp':
        r = re.match(r'(x\d+), (0x[0-9a-f]+)', args)
        if r:
            pages[r.group(1)] = int(r.group(2), 16)
    elif op in ('ldr', 'add'):
        r = re.match(r'(x\d+), \[?(x\d+), #(0x[0-9a-f]+)\]?', args)
        if r and r.group(2) in pages:
            target = pages[r.group(2)] + int(r.group(3), 16)
            if op == 'ldr':
                ptr = mem(target, 8)
                if ptr:
                    p = struct.unpack('<Q', ptr)[0] & 0xfffffffff
                    s = cstr(p)
                    note = '  ; [%#x] -> %#x %s' % (target, p, ('"%s"' % s) if s else '')
            else:
                s = cstr(target)
                note = '  ; %#x %s' % (target, ('"%s"' % s) if s else '')
    elif op == 'bl':
        r = re.match(r'(0x[0-9a-f]+)', args)
        if r:
            note = '  ; call %s+%#x' % (name, int(r.group(1), 16) - base) if base <= int(r.group(1), 16) < base + 0x4000000 else ''
    print('%#x (+%#x): %s %s%s' % (pc, pc - base, op, args.split(' <')[0], note))
