"""Run the actual preparation-only EFI loader with emulated UEFI services."""
import gc
import json
import pathlib
import struct
import unicorn.arm64_const as arm
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_CPU_ARM64_MAX, UC_ARM64_REG_PC, UC_ARM64_REG_X9, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP

inputs = pathlib.Path('/inputs')
binary = (inputs / 'r5q-test32-loader.efi').read_bytes()
pe = struct.unpack_from('<I', binary, 60)[0]
optional = pe + 24
entry = struct.unpack_from('<I', binary, optional + 16)[0]
preferred = struct.unpack_from('<Q', binary, optional + 24)[0]
image_size, header_size = struct.unpack_from('<II', binary, optional + 56)
section_headers = optional + struct.unpack_from('<H', binary, pe + 20)[0]
BASE = 0x9a000000
ENV, STACK = 0x60000000, 0x61000000
def run_case(name, payload, windowdata, expected_text, expected_status=0x8000000000000001,
             map_variant='valid', gic_expected=None):
    gc.collect()
    expected_claims = []
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
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
        elif pc==getmap:
            desc=struct.pack('<IIQQQQ',1,0,BASE,0,(image_size+4095)//4096,0xf)+bytes(8)
            desc+=struct.pack('<IIQQQQ',4,0,STACK,0,16,0xf)+bytes(8)
            if map_variant=='overflow':
                desc=struct.pack('<IIQQQQ',4,0,0xfffffffffffff000,0,2,0xf)+bytes(8)
            q(machine.reg_read(UC_ARM64_REG_X0),len(desc))
            machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),desc)
            q(machine.reg_read(UC_ARM64_REG_X2),1)
            q(machine.reg_read(UC_ARM64_REG_X3),0 if map_variant=='zero-descriptor' else 48)
            machine.mem_write(machine.reg_read(UC_ARM64_REG_X4),struct.pack('<I',1))
        elif pc==allocate:
            raise AssertionError('Reserved-window loader must not allocate ordinary RAM')
        else:assert pc==watchdog
        machine.reg_write(UC_ARM64_REG_X0,0)
    hook = uc.hook_add(UC_HOOK_CODE,service,begin=watchdog,end=allocate)
    # Unicorn has no board GIC device. Model only the three GIC CPU-interface
    # registers saved/restored by the actual stub; keep every other instruction.
    gic={0xc230:0xff,0xc667:1,0xc663:0}
    gic_initial=gic.copy()
    gic_accesses=[]
    def gic_service(machine,pc,size,user):
        word=struct.unpack('<I',bytes(machine.mem_read(pc,4)))[0]
        key=(word>>5)&0xffff
        assert key in gic and word&31==9
        if word&0xffe00000==0xd5200000:
            machine.reg_write(UC_ARM64_REG_X9,gic[key]);gic_accesses.append('read')
        else:
            gic[key]=machine.reg_read(UC_ARM64_REG_X9);gic_accesses.append('write')
        machine.reg_write(UC_ARM64_REG_PC,pc+4)
    gic_hooks=[]
    for offset in range(0x1000,0x1000+struct.unpack_from('<I',binary,section_headers+8)[0],4):
        word=struct.unpack('<I',bytes(uc.mem_read(BASE+offset,4)))[0]
        if word&0xffe00000 in [0xd5200000,0xd5000000] and (word>>5)&0xffff in gic:
            gic_hooks.append(uc.hook_add(UC_HOOK_CODE,gic_service,begin=BASE+offset,end=BASE+offset))
    uc.reg_write(UC_ARM64_REG_X0,0x1234);uc.reg_write(UC_ARM64_REG_X1,st)
    uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00);uc.reg_write(UC_ARM64_REG_X30,sentinel)
    preserved={getattr(arm,'UC_ARM64_REG_X'+str(i)):0x11000000+i for i in range(18,30)}
    preserved[arm.UC_ARM64_REG_TPIDR_EL1]=0x12345000
    preserved[arm.UC_ARM64_REG_TPIDR_EL0]=0x23456000
    preserved[arm.UC_ARM64_REG_TPIDRRO_EL0]=0x34567000
    for register,value in preserved.items():uc.reg_write(register,value)
    try:
        uc.emu_start(BASE+entry,0,timeout=45000000,count=100000000)
    except UcError:
        pc=uc.reg_read(UC_ARM64_REG_PC)
        import sys
        print(json.dumps({'pc':hex(pc),'instructions':bytes(uc.mem_read(pc,16)).hex(),
                          'console_tail':''.join(transcript)[-1800:]}),file=sys.stderr)
        raise
    uc.hook_del(hook)
    for item in gic_hooks:uc.hook_del(item)
    expected_gic = (['read']*3+['write']*3 if expected_status==0 else ['read']*3) if gic_expected is None else gic_expected
    assert gic==gic_initial and gic_accesses==expected_gic
    assert all(uc.reg_read(register)==value for register,value in preserved.items())
    text=''.join(transcript).replace('\r','')
    assert returned == [expected_status], (name, returned, text)
    assert claims == expected_claims, (name, claims, text)
    assert expected_text in text, (name, text)
    count=struct.unpack('<I',bytes(uc.mem_read(0xc1600008,4)))[0]
    trace=bytes(uc.mem_read(0xc1600010,count)).decode()
    filtered=''.join(line for line in trace.splitlines(keepends=True) if not line.startswith('[LAB]') and line!='RB0\n')
    assert filtered==text
    # RB0 is the self-test stub's tag; a kernel checkpoint comes back through the kernel's return slot instead
    assert trace.count('RB0\n') == 0, (name, trace[-3000:])
    if expected_status==0:
        for tag in ['[LAB] firmware map validated; per-region clean skipped (set/way flush follows)',
                    '[LAB] before MMU-off handoff', '[LAB] context restored', 'STOPPED AT CHECKPOINT 1']:
            assert tag in trace, (name, tag, trace[-3000:])
    return {'case': name, 'result': 'passed', 'claims': claims, 'console': text, 'ram_stage_trace': trace}

payload=(inputs/'r5q-test32-payload.bin').read_bytes()
fields=[0x31304e4957513552,1,48,0xe0000000,0x10000000,8,1]
def window(values):return struct.pack('<QIIQQQQ',*values)
results=[run_case('actual-efi-xnu-checkpoint1-context-return',payload,window(fields),
                  'TEST32 REAL XNU CHECKPOINT 1 VERIFIED',0)]
results.append(run_case('refuse-zero-cache-map-descriptor',payload,window(fields),
    'handoff refused: invalid firmware memory map',map_variant='zero-descriptor'))
results.append(run_case('refuse-overflowing-cache-map',payload,window(fields),
    'handoff refused: overflowing firmware memory descriptor',map_variant='overflow'))
oversized=bytearray(payload);struct.pack_into('<I',oversized,4,0x03f00000)
results.append(run_case('refuse-payload-in-log-map-scratch',bytes(oversized),window(fields),
    'lab payload overlaps reserved log/map scratch',gic_expected=[]))
print(json.dumps({'result':'passed','engine':'actual relocated EFI and pinned XNU instructions in Unicorn ARM64 MAX',
 'gic_model':'PMR/IGRPEN1/BPR1 sysreg effects explicitly emulated; no board GIC present',
 'register_checks':'x18-x29 and TPIDR_EL1/EL0/RO preserved with nonzero fixture values',
 'scope':'full EFI -> Test 31 hand-off (no per-region clean, ABL set/way + shutdown order) -> early XNU checkpoint1 -> loader context return; emulation does not reproduce phone MMU/cache/hypervisor behavior',
 'cases':results},indent=2))
