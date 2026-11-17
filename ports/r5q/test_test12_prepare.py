"""Run the actual preparation-only EFI loader with emulated UEFI services."""
import gc
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP

inputs = pathlib.Path('/inputs')
binary = (inputs / 'r5q-test12-loader.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
preferred = struct.unpack_from('<Q', binary, optional + 24)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
section_headers = optional + struct.unpack_from('<H', binary, pe + 20)[0]
BASE = 0x9a000000
ENV, STACK = 0x60000000, 0x61000000
def run_case(name, payload, windowdata, expected_text, expected_status=0x8000000000000001):
    gc.collect()
    expected_claims = []
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
    for address, size in [(ENV,0x10000),(STACK,0x10000),(0xc1600000,0x100000),
                           (0xe0000000,0x10000000),(0xf0000000,0x4000000)]:
        uc.mem_map(address,size)
    uc.mem_write(0xc1600000,struct.pack('<4I',0,0x4d474f4c,0,0))
    uc.mem_write(0xf0000000,payload)
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
        if pc==locate:
            guid=bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X0),16))
            if guid==bytes.fromhex('45997b48223edd4d91b7892402d6cf99'):
                if windowdata is None:
                    machine.reg_write(UC_ARM64_REG_X0,0x800000000000000e);return
                machine.mem_write(ENV+0x4000,windowdata)
                q(machine.reg_read(UC_ARM64_REG_X2),ENV+0x4000)
            else:
                assert guid==bytes.fromhex('dea94290dc23384a96fb7aded080516a')
                q(machine.reg_read(UC_ARM64_REG_X2),gop)
        elif pc==output:
            transcript.append(bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X1),6)).decode('utf-16-le').split('\0')[0])
        elif pc in [getmap,allocate]:
            raise AssertionError('Reserved-window preparation must not allocate or inspect ordinary RAM')
        else:assert pc==watchdog
        machine.reg_write(UC_ARM64_REG_X0,0)
    hook = uc.hook_add(UC_HOOK_CODE,service,begin=watchdog,end=allocate)
    uc.reg_write(UC_ARM64_REG_X0,0x1234);uc.reg_write(UC_ARM64_REG_X1,st)
    uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00);uc.reg_write(UC_ARM64_REG_X30,sentinel)
    uc.emu_start(BASE+entry,0,timeout=45000000,count=100000000)
    uc.hook_del(hook)
    text=''.join(transcript).replace('\r','')
    assert returned == [expected_status], (name, returned, text)
    assert claims == expected_claims, (name, claims, text)
    assert expected_text in text, (name, text)
    count=struct.unpack('<I',bytes(uc.mem_read(0xc1600008,4)))[0]
    assert bytes(uc.mem_read(0xc1600010,count)).decode()==text
    return {'case': name, 'result': 'passed', 'claims': claims, 'console': text}

payload=(inputs/'r5q-test12-payload.bin').read_bytes()
fields=[0x31304e4957513552,1,48,0xe0000000,0x10000000,8,1]
def window(values):return struct.pack('<QIIQQQQ',*values)
results=[run_case('valid-exclusive-reserved-window',payload,window(fields),
                  'TEST12 KERNEL PREPARATION VERIFIED',0)]
results.append(run_case('missing-ownership-protocol',payload,None,'ownership handoff refused'))
for index,label,value in [(0,'bad-magic',0),(1,'wrong-version',2),(2,'short-structure',40),
                           (3,'wrong-base',0xd0000000),(4,'oversized-window',0x20000000),
                           (5,'wrong-cache-attributes',1),(6,'unclaimed-resource',0)]:
    values=fields.copy();values[index]=value
    results.append(run_case(label,payload,window(values),'ownership handoff refused'))
# Malformed Mach-O must still be rejected before using the loan.
pos=8
while payload[pos:pos+16].split(b'\0')[0]!=b'kernel':
    size=struct.unpack_from('<I',payload,pos+16)[0];pos+=24+((size+15)&~15)
data=bytearray(payload);struct.pack_into('<I',data,pos+24+20,0xffffffff)
results.append(run_case('bad-kernel-command-table',bytes(data),window(fields),'command table bounds'))
print(json.dumps({'result':'passed','engine':'actual relocated Test 12 EFI loader in Unicorn ARM64',
 'scope':'reserved ownership handoff, Mach-O load, DeviceTree/boot_args and clean return; no CPU/MMU switch',
 'cases':results},indent=2))
