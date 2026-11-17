"""EMULATION ONLY: run the actual Test 15-profile EFI loader + kernel to a later checkpoint.

Same emulated UEFI services, reserved window and GIC sysreg model as test_test15_roundtrip.py's
main case. Inputs (/inputs): r5q-test15-loader.efi (built with --emu-checkpoint N),
r5q-test15-payload.bin, kernel.macho (for symbolization) and checkpoint.txt (N).
Prints one JSON object; on failure it says where the emulated CPU stopped and why.
"""
import json
import pathlib
import re
import struct
import unicorn.arm64_const as arm
from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_FETCH_UNMAPPED)
from unicorn.arm64_const import (UC_CPU_ARM64_MAX, UC_ARM64_REG_PC, UC_ARM64_REG_X9, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
                                 UC_ARM64_REG_X30, UC_ARM64_REG_SP)

inputs = pathlib.Path('/inputs')
checkpoint = int((inputs / 'checkpoint.txt').read_text())
binary = (inputs / 'r5q-test15-loader.efi').read_bytes()
payload = (inputs / 'r5q-test15-payload.bin').read_bytes()
kernel = (inputs / 'kernel.macho').read_bytes()


def kernel_symbols():
    ncmds = struct.unpack_from('<I', kernel, 16)[0]
    off, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', kernel, off)
        if cmd == 0x2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', kernel, off + 8)
            for i in range(nsyms):
                strx, ntype, nsect, _, value = struct.unpack_from('<IBBHQ', kernel, symoff + 16 * i)
                if ntype & 0xe0 or nsect == 0:
                    continue
                end = kernel.index(b'\0', stroff + strx)
                out.append((value, kernel[stroff + strx:end].decode(errors='replace')))
        off += size
    return sorted(out)


def symbolize(va, symbols):
    best = None
    for value, name in symbols:
        if value > va:
            break
        best = (value, name)
    return f'{best[1]}+{va - best[0]:#x}' if best and va - best[0] < 0x100000 else None


pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
preferred = struct.unpack_from('<Q', binary, optional + 24)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
section_headers = optional + struct.unpack_from('<H', binary, pe + 20)[0]
BASE = 0x9a000000
ENV, STACK = 0x60000000, 0x61000000
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
uc.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
uc.mem_map(BASE, (image_size + 4095) & ~4095)
uc.mem_write(BASE, binary[:header_size])
for i in range(struct.unpack_from('<H', binary, pe + 6)[0]):
    _, vs, va, size, offset = struct.unpack_from('<8sIIII', binary, section_headers + 40 * i)
    if size:
        uc.mem_write(BASE + va, binary[offset:offset + size])
rva, length = struct.unpack_from('<II', binary, optional + 112 + 5 * 8)
cursor = rva
while cursor < rva + length:
    page, block = struct.unpack('<II', bytes(uc.mem_read(BASE + cursor, 8)))
    for offset in range(cursor + 8, cursor + block, 2):
        item = struct.unpack('<H', bytes(uc.mem_read(BASE + offset, 2)))[0]
        kind, location = item >> 12, BASE + page + (item & 0xfff)
        if kind == 10:
            value = struct.unpack('<Q', bytes(uc.mem_read(location, 8)))[0]
            uc.mem_write(location, struct.pack('<Q', (value + BASE - preferred) & ((1 << 64) - 1)))
        else:
            assert kind == 0
    cursor += block
for address, size in [(ENV, 0x10000), (STACK, 0x10000), (0xc1600000, 0x100000),
                      (0xe0000000, 0x10000000), (0xf0000000, 0x4000000)]:
    uc.mem_map(address, size)
uc.mem_write(0xc1600000, struct.pack('<4I', 0, 0x4d474f4c, 0, 0))
uc.mem_write(0xf0000000, payload)
st, bs, console, gop, mode, info = [ENV + x for x in [0, 0x1000, 0x2000, 0x3000, 0x3100, 0x3200]]
watchdog, locate, output, sentinel, getmap, allocate = [ENV + x for x in [0x5000, 0x5010, 0x5020, 0x5030, 0x5040, 0x5050]]


def q(address, value):
    uc.mem_write(address, struct.pack('<Q', value))


q(st + 64, console); q(st + 96, bs); q(console + 8, output); q(bs + 256, watchdog); q(bs + 320, locate)
q(bs + 56, getmap); q(bs + 40, allocate); q(gop + 24, mode); q(mode + 8, info)
q(mode + 24, 0x9c400000); q(mode + 32, 1080 * 2400 * 4)
uc.mem_write(info, struct.pack('<9I', 0, 1080, 2400, 1, 0, 0, 0, 0, 1080))
for address in [watchdog, locate, output, sentinel, getmap, allocate]:
    uc.mem_write(address, b'\xc0\x03\x5f\xd6')
window = struct.pack('<QIIQQQQ', 0x31304e4957513552, 1, 48, 0xe0000000, 0x10000000, 8, 1)
transcript, returned, unmapped = [], [], []


