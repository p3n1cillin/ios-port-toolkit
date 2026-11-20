#!/usr/bin/env python3
"""macho_static.py IN OUT (2026-11-20): an lld-linked iOS executable made into a static one, as Apple's ld64 -static makes
it: no dyld (LC_LOAD_DYLINKER, LC_MAIN and the dyld info commands dropped), the entry point as an LC_UNIXTHREAD, and
MH_PIE / MH_DYLDLINK cleared so the kernel maps it where it was linked.

Why: init and var_store (libmini, -nostdlib) must run with no dyld -- init is process 1, started before anything
else, and the root's prebuilt ones were linked with ld64 -static on a Mac. Here the linker is LLVM's ld64.lld (21),
which takes -static but still writes a dynamic executable: Test 411's init died with "initproc failed to start --
Library not loaded: /usr/lib/libSystem.B.dylib". Link with -Wl,-no_fixup_chains -Wl,-headerpad,0x400: classic
rebase opcodes leave every pointer in the file holding its unslid address (lld keeps arm64 executables PIE even with
-no_pie, so MH_PIE is cleared here and the kernel maps the file where it was linked), and there is room for the thread
command.
"""
import struct
import sys

LC_SEGMENT_64, LC_UNIXTHREAD, LC_LOAD_DYLINKER = 0x19, 0x5, 0xe
LC_MAIN, LC_DYLD_INFO_ONLY = 0x80000028, 0x80000022
LC_DYLD_CHAINED_FIXUPS, LC_DYLD_EXPORTS_TRIE = 0x80000034, 0x80000033
DROP = {LC_LOAD_DYLINKER, LC_MAIN, LC_DYLD_INFO_ONLY, LC_DYLD_CHAINED_FIXUPS, LC_DYLD_EXPORTS_TRIE, 0x22}
MH_PIE, MH_DYLDLINK = 0x200000, 0x4


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    b = bytearray(open(sys.argv[1], 'rb').read())
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', b, 0)
    assert magic == 0xfeedfacf and cpu == 0x0100000c and ftype == 2, 'not an arm64 executable'
    off, kept, entryoff, text_vm, first_data, chained = 32, [], None, None, len(b), False
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', b, off)
        raw = bytes(b[off:off + size])
        if cmd == LC_DYLD_CHAINED_FIXUPS:
            chained = True
        if cmd == LC_MAIN:
            entryoff = struct.unpack_from('<Q', raw, 8)[0]
        if cmd == LC_SEGMENT_64:
            name = raw[8:24].rstrip(b'\0')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', raw, 24)
            if name == b'__TEXT':
                text_vm = vmaddr
            nsects = struct.unpack_from('<I', raw, 64)[0]
            for i in range(nsects):
                s_off = struct.unpack_from('<I', raw, 72 + 80 * i + 48)[0]
                if s_off:
                    first_data = min(first_data, s_off)
        if cmd not in DROP:
            kept.append(raw)
        off += size
    if entryoff is None or text_vm is None:
        print('macho_static: no LC_MAIN or __TEXT')
        return 1
    if chained:
        print('macho_static: chained fixups (link with -Wl,-no_fixup_chains): its pointers would not be addresses')
        return 1
    # ARM_THREAD_STATE64 (flavor 6, 68 words): x0-x28, fp, lr, sp, pc, then cpsr and a pad word
    state = [0] * 32 + [text_vm + entryoff, 0]
    thread = struct.pack('<4I', LC_UNIXTHREAD, 16 + 272, 6, 68) + struct.pack('<34Q', *state)
    kept.append(thread)
    cmds = b''.join(kept)
    if 32 + len(cmds) > first_data:
        print(f'macho_static: no room for the load commands ({32 + len(cmds)} > {first_data}): link with -headerpad')
        return 1
    b[32:32 + sizeofcmds] = bytes(sizeofcmds)
    b[32:32 + len(cmds)] = cmds
    struct.pack_into('<3I', b, 16, len(kept), len(cmds), flags & ~(MH_PIE | MH_DYLDLINK))
    open(sys.argv[2], 'wb').write(b)
    print(f'macho_static: {sys.argv[2]}: {len(kept)} load commands, entry {text_vm + entryoff:#x}, no dyld')
    return 0


if __name__ == '__main__':
    sys.exit(main())
