"""Intercept the helper's actual SVC instruction; no host reboot is possible."""
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X8

binary = pathlib.Path('/inputs/reboot-recovery').read_bytes()
assert binary[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', binary, 18)[0] == 183
entry, phoff = struct.unpack_from('<QQ', binary, 24)
phsize, phcount = struct.unpack_from('<HH', binary, 54)
segments = []
for index in range(phcount):
    kind, flags, offset, address, physical, size, memsize, alignment = struct.unpack_from('<IIQQQQQQ', binary, phoff + phsize * index)
    if kind == 1:
        segments.append((offset, address, size, memsize))
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
start = min(s[1] for s in segments) & ~4095
end = (max(s[1] + s[3] for s in segments) + 4095) & ~4095
uc.mem_map(start, end - start)
for offset, address, size, memsize in segments:
    uc.mem_write(address, binary[offset:offset + size])
observed = []
def syscall(machine, number, data):
    values = [machine.reg_read(r) for r in [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X8]]
    reason = bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X3), 9))
    assert values == [0xfee1dead, 0x28121969, 0xa1b2c3d4, 142]
    assert reason == b'recovery\0'
    observed.append('reboot(RESTART2, recovery), intercepted before any real syscall')
    machine.emu_stop()
uc.hook_add(UC_HOOK_INTR, syscall)
uc.emu_start(entry, 0, timeout=1000000, count=100)
assert len(observed) == 1
print(json.dumps({'engine': 'Unicorn ARM64, SVC interception', 'test': 'recovery entry syscall arguments',
                  'result': 'passed', 'observed': observed[0], 'phone_reboot': 'not requested'}, indent=2))
