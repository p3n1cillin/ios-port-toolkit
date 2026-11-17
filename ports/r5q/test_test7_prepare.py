"""Run the actual preparation-only EFI loader with emulated UEFI services."""
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP

inputs = pathlib.Path('/inputs')
binary = (inputs / 'r5q-test7-loader.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
preferred = struct.unpack_from('<Q', binary, optional + 24)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
section_headers = optional + struct.unpack_from('<H', binary, pe + 20)[0]
BASE = 0x9a000000
ENV, STACK = 0x60000000, 0x61000000
uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
uc.ctl_set_tcg_buffer_size(32 * 1024 * 1024)
uc.mem_map(BASE, (image_size + 4095) & ~4095)
uc.mem_write(BASE, binary[:header_size])
for i in range(struct.unpack_from('<H', binary, pe + 6)[0]):
    _, vs, va, size, offset = struct.unpack_from('<8sIIII', binary, section_headers + 40*i)
    if size:
        uc.mem_write(BASE+va, binary[offset:offset+size])
rva, length = struct.unpack_from('<II', binary, optional + 112 + 5*8)
cursor = rva
while cursor < rva+length:
    page, block = struct.unpack('<II', bytes(uc.mem_read(BASE+cursor,8)))
    assert block >= 8 and cursor+block <= rva+length
    for offset in range(cursor+8,cursor+block,2):
        item = struct.unpack('<H',bytes(uc.mem_read(BASE+offset,2)))[0]
        kind, location = item >> 12, BASE+page+(item & 0xfff)
        if kind == 10:
            value=struct.unpack('<Q',bytes(uc.mem_read(location,8)))[0]
            uc.mem_write(location,struct.pack('<Q',(value+BASE-preferred) & ((1<<64)-1)))
        else:
            assert kind == 0
    cursor += block
for address, size in [(ENV,0x10000),(STACK,0x20000),(0xc1600000,0x200000),
                       (0xd0000000,0x20000000),(0xf0000000,0x4000000)]:
    uc.mem_map(address,size)
uc.mem_write(0xc1600000,struct.pack('<4I',0,0x4d474f4c,0,0))
uc.mem_write(0xf0000000,(inputs/'r5q-test7-payload.bin').read_bytes())
st,bs,console,gop,mode,info=[ENV+x for x in [0,0x1000,0x2000,0x3000,0x3100,0x3200]]
watchdog,locate,output,sentinel,getmap,allocate=[ENV+x for x in [0x5000,0x5010,0x5020,0x5030,0x5040,0x5050]]
def q(address,value):uc.mem_write(address,struct.pack('<Q',value))
q(st+64,console);q(st+96,bs);q(console+8,output);q(bs+256,watchdog);q(bs+320,locate)
q(bs+56,getmap);q(bs+40,allocate);q(gop+24,mode);q(mode+8,info)
q(mode+24,0x9c400000);q(mode+32,1080*2400*4)
uc.mem_write(info,struct.pack('<9I',0,1080,2400,1,0,0,0,0,1080))
for address in [watchdog,locate,output,sentinel,getmap,allocate]:uc.mem_write(address,b'\xc0\x03\x5f\xd6')
transcript=[];returned=[];claims=[]
def service(machine,pc,size,user):
    if pc==sentinel:
        returned.append(machine.reg_read(UC_ARM64_REG_X0));machine.emu_stop();return
    if pc==locate:q(machine.reg_read(UC_ARM64_REG_X2),gop)
    elif pc==output:
        transcript.append(bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X1),6)).decode('utf-16-le').split('\0')[0])
    elif pc==getmap:
        desc=struct.pack('<IIQQQQ',7,0,0xd0000000,0,0x20000,0xf)
        q(machine.reg_read(UC_ARM64_REG_X0),len(desc))
        machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),desc)
        q(machine.reg_read(UC_ARM64_REG_X2),1);q(machine.reg_read(UC_ARM64_REG_X3),48)
        machine.mem_write(machine.reg_read(UC_ARM64_REG_X4),struct.pack('<I',1))
    elif pc==allocate:
        claim=struct.unpack('<Q',bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X3),8)))[0]
        assert (machine.reg_read(UC_ARM64_REG_X0),machine.reg_read(UC_ARM64_REG_X1),
                machine.reg_read(UC_ARM64_REG_X2),claim)==(2,2,0x20000,0xd0000000)
        claims.append(hex(claim))
    else:assert pc==watchdog
    machine.reg_write(UC_ARM64_REG_X0,0)
uc.hook_add(UC_HOOK_CODE,service,begin=watchdog,end=allocate)
uc.reg_write(UC_ARM64_REG_X0,0x1234);uc.reg_write(UC_ARM64_REG_X1,st)
uc.reg_write(UC_ARM64_REG_SP,STACK+0x1ff00);uc.reg_write(UC_ARM64_REG_X30,sentinel)
uc.emu_start(BASE+entry,0,timeout=45000000,count=100000000)
text=''.join(transcript).replace('\r','')
assert returned==[0] and claims==['0xd0000000']
assert 'TEST7 KERNEL PREPARATION VERIFIED' in text and 'TEST7 PREPARATION RESULT 2' in text
count=struct.unpack('<I',bytes(uc.mem_read(0xc1600008,4)))[0]
assert bytes(uc.mem_read(0xc1600010,count)).decode()==text
print(json.dumps({'result':'passed','engine':'actual relocated EFI loader in Unicorn ARM64',
 'claimed_window':claims,'console':text,'scope':'Mach-O load, generated DeviceTree/boot_args and return; no CPU/MMU switch or phone execution'},indent=2))
