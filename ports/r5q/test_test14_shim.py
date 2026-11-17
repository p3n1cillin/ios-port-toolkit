"""Exercise actual ARM64 Test 14 BootShim instructions."""
import gc
import hashlib
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE

base = pathlib.Path('/inputs')
shim = (base / 'r5q-test14-shim.bin').read_bytes()
fd = (base / 'r5q-test14-uefi.fd').read_bytes()
payload = (base / 'r5q-test14-payload.bin').read_bytes()
FD = 0x9fc00000
PAY = 0xf0000000
SOURCE = 0x80080000
MDSS = 0x0ae00000
INFO = 0x9e300000
SEC_LOG = 0xc1600000
results = []

for name in ['valid', 'valid_display', 'bad_magic', 'oversized', 'too_short', 'fd_in_place']:
    gc.collect()
    content = bytearray(payload)
    if name == 'bad_magic': content[:4] = b'FAIL'
    if name == 'oversized': struct.pack_into('<I', content, 4, 0x04000000)
    if name == 'too_short': struct.pack_into('<I', content, 4, 24)
    address = FD - len(shim) if name == 'fd_in_place' else SOURCE
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
    machine.mem_map(SOURCE, 0x2000000)
    machine.mem_map(FD - 0x1000, 0x2000000)
    machine.mem_map(PAY, 0x4000000)
    machine.mem_map(MDSS, 0x100000)
    machine.mem_map(INFO, 0x100000)
    machine.mem_map(SEC_LOG, 0x200000)
    if name == 'valid_display':
        machine.mem_map(0x9c400000, 0xa00000)
        machine.mem_write(MDSS+0x25014,struct.pack('<I',0x9c400000))
        machine.mem_write(MDSS+0x25024,struct.pack('<I',4320))
        machine.mem_write(MDSS+0x25000,struct.pack('<I',2400<<16|1080))
        machine.mem_write(MDSS+0x20f4,struct.pack('<I',2))

    # Initialize fake sec_log header: "LOGM" magic at offset 4, w_idx = 0x10 at offset 8
    sec_hdr = bytearray(0x100)
    struct.pack_into('<II', sec_hdr, 4, 0x4D474F4C, 0x10)
    machine.mem_write(SEC_LOG, bytes(sec_hdr))

    machine.mem_write(address, shim + fd + bytes(content) + bytes(32))
    machine.mem_write(PAY, b'OLD!OLD!' + bytes([0xa5]) * (len(payload) + 32))
    reached = []
    def stop(uc, pc, size, user):
        reached.append(pc)
        uc.emu_stop()
    machine.hook_add(UC_HOOK_CODE, stop, begin=FD, end=FD)
    machine.emu_start(address, 0, timeout=20000000, count=50000000)
    assert reached == [FD], f'{name}: did not reach SEC entry (pc={hex(reached[0]) if reached else "none"})'
    assert bytes(machine.mem_read(FD, len(fd))) == fd, f'{name}: FD corruption'
    if name in ['valid', 'valid_display', 'fd_in_place']:
        assert bytes(machine.mem_read(PAY, len(payload))) == payload, f'{name}: payload copy mismatch'
        rounded = (len(payload) + 15) & ~15
        assert bytes(machine.mem_read(PAY + rounded, 16)) == bytes([0xa5]) * 16, f'{name}: overrun'
    else:
        assert bytes(machine.mem_read(PAY, 8)) == bytes(8), f'{name}: stale header retained'
        assert bytes(machine.mem_read(PAY + 8, 16)) == bytes([0xa5]) * 16, f'{name}: rejected bytes copied'
    
    # Check sec_log telemetry
    log_data = bytes(machine.mem_read(SEC_LOG + 0x10, 256)).decode('ascii', errors='ignore')
    results.append({'case': name, 'result': 'passed', 'log_snippet': log_data[:80]})

print(json.dumps({'engine': 'Unicorn ARM64 Test 14 BootShim validation',
                  'tests': results}, indent=2))
