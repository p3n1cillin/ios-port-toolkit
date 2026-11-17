"""Execute the compiled EFI diagnostic against a tiny emulated UEFI ABI."""
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X30, UC_ARM64_REG_SP

binary = pathlib.Path('/inputs/r5q-test1-loader.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
sections = pe + 24 + struct.unpack_from('<H', binary, pe + 20)[0]
results = []

for load_base, case in [(0xe0000000, 'valid'), (0x99e00000, 'relocated'),
                        (0x99e00000, 'missing_gop'), (0x99e00000, 'wrong_display')]:
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
    uc.mem_map(load_base, (image_size + 4095) & ~4095)
    uc.mem_write(load_base, binary[:header_size])
    for i in range(struct.unpack_from('<H', binary, pe + 6)[0]):
        _, virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from('<8sIIII', binary, sections + 40 * i)
        if raw_size:
            uc.mem_write(load_base + virtual_address, binary[raw_offset:raw_offset + raw_size])
    ENV = 0x60000000
    STACK = 0x61000000
    uc.mem_map(ENV, 0x10000)
    uc.mem_map(STACK, 0x10000)
    st, bs, console, gop, mode, info = [ENV + x for x in [0, 0x1000, 0x2000, 0x3000, 0x3100, 0x3200]]
    watchdog, locate, output, sentinel = [ENV + x for x in [0x5000, 0x5010, 0x5020, 0x5030]]
    def q(address, value): uc.mem_write(address, struct.pack('<Q', value))
    q(st + 64, console)
    q(st + 96, bs)
    q(console + 8, output)
    q(bs + 256, watchdog)
    q(bs + 320, locate)
    q(gop + 24, mode)
    q(mode + 8, info)
    q(mode + 24, 0x9c400000)
    q(mode + 32, 1080 * 2400 * 4)
    uc.mem_write(info, struct.pack('<9I', 0, 1080, 2400 if case != 'wrong_display' else 2340, 1, 0, 0, 0, 0, 1080))
    for address in [watchdog, locate, output, sentinel]:
        uc.mem_write(address, b'\xc0\x03\x5f\xd6')  # ret
    transcript = []
    returned = []
    def service(machine, pc, size, data):
        if pc == sentinel:
            returned.append(machine.reg_read(UC_ARM64_REG_X0))
            machine.emu_stop()
        elif pc == locate:
            if case == 'missing_gop':
                machine.reg_write(UC_ARM64_REG_X0, (1 << 63) | 14)
            else:
                q(machine.reg_read(UC_ARM64_REG_X2), gop)
                machine.reg_write(UC_ARM64_REG_X0, 0)
        elif pc == output:
            address = machine.reg_read(UC_ARM64_REG_X1)
            text = bytes(machine.mem_read(address, 6)).decode('utf-16-le').split('\0')[0]
            transcript.append(text)
            machine.reg_write(UC_ARM64_REG_X0, 0)
        elif pc == watchdog:
            machine.reg_write(UC_ARM64_REG_X0, 0)
        else:
            raise AssertionError('Unexpected UEFI service')
    uc.hook_add(UC_HOOK_CODE, service, begin=watchdog, end=sentinel)
    uc.reg_write(UC_ARM64_REG_X0, 0x1234)
    uc.reg_write(UC_ARM64_REG_X1, st)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0xff00)
    uc.reg_write(UC_ARM64_REG_X30, sentinel)
    uc.emu_start(load_base + entry, 0, timeout=5000000, count=1000000)
    text = ''.join(transcript).replace('\r', '')
    assert len(returned) == 1 and 'TEST1 XNU LOADER ENTRY OK' in text
    if case in ['valid', 'relocated']:
        assert returned[0] == 0 and 'TEST1 HANDOFF OK' in text
    else:
        assert returned[0] == (1 << 63) | 3 and 'TEST1 FAIL:' in text
        assert 'TEST1 HANDOFF OK' not in text
    results.append({'case': case, 'load_base': hex(load_base), 'result': 'passed', 'console': text})
print(json.dumps({'engine': 'Unicorn ARM64 with stubbed UEFI console/GOP services',
                  'tests': results, 'phone_display': 'not tested'}, indent=2))
