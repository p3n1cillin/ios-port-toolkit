"""Execute the corrected shim, checking ring wrap, container bounds and cache calls."""
import gc
import json
import pathlib
import re
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1

build=pathlib.Path('/inputs')
shim=(build/'r5q-test234-shim.bin').read_bytes()
fd=(build/'r5q-test234-uefi.fd').read_bytes()
payload=(build/'r5q-test234-payload.bin').read_bytes()
symbols={}
for line in (build/'r5q-test234-shim-symbols.txt').read_text().splitlines():
    parts=line.split()
    if len(parts)==3 and re.fullmatch('[0-9a-fA-F]+',parts[0]):
        symbols[parts[2]]=int(parts[0],16)
assert symbols['_Payload']==len(shim)
FD,PAY,SOURCE,MDSS,INFO,LOG=0x9fc00000,0xf0000000,0x80080000,0x0ae00000,0x9e300000,0xc1600000
CAP=0x200000-16
cases=[('valid',0),('valid-display',0),('bad-magic',0),('oversized',0),
       ('too-short',0),('outside-image-container',0),('fd-in-place',0),
       ('ring-end-wrap',CAP-2),('already-wrapped-index',CAP+37),
       ('uint32-index-wrap',0xfffffffc),('bad-log-magic',123)]
results=[]
for name,seed in cases:
    gc.collect()
    content=bytearray(payload)
    if name=='bad-magic':content[:4]=b'FAIL'
    if name=='oversized':struct.pack_into('<I',content,4,0x4000000)
    if name=='too-short':struct.pack_into('<I',content,4,24)
    if name=='outside-image-container':struct.pack_into('<I',content,4,0x3e00000)
    valid=name not in ['bad-magic','oversized','too-short','outside-image-container']
    address=FD-len(shim) if name=='fd-in-place' else SOURCE
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    uc.ctl_set_tcg_buffer_size(32*1024*1024)
    for base,size in [(SOURCE,0x2000000),(FD-0x1000,0x2000000),(PAY,0x4000000),
                      (MDSS,0x100000),(INFO,0x100000),(LOG,0x201000)]:uc.mem_map(base,size)
    uc.mem_write(address,shim+fd+bytes(content)+bytes(32))
    uc.mem_write(PAY,b'OLD!OLD!'+bytes([0xa5])*(len(payload)+32))
    magic=0x12345678 if name=='bad-log-magic' else 0x4d474f4c
    uc.mem_write(LOG,struct.pack('<4I',123,magic,seed,0xaabbccdd))
    uc.mem_write(LOG+0x200000,bytes([0xa5])*64)
    if name=='valid-display':
        uc.mem_map(0x9c400000,0xa00000)
        for offset,value in [(0x25014,0x9c400000),(0x25024,4320),(0x25000,2400<<16|1080),(0x20f4,2)]:
            uc.mem_write(MDSS+offset,struct.pack('<I',value))
    reached=[];writes=[];cache_calls=[]
    def stop(machine,pc,size,user):reached.append(pc);machine.emu_stop()
    def ring_write(machine,access,where,size,value,user):
        if LOG+16<=where<LOG+0x200000:
            assert size==1
            writes.append((where,value))
    def cache_call(machine,pc,size,user):
        kind='invalidate' if pc==address+symbols['_CleanInvalidateRange'] else 'clean'
        cache_calls.append((kind,machine.reg_read(UC_ARM64_REG_X0),machine.reg_read(UC_ARM64_REG_X1)))
    uc.hook_add(UC_HOOK_CODE,stop,begin=FD,end=FD)
    uc.hook_add(UC_HOOK_MEM_WRITE,ring_write,begin=LOG,end=LOG+0x200fff)
    for symbol in ['_CleanInvalidateRange','_CleanRange']:
        pc=address+symbols[symbol];uc.hook_add(UC_HOOK_CODE,cache_call,begin=pc,end=pc)
    uc.emu_start(address,0,timeout=20000000,count=50000000)
    assert reached==[FD],name
    assert bytes(uc.mem_read(FD,len(fd)))==fd,name
    if valid:
        assert bytes(uc.mem_read(PAY,len(payload)))==payload,name
        rounded=(len(payload)+15)&~15
        assert bytes(uc.mem_read(PAY+rounded,16))==bytes([0xa5])*16,name
    else:
        assert bytes(uc.mem_read(PAY,8))==bytes(8),name
        assert bytes(uc.mem_read(PAY+8,16))==bytes([0xa5])*16,name
    expected=[] if name=='fd-in-place' else [('invalidate',FD,0x300000)]
    expected += [('clean',FD,0x300000),('invalidate',PAY,64),('clean',PAY,8)]
    if valid:expected += [('invalidate',PAY,(len(payload)+15)&~15),('clean',PAY,(len(payload)+15)&~15)]
    assert cache_calls==expected,(name,cache_calls,expected)
    boot,actual_magic,next_index,previous=struct.unpack('<4I',bytes(uc.mem_read(LOG,16)))
    assert (boot,actual_magic,previous)==(123,magic,0xaabbccdd),name
    assert bytes(uc.mem_read(LOG+0x200000,64))==bytes([0xa5])*64,name
    if name=='bad-log-magic':
        assert writes==[] and next_index==seed
        trace=''
    else:
        assert next_index==(seed+len(writes))&0xffffffff,name
        for count,(where,value) in enumerate(writes):
            assert where==LOG+16+(((seed+count)&0xffffffff)%CAP),(name,count,hex(where))
        trace=bytes(value for where,value in writes).decode('ascii')
        assert trace.startswith('\n[TEST17] BootShim entry\n'),(name,trace)
        assert re.search(r'SCTLR_EL1=0x[0-9a-f]{16}\n',trace)
        assert 'CurrentEL=0x0000000000000004\n' in trace
        assert f'source FD=0x{address+len(shim):016x}\n' in trace
        assert '[TEST17] BootShim: entering SEC\n' in trace
        assert ('XNUP copied and cleaned' in trace)==valid
        assert ('XNUP payload rejected' in trace)==(not valid)
    results.append({'case':name,'result':'passed','initial_log_index':seed,
                    'logged_bytes':len(writes),'cache_range_calls':len(cache_calls),'trace':trace})
print(json.dumps({'result':'passed','engine':'actual Test17 ARM64 shim in Unicorn',
 'scope':'ring/index/container/copy bounds and cache-range call arguments; cache visibility on phone is not emulated',
 'cases':results},indent=2))
