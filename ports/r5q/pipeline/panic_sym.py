#!/usr/bin/env python3
"""panic_sym.py KERNEL.macho LOG [N] (2026-11-20): the last N (default 1) panics in LOG (a `phone.py logs` text file,
e.g. firmware-now.txt), with pc, lr and every backtrace address named from KERNEL's symbol table (the kernel the
panicking boot ran: port/build/xnu-r5q-tNNN.macho). This kernel runs unslid, so addresses map 1:1; if a report says
otherwise (a "Kernel slide:" line), the slide is taken off.

Why: until Test 409 each panic's backtrace was named by hand with nm. Usage:
    python pipeline/panic_sym.py port/build/xnu-r5q-t409.macho port/build/phone-logs/maint-logs-*/firmware-now.txt
"""
import bisect
import re
import struct
import sys


def symbols(path):
    k = open(path, 'rb').read()
    ncmds = struct.unpack_from('<I', k, 16)[0]
    off, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', k, off)
        if cmd == 0x2:  # LC_SYMTAB
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', k, off + 8)
            for i in range(nsyms):
                strx, ntype, nsect, _, value = struct.unpack_from('<IBBHQ', k, symoff + 16 * i)
                if ntype & 0xe0 or nsect == 0:
                    continue
                end = k.index(b'\0', stroff + strx)
                out.append((value, k[stroff + strx:end].decode(errors='replace')))
        off += size
    out.sort()
    return [v for v, _ in out], [n for _, n in out]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    values, names = symbols(sys.argv[1])
    text = open(sys.argv[2], 'rb').read().decode('latin-1')
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    starts = [m.start() for m in re.finditer(r'panic\(cpu \d+ caller', text)]
    if not starts:
        print('no panic in', sys.argv[2])
        return 1

    def name(va):
        i = bisect.bisect_right(values, va) - 1
        if i < 0 or va - values[i] > 0x100000:
            return '?'
        return f'{names[i]}+{va - values[i]:#x}'

    for start in starts[-count:]:
        block = text[start:start + 12000]
        cut = block.find('\n\n\n')
        block = block[:cut] if cut > 0 else block
        slide = re.search(r'Kernel slide:\s+0x([0-9a-f]+)', block)
        slide = int(slide.group(1), 16) if slide else 0
        print(block.splitlines()[0])
        for label, pat in [('pc', r'pc:\s+0x([0-9a-f]{16})'), ('lr', r'\blr:\s+0x([0-9a-f]{16})')]:
            m = re.search(pat, block)
            if m:
                print(f'  {label} {m.group(1)} {name(int(m.group(1), 16) - slide)}')
        m = re.search(r'caller 0x([0-9a-f]+)', block)
        if m:
            print(f'  caller {m.group(1)} {name(int(m.group(1), 16) - slide)}')
        bt = block.find('backtrace:')
        if bt > 0:
            print('  backtrace:')
            for m in re.finditer(r'lr: 0x([0-9a-f]{16})', block[bt:]):
                print(f'    {m.group(1)} {name(int(m.group(1), 16) - slide)}')
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
