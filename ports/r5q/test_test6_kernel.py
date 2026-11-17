"""Execute the pinned XNU's real assembly entry to checkpoint 1 in ARM64."""
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_SP

data = pathlib.Path('/inputs/test6-xnu.macho').read_bytes()
report = json.loads(pathlib.Path('/inputs/test6-kernel-input.json').read_text())
virt = 0xfffffff004000000
base = 0x100000000
pa = lambda va: (va - virt + base) & ((1 << 64) - 1)
segments = report['segments']
low = min(pa(int(s['va'], 16)) for s in segments) & ~4095
high = (max(pa(int(s['va'], 16)) + s['size'] for s in segments) + 4095) & ~4095
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
uc.mem_map(low, high-low)
slot = None
for segment in segments:
    start = segment['file_offset']
    size = segment['file_size']
    physical = pa(int(segment['va'],16))
    if size:
        uc.mem_write(physical, data[start:start+size])
        marker = data[start:start+size].find(b'XNULDRST')
        if marker >= 0:
            slot = physical + marker
assert slot is not None
ENV = 0x60000000
STUB = ENV + 0x5000
uc.mem_map(ENV, 0x10000)
uc.mem_write(ENV, struct.pack('<HHI7Q',2,2,0,virt,base,0x40000000,base+0x08000000,0,0,0))
uc.mem_write(slot + 8, struct.pack('<QQQ', STUB, 1, (base-virt) & ((1<<64)-1)))
uc.mem_write(STUB, b'\xc0\x03\x5f\xd6')
observed=[]
def returned(machine, pc, size, user):
    observed.append({'reason':machine.reg_read(UC_ARM64_REG_X2),
                     'checkpoint':machine.reg_read(UC_ARM64_REG_X3)})
    machine.emu_stop()
uc.hook_add(UC_HOOK_CODE,returned,begin=STUB,end=STUB)
uc.reg_write(UC_ARM64_REG_X0, ENV)
uc.reg_write(UC_ARM64_REG_SP, ENV+0xff00)
uc.emu_start(pa(int(report['entry_va'],16)),0,timeout=5000000,count=1000000)
assert observed==[{'reason':2,'checkpoint':1}], observed
print(json.dumps({'result':'passed','engine':'Unicorn ARM64, actual pinned XNU entry instructions',
 'kernel_sha256':report['kernel_sha256'],'observed':observed,
 'scope':'kernel entry and checkpoint branch; real loader context restore and phone hardware remain untested'},indent=2))
