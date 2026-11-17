"""Run actual ARM64 manager ownership helpers against emulated UEFI services."""
import gc
import json
import pathlib
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X30, UC_ARM64_REG_SP

binary = pathlib.Path('/inputs/r5q-window-harness.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe+24
entry = struct.unpack_from('<I', binary, optional+16)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional+56)
sections = optional+struct.unpack_from('<H', binary, pe+20)[0]
BASE, ENV, STACK = 0x9a000000, 0x60000000, 0x61000000
ERR = 1 << 63
results = []
for case in ['success', 'descriptor-failure', 'wrong-type', 'owned', 'device-owned',
             'not-write-back', 'short-region', 'wrong-base', 'allocation-failure',
             'unexpected-allocation', 'install-failure', 'uninstall-failure']:
    gc.collect()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_tcg_buffer_size(8*1024*1024)
    for address, size in [(BASE,(image_size+4095)&~4095),(ENV,0x10000),(STACK,0x10000)]:
        uc.mem_map(address,size)
    uc.mem_write(BASE,binary[:header_size])
    for i in range(struct.unpack_from('<H',binary,pe+6)[0]):
        _,vs,va,size,offset=struct.unpack_from('<8sIIII',binary,sections+40*i)
        if size:uc.mem_write(BASE+va,binary[offset:offset+size])
    ds,bs=ENV,ENV+0x1000
    getdesc,allocate,free,install,uninstall,sentinel=[ENV+x for x in range(0x5000,0x5060,0x10)]
    def q(address,value):uc.mem_write(address,struct.pack('<Q',value))
    q(ds+56,getdesc);q(ds+32,allocate);q(ds+40,free)
    q(bs+128,install);q(bs+144,uninstall)
    for address in [getdesc,allocate,free,install,uninstall,sentinel]:uc.mem_write(address,b'\xc0\x03\x5f\xd6')
    calls=[];returned=[];windows=[]
    def service(machine,pc,size,user):
        if pc==sentinel:
            returned.append(machine.reg_read(UC_ARM64_REG_X0));machine.emu_stop();return
        status=0
        regs=[machine.reg_read(reg) for reg in [UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,
              UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6]]
        if pc==getdesc:
            calls.append('descriptor');assert regs[0]==0xe0000000
            base=0xe0010000 if case=='wrong-base' else 0xe0000000
            length=0xffff000 if case=='short-region' else 0x14000000
            attrs=0 if case=='not-write-back' else 8
            kind=2 if case=='wrong-type' else 1
            owner=0x123 if case=='owned' else 0
            device=0x456 if case=='device-owned' else 0
            machine.mem_write(regs[1],struct.pack('<QQQQI4xQQ',base,length,attrs,8,kind,owner,device))
            if case=='descriptor-failure':status=ERR|14
        elif pc==allocate:
            calls.append('allocate');assert regs[:4]==[2,1,12,0x10000000]
            assert struct.unpack('<Q',bytes(machine.mem_read(regs[4],8)))[0]==0xe0000000
            assert regs[5:]==[0x1234,0]
            if case=='allocation-failure':status=ERR|9
            if case=='unexpected-allocation':q(regs[4],0xd0000000)
        elif pc==install:
            calls.append('install');assert regs[2]==0
            window=struct.unpack('<QIIQQQQ',bytes(machine.mem_read(regs[3],48)))
            assert window==(0x31304e4957513552,1,48,0xe0000000,0x10000000,8,1)
            windows.append(regs[3]);q(regs[0],0x7654)
            if case=='install-failure':status=ERR|9
        elif pc==uninstall:
            calls.append('uninstall');assert regs[0]==0x7654 and regs[2]==windows[-1]
            if case=='uninstall-failure':status=ERR|7
        elif pc==free:
            calls.append('free');assert regs[:2]==[0xe0000000,0x10000000]
        else:raise AssertionError(pc)
        machine.reg_write(UC_ARM64_REG_X0,status)
    hook=uc.hook_add(UC_HOOK_CODE,service,begin=getdesc,end=sentinel)
    def execute(action):
        uc.reg_write(UC_ARM64_REG_X0,ds);uc.reg_write(UC_ARM64_REG_X1,bs)
        uc.reg_write(UC_ARM64_REG_X2,action);uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00)
        uc.reg_write(UC_ARM64_REG_X30,sentinel)
        uc.emu_start(BASE+entry,0,timeout=1000000,count=100000)
        return returned[-1]
    status=execute(0)
    if case in ['success','uninstall-failure']:
        assert status==0
        assert execute(0)==ERR|20  # no duplicate resource loan
        if case=='success':
            assert execute(1)==0 and calls.count('free')==1
            assert execute(1)==ERR|19
            assert struct.unpack('<Q',bytes(uc.mem_read(windows[0]+40,8)))[0]==0
        else:
            assert execute(1)==ERR|7 and 'free' not in calls
            assert struct.unpack('<Q',bytes(uc.mem_read(windows[0]+40,8)))[0]==1
    elif case=='descriptor-failure':assert status==ERR|14 and calls==['descriptor']
    elif case=='allocation-failure':assert status==ERR|9 and calls==['descriptor','allocate']
    elif case=='unexpected-allocation':assert status==ERR|33 and calls==['descriptor','allocate']
    elif case=='install-failure':assert status==ERR|9 and calls==['descriptor','allocate','install','free']
    else:assert status==ERR|15 and calls==['descriptor']
    uc.hook_del(hook)
    results.append({'case':case,'result':'passed','calls':calls})
print(json.dumps({'result':'passed','engine':'actual ARM64 helper with real EDK2 types, emulated UEFI services',
                  'cases':results},indent=2))