def service(machine, pc, size, user):
    if pc == sentinel:
        returned.append(machine.reg_read(UC_ARM64_REG_X0)); machine.emu_stop(); return
    if pc == locate:
        guid = bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X0), 16))
        if guid == bytes.fromhex('45997b48223edd4d91b7892402d6cf99'):
            machine.mem_write(ENV + 0x4000, window)
            q(machine.reg_read(UC_ARM64_REG_X2), ENV + 0x4000)
        else:
            assert guid == bytes.fromhex('dea94290dc23384a96fb7aded080516a')
            q(machine.reg_read(UC_ARM64_REG_X2), gop)
    elif pc == output:
        transcript.append(bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X1), 6)).decode('utf-16-le').split('\0')[0])
    elif pc == getmap:
        desc = struct.pack('<IIQQQQ', 1, 0, BASE, 0, (image_size + 4095) // 4096, 0xf) + bytes(8)
        desc += struct.pack('<IIQQQQ', 4, 0, STACK, 0, 16, 0xf) + bytes(8)
        q(machine.reg_read(UC_ARM64_REG_X0), len(desc))
        machine.mem_write(machine.reg_read(UC_ARM64_REG_X1), desc)
        q(machine.reg_read(UC_ARM64_REG_X2), 1)
        q(machine.reg_read(UC_ARM64_REG_X3), 48)
        machine.mem_write(machine.reg_read(UC_ARM64_REG_X4), struct.pack('<I', 1))
    elif pc == allocate:
        raise AssertionError('Reserved-window loader must not allocate ordinary RAM')
    else:
        assert pc == watchdog
    machine.reg_write(UC_ARM64_REG_X0, 0)


uc.hook_add(UC_HOOK_CODE, service, begin=watchdog, end=allocate)
gic = {0xc230: 0xff, 0xc667: 1, 0xc663: 0}


def gic_service(machine, pc, size, user):
    word = struct.unpack('<I', bytes(machine.mem_read(pc, 4)))[0]
    key = (word >> 5) & 0xffff
    if word & 0xffe00000 == 0xd5200000:
        machine.reg_write(UC_ARM64_REG_X9, gic[key])
    else:
        gic[key] = machine.reg_read(UC_ARM64_REG_X9)
    machine.reg_write(UC_ARM64_REG_PC, pc + 4)


for offset in range(0x1000, 0x1000 + struct.unpack_from('<I', binary, section_headers + 8)[0], 4):
    word = struct.unpack('<I', bytes(uc.mem_read(BASE + offset, 4)))[0]
    if word & 0xffe00000 in [0xd5200000, 0xd5000000] and (word >> 5) & 0xffff in gic:
        uc.hook_add(UC_HOOK_CODE, gic_service, begin=BASE + offset, end=BASE + offset)


def on_unmapped(machine, access, address, size, value, user):
    unmapped.append({'access': access, 'address': hex(address), 'size': size,
                     'pc': hex(machine.reg_read(UC_ARM64_REG_PC))})
    return False


uc.hook_add(UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, on_unmapped)
interrupts = []


def on_interrupt(machine, intno, user):
    # Unicorn routes every CPU exception here instead of the guest vectors; record and stop.
    interrupts.append({'intno': intno, 'pc': hex(machine.reg_read(UC_ARM64_REG_PC))})
    machine.emu_stop()


from unicorn import UC_HOOK_INTR, __version__ as unicorn_version
uc.hook_add(UC_HOOK_INTR, on_interrupt)
mmfr0 = None
for constant in ['UC_ARM64_REG_ID_AA64MMFR0_EL1']:
    if hasattr(arm, constant):
        mmfr0 = uc.reg_read(getattr(arm, constant))
uc.reg_write(UC_ARM64_REG_X0, 0x1234); uc.reg_write(UC_ARM64_REG_X1, st)
uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff00); uc.reg_write(UC_ARM64_REG_X30, sentinel)
error = None
try:
    uc.emu_start(BASE + entry, 0, timeout=100_000_000, count=400_000_000)
except UcError as exc:
    error = str(exc)
text = ''.join(transcript).replace('\r', '')
expected = f'TEST15 REAL XNU CHECKPOINT {checkpoint} VERIFIED'
result = {'checkpoint': checkpoint, 'result': 'passed' if expected in text and returned == [0] else 'failed',
          'returned': [hex(x) for x in returned], 'console_tail': text[-1500:]}
if result['result'] != 'passed':
    pc = uc.reg_read(UC_ARM64_REG_PC)
    symbols = kernel_symbols()
    loaded = re.search(r'kernel loaded at pa ([0-9a-f]+)', text)
    slide = int(loaded.group(1), 16) - 0xfffffff007004000 if loaded else None
    regs = {'pc': pc, 'lr': uc.reg_read(UC_ARM64_REG_X30), 'sp': uc.reg_read(UC_ARM64_REG_SP)}
    for name in ['ELR_EL1', 'FAR_EL1', 'ESR_EL1', 'VBAR_EL1', 'TTBR0_EL1', 'TTBR1_EL1', 'SCTLR_EL1', 'TCR_EL1', 'MAIR_EL1']:
        constant = getattr(arm, 'UC_ARM64_REG_' + name, None)
        if constant is not None:
            try:
                regs[name.lower()] = uc.reg_read(constant)
            except UcError:
                pass
    where = {}
    for name in ['pc', 'lr', 'elr_el1']:
        value = regs.get(name)
        if value is None:
            continue
        va = value if value >> 48 == 0xffff else (value - slide) & (2**64 - 1) if slide is not None else None
        if va is not None:
            where[name] = symbolize(va, symbols)
    try:
        stopped = bytes(uc.mem_read(pc, 4)).hex()
    except UcError:
        stopped = None
    result.update(error=error, registers={k: hex(v) for k, v in regs.items()}, symbols=where,
                  unmapped=unmapped[:4], stopped_instruction=stopped, cpu_exceptions=interrupts[:4],
                  unicorn_version=unicorn_version,
                  emulated_mmfr0_from_console=(re.search(r'MMFR0=([0-9a-f]+)', text) or [None, None])[1])
print(json.dumps(result))
