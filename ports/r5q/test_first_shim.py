"""Exercise actual ARM64 copy instructions, including malformed containers."""
import hashlib
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC

base = pathlib.Path('/inputs')
shim = (base / 'r5q-test1-shim.bin').read_bytes()
fd = (base / 'r5q-test1-uefi.fd').read_bytes()
payload = (base / 'r5q-test1-payload.bin').read_bytes()
FD = 0x9fc00000
PAY = 0xf0000000
SOURCE = 0x80080000
results = []

for name in ['valid', 'bad_magic', 'oversized', 'too_short', 'fd_in_place']:
    content = bytearray(payload)
    if name == 'bad_magic': content[:4] = b'FAIL'
    if name == 'oversized': struct.pack_into('<I', content, 4, 0x04000000)
    if name == 'too_short': struct.pack_into('<I', content, 4, 24)
    address = FD - len(shim) if name == 'fd_in_place' else SOURCE
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
    machine.mem_map(SOURCE, 0x800000)
    machine.mem_map(FD - 0x1000, 0x800000)
    machine.mem_map(PAY, 0x4000000)
    machine.mem_write(address, shim + fd + bytes(content) + bytes(32))
    machine.mem_write(PAY, b'OLD!OLD!' + bytes([0xa5]) * (len(payload) + 32))
    reached = []
    def stop(uc, pc, size, user):
        reached.append(pc)
        uc.emu_stop()
    machine.hook_add(UC_HOOK_CODE, stop, begin=FD, end=FD)
    machine.emu_start(address, 0, timeout=20000000, count=2000000)
    assert reached == [FD], f'{name}: did not reach SEC entry'
    assert bytes(machine.mem_read(FD, len(fd))) == fd, f'{name}: FD corruption'
    if name in ['valid', 'fd_in_place']:
        assert bytes(machine.mem_read(PAY, len(payload))) == payload, f'{name}: payload copy mismatch'
        rounded = (len(payload) + 15) & ~15
        assert bytes(machine.mem_read(PAY + rounded, 16)) == bytes([0xa5]) * 16, f'{name}: overrun'
    else:
        assert bytes(machine.mem_read(PAY, 8)) == bytes(8), f'{name}: stale header retained'
        assert bytes(machine.mem_read(PAY + 8, 16)) == bytes([0xa5]) * 16, f'{name}: rejected bytes copied'
    results.append({'case': name, 'result': 'passed'})

print(json.dumps({'engine': 'Unicorn ARM64, actual compiled BootShim instructions',
                  'tests': results, 'phone_boot': 'not tested'}, indent=2))
