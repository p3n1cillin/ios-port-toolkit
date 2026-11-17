"""Run the actual preparation-only EFI loader with emulated UEFI services."""
import gc
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP

inputs = pathlib.Path('/inputs')
binary = (inputs / 'r5q-candidate-loader.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
preferred = struct.unpack_from('<Q', binary, optional + 24)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
section_headers = optional + struct.unpack_from('<H', binary, pe + 20)[0]
BASE = 0x9a000000
ENV, STACK = 0x60000000, 0x61000000
def run_case(name, payload, banks, expected_text, expected_status=0x8000000000000001,
             expected_claims=None, descriptor_size=48, allocation=(0xe0000000, 0x10000)):
    gc.collect()
    expected_claims = expected_claims or []
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
        if pc==locate:q(machine.reg_read(UC_ARM64_REG_X2),gop)
        elif pc==output:
            transcript.append(bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X1),6)).decode('utf-16-le').split('\0')[0])
        elif pc==getmap:
            desc=b''.join(struct.pack('<IIQQQQ',7,0,base,0,pages,0xf)+bytes(8) for base,pages in banks)
            q(machine.reg_read(UC_ARM64_REG_X0),len(desc))
            machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),desc)
            q(machine.reg_read(UC_ARM64_REG_X2),1);q(machine.reg_read(UC_ARM64_REG_X3),descriptor_size)
            machine.mem_write(machine.reg_read(UC_ARM64_REG_X4),struct.pack('<I',1))
        elif pc==allocate:
            claim=struct.unpack('<Q',bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X3),8)))[0]
            assert (machine.reg_read(UC_ARM64_REG_X0),machine.reg_read(UC_ARM64_REG_X1),
                    machine.reg_read(UC_ARM64_REG_X2),claim)==(2,2,allocation[1],allocation[0])
            claims.append(hex(claim))
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

payload = (inputs / 'r5q-test8-payload.bin').read_bytes()
banks=[(0x180000000,0x100000),(0xe0000000,0x10000),(0x80000000,0x30000)]
results=[]
results.append(run_case('valid-with-larger-outside-banks',payload,banks,'TEST8 KERNEL PREPARATION VERIFIED',0,['0xe0000000']))
results.append(run_case('firmware-fragmented-window',payload,[(0xe0010000,0xffe0)],
    'TEST8 KERNEL PREPARATION VERIFIED',0,['0xe2000000'],allocation=(0xe2000000,0xc000)))
results.append(run_case('outside-banks-only',payload,banks[:1]+banks[2:],'no usable RAM window'))
results.append(run_case('zero-descriptor-size',payload,banks,'invalid descriptor bounds',descriptor_size=0))
results.append(run_case('overflowing-memory-descriptor',payload,[(0xfffffffffffff000,2)],'no usable RAM window'))
def altered(offset,fmt,value):
    data=bytearray(payload);struct.pack_into(fmt,data,offset,value);return bytes(data)
results.append(run_case('oversized-payload',altered(4,'<I',0xffffffff),banks,'no payload at'))
data=bytearray(payload);data[8:24]=b'X'*16
results.append(run_case('unterminated-entry-name',bytes(data),banks,'invalid name or bounds'))
# Locate the kernel item, and exercise the parser with the real EFI code.
pos=8
while payload[pos:pos+16].split(b'\0')[0] != b'kernel':
    size=struct.unpack_from('<I',payload,pos+16)[0];pos+=24+((size+15)&~15)
k=pos+24
results.append(run_case('truncated-kernel',altered(pos+16,'<I',4),banks,'invalid name or bounds'))
results.append(run_case('oversized-command-table',altered(k+20,'<I',0xffffffff),banks,'command table bounds'))
results.append(run_case('short-load-command',altered(k+32+4,'<I',4),banks,'command extent'))
# First LC_SEGMENT_64: its file extent must remain inside the kernel entry.
lc=k+32
while struct.unpack_from('<I',payload,lc)[0]!=0x19:lc+=struct.unpack_from('<I',payload,lc+4)[0]
results.append(run_case('segment-file-outside-payload',altered(lc+40,'<Q',0xfffffff0),banks,'segment bounds'))
# A source-valid but huge virtual span must be rejected before any segment write.
results.append(run_case('segment-outside-claimed-window',altered(lc+32,'<Q',0x20000000),banks,'does not fit claimed RAM',expected_claims=['0xe0000000']))
print(json.dumps({'result':'passed','engine':'actual relocated EFI code in Unicorn ARM64',
 'scope':'runtime window enforcement and malformed input rejection; no phone or XNU execution',
 'cases':results},indent=2))
