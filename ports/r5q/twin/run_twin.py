"""Run a phone test image on the r5q virtual twin (QEMU). EMULATION ONLY — never touches the phone.

usage: python3 run_twin.py TEST [TEST ...]        e.g.  run_twin.py 17 18 29 30

For each test: boots port/build/r5q-test<N>-kernel.bin (the exact image the phone gets, BootShim + FD +
payload) at 0x80100000 under the EL2 uH stand-in, then reads the Samsung RAM ring (like last_kmsg) and the
framebuffer out of guest memory, applies the phone runner's ordered marker checks, and writes
port/build/twin/test<N>/{serial.log, ring.txt, screen.png, receipt.json}.
"""
import json
import os
import pathlib
import re
import select
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
BUILD = ROOT / 'port/build'
VARIANT = ''.join(['-realfd' if os.environ.get('TWIN_REAL_FD') else '',
                   ('-cache' + os.environ.get('TWIN_CACHE_PAGES', '640')) if os.environ.get('TWIN_CACHE') else '',
                   '-noloader' if os.environ.get('TWIN_MAP') == 'noloader' else '',
                   '-secure' if os.environ.get('TWIN_SECURE') == '1' else ''])
WORK =pathlib.Path('/var/tmp/codex-ios-r5q/work/twin-runs' + VARIANT)
RING, RING_LEN, RING_SIZE = 0xC1600000, 0x200000, 2097136
FB, FB_W, FB_H = 0x9C400000, 1080, 2400

SHIM = ['[TEST17] BootShim entry', '[TEST17] BootShim: entering SEC']
LAB = ['[LAB] before MMU-off handoff']
FULL = lambda n: ['RB0', '[LAB] context restored', f'TEST{n} RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD']
STAGES = {19: ['[LAB] STAGE 1: PS_HOLD reset before MMU-off'], 20: ['RB0'], 21: ['RB0', 'RB1', 'RB2'],
          22: ['RB0', 'RB1', 'RB2', 'RB3'], 23: ['RB0', 'RB1', 'RB2', 'RB3', 'RB4', '[LAB] context restored',
          'TEST23 RETURN PATH SELF TEST VERIFIED', '[TEST12] Resetting via PSHOLD']}
WDT_TAG = '[LAB] WDT ARMED: bite 8 s guard over the hand-off'


# The phone's own RAM log, captured by the Test 17 hardware run, as the twin's starting state:
# same contents, and the same wrapped index (1603 wraps, derived from where the phone's SEC writer
# landed its [LABSEC] lines: 16 bytes of drift per wrap plus 16 for the header).
PHONE_RING = ROOT.parent.parent / 'outputs/S10-Lite-native-iOS-port/test17/validation/automatic-20261116-093931/last_kmsg.bin'
PHONE_WRAPS, PHONE_SHIM_START = 1603, 2028159


def phone_ring_image(d):
    if os.environ.get('TWIN_FRESH_RING') or not PHONE_RING.exists():
        return None
    buf = PHONE_RING.read_bytes()[:RING_SIZE]
    idx = PHONE_WRAPS * RING_SIZE + PHONE_SHIM_START
    img = d / 'ring_init.bin'
    img.write_bytes(struct.pack('<4I', 1, 0x4D474F4C, idx, idx) + buf)
    return img


def expected(n):
    if n == 17:
        return SHIM + ['TEST1 HANDOFF OK', '[TEST12] Resetting via PSHOLD']
    if n == 18:
        return SHIM + ['TEST18 FIRMWARE RETURN PATH ONLY'] + LAB + FULL(18)
    if 19 <= n <= 23:
        return SHIM + [f'TEST{n} FIRMWARE RETURN PATH ONLY'] + LAB + STAGES[n]
    if n == 24:
        return SHIM + ['TEST24 FIRMWARE RETURN PATH ONLY'] + LAB + [
            '[LAB] WDT PROBE: arming bark 5 s, bite 8 s; not petting', '[LAB] WDT PROBE: armed, waiting for the bite']
    if 25 <= n <= 28:
        base = {25: 20, 26: 21, 27: 22, 28: 23}[n]
        tail = [t.replace('TEST23', 'TEST28') for t in STAGES[base]]
        return SHIM + [f'TEST{n} FIRMWARE RETURN PATH ONLY'] + LAB + [WDT_TAG] + tail
    if n == 29:
        return SHIM + ['TEST29 FIRMWARE RETURN PATH ONLY', '[LAB] ctx+stack flushed to PoC (explicit)'] + LAB + FULL(29)
    if n == 30:
        return SHIM + ['TEST30 FIRMWARE RETURN PATH ONLY'] + LAB + FULL(30)
    if n == 37:   # the twin's stand-in has no Qualcomm drivers: the probe must report them missing and finish
        return SHIM + ['TEST37 PROBE usb device protocol MISSING', 'TEST37 PROBE block io handles 0, raw units 0',
                       'TEST37 PROBE conventional total', 'TEST37 FULL FIRMWARE PROBE DONE', '[TEST12] Resetting via PSHOLD']
    if n == 35:
        return SHIM + ['TEST35 REAL XNU TIMED RUN 60 S', '[LAB] firmware map validated; per-region clean skipped'] + LAB + [
            '[LAB] context restored', '== KERNEL LOG ==', 'Darwin Kernel Version', '== END OF LOG ==',
            'TEST35 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD']
    if n == 34:
        return SHIM + ['TEST34 REAL XNU TIMED RUN 25 S', '[LAB] firmware map validated; per-region clean skipped'] + LAB + [
            '[LAB] context restored', '== KERNEL LOG ==', 'Darwin Kernel Version', '== END OF LOG ==',
            'TEST34 REAL XNU TIMED RUN RETURNED', '[TEST12] Resetting via PSHOLD']
    if n == 33:
        return SHIM + ['TEST33 REAL XNU EARLY CHECKPOINT 5', '[LAB] firmware map validated; per-region clean skipped'] + LAB + [
            '[LAB] context restored', 'STOPPED AT CHECKPOINT 5', 'TEST33 REAL XNU CHECKPOINT 5 VERIFIED', '[TEST12] Resetting via PSHOLD']
    if n == 32:
        return SHIM + ['TEST32 REAL XNU EARLY CHECKPOINT 1', '[LAB] firmware map validated; per-region clean skipped'] + LAB + [
            '[LAB] context restored', 'STOPPED AT CHECKPOINT 1', 'TEST32 REAL XNU CHECKPOINT 1 VERIFIED', '[TEST12] Resetting via PSHOLD']
    if n == 31:
        return SHIM + ['TEST31 FIRMWARE RETURN PATH ONLY', '[LAB] firmware map validated; per-region clean skipped (set/way flush follows)'] + LAB + FULL(31)
    return SHIM


def build():
    out = WORK / 'twin.elf'
    WORK.mkdir(parents=True, exist_ok=True)
    srcs = [HERE / 'twin.c', HERE / 'el2.S', HERE / 'link.ld']
    if out.exists() and all(out.stat().st_mtime > s.stat().st_mtime for s in srcs):
        return out
    flags = ['--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-nostdlib', '-mgeneral-regs-only',
             '-mstrict-align', '-O1', '-Wall', '-Wno-unused-function']
    if os.environ.get('TWIN_REAL_FD'):
        flags += ['-DREAL_FD=1', '-DMMIO_LOG_LIMIT=400']
    if os.environ.get('TWIN_CACHE'):
        flags += ['-DCACHE_MODEL=1', '-DCM_CAPACITY_PAGES=' + os.environ.get('TWIN_CACHE_PAGES', '640')]
    if os.environ.get('TWIN_MAP') == 'noloader':
        flags += ['-DMAP_NOLOADER=1']
    if os.environ.get('TWIN_SECURE') == '1':
        flags += ['-DSECURE_EL3=1']
    subprocess.run(['clang', *flags, '-c', str(HERE / 'twin.c'), '-o', str(WORK / 'twin.o')], check=True)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(HERE / 'el2.S'), '-o', str(WORK / 'el2.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(HERE / 'link.ld'), str(WORK / 'el2.o'), str(WORK / 'twin.o'), '-o', str(out)], check=True)
    return out


def secure_machine(cmd, work):
    """TrustZone layer (TWIN_SECURE=1): virt,secure=on + our boot ROM (pen.S) as -bios, so QEMU has no PSCI
    of its own; the twin starts at EL3, secures the GIC as Qualcomm TZ does, and answers PSCI itself."""
    pen = work / 'pen.bin'
    if not pen.exists() or pen.stat().st_mtime < (HERE / 'pen.S').stat().st_mtime:
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(HERE / 'pen.S'), '-o', str(work / 'pen.o')], check=True)
        subprocess.run(['ld.lld', '-Ttext=0', '--image-base=0', '-e', '_start', str(work / 'pen.o'), '-o', str(work / 'pen.elf')], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', str(work / 'pen.elf'), str(pen)], check=True)
    m = cmd.index('-M') + 1
    cmd[m] = cmd[m].replace('virtualization=on', 'virtualization=on,secure=on')
    q = cmd.index('/usr/bin/qemu-system-aarch64')
    cmd[q:q] = ['--ro-bind', str(pen), '/pen.bin']
    cmd[cmd.index('-nic'):cmd.index('-nic')] = ['-bios', '/pen.bin']
    return cmd


def hmp(sock_path, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(30)
    s.connect(str(sock_path))
    buf = b''
    while b'(qemu)' not in buf:
        buf += s.recv(4096)
    s.sendall(cmd.encode() + b'\n')
    buf = b''
    while buf.count(b'(qemu)') < 1:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    return buf.decode(errors='replace')


def png(path, raw, w, h, step):
    rows = []
    for y in range(0, h, step):
        row = bytearray(b'\0')
        base = y * w * 4
        for x in range(0, w, step):
            b, g, r = raw[base + 4 * x], raw[base + 4 * x + 1], raw[base + 4 * x + 2]
            row += bytes((r, g, b))
        rows.append(bytes(row))
    ow, oh = (w + step - 1) // step, (h + step - 1) // step
    chunk = lambda k, b: struct.pack('>I', len(b)) + k + b + struct.pack('>I', zlib.crc32(k + b))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))


def run(n, timeout=240):
    kernel = BUILD / f'r5q-test{n}-kernel.bin'
    if not kernel.exists():
        return {'test': n, 'result': 'no kernel image', 'path': str(kernel)}
    twin = build()
    d = WORK / f'test{n}'
    subprocess.run(['rm', '-rf', str(d)])
    d.mkdir(parents=True)
    mon = d / 'mon.sock'
    ring_img = phone_ring_image(d)
    out = BUILD / ('twin' + VARIANT) / f'test{n}'
    out.mkdir(parents=True, exist_ok=True)
    cmd = ['/usr/bin/bwrap', '--unshare-all', '--die-with-parent', '--new-session',
           '--ro-bind', '/usr', '/usr', '--ro-bind', '/bin', '/bin', '--ro-bind', '/lib', '/lib', '--ro-bind', '/lib64', '/lib64',
           '--ro-bind', '/etc/alternatives', '/etc/alternatives', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
           '--ro-bind', str(twin), '/twin.elf', '--ro-bind', str(kernel), '/kernel.bin', '--bind', str(d), str(d),
           '/usr/bin/qemu-system-aarch64', '-M', 'virt,gic-version=3,virtualization=on,highmem=off', '-cpu', 'cortex-a76',
           '-smp', '1', '-m', '3G', '-accel', 'tcg,tb-size=256',
           '-device', 'loader,file=/kernel.bin,addr=0x80100000,force-raw=on',
           '-device', 'loader,file=/twin.elf,cpu-num=0',
           '-nic', 'none', '-display', 'none', '-serial', 'stdio', '-monitor', f'unix:{mon},server,nowait', '-no-reboot']
    if ring_img:
        cmd[cmd.index('-nic'):cmd.index('-nic')] = ['-device', f'loader,file={ring_img},addr={RING:#x},force-raw=on']
    if os.environ.get('TWIN_SECURE') == '1':
        secure_machine(cmd, WORK)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    start = time.monotonic()
    lines, partial, outcome, bite_at = [], b'', None, None
    while outcome is None:
        now = time.monotonic()
        if bite_at and now > bite_at:
            outcome = 'watchdog bite (emulated: armed, no reset before the bite time)'
            break
        if now - start > timeout:
            outcome = 'hang: nothing ended the run (on the phone: stuck until a manual reset)'
            break
        r, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not r:
            if proc.poll() is not None:
                outcome = f'QEMU exited ({proc.returncode})'
            continue
        data = os.read(proc.stdout.fileno(), 65536)
        if not data:
            outcome = 'QEMU closed its output'
            break
        partial += data
        *done, partial = partial.split(b'\n')
        for raw in done:
            line = raw.decode(errors='replace').rstrip('\r')
            lines.append(line)
            m = re.search(r'\[TWIN\] WDT armed bite_ms=(\d+)', line)
            if m:
                bite_at = time.monotonic() + int(m.group(1)) / 1000 + 2
            if line.startswith('[TWIN] RESET'):
                outcome = 'reset via PS_HOLD (on the phone: returns to Android by itself)'
            elif line.startswith('[TWIN] HANG') or line.startswith('[TWIN] PARKED'):
                outcome = outcome or 'stopped: firmware stand-in or EL2 parked (see serial log)'
    elapsed = round(time.monotonic() - start, 1)
    try:
        for c in (f'pmemsave {RING:#x} {RING_LEN:#x} "{d}/ring.bin"', f'pmemsave {FB:#x} {FB_W * FB_H * 4:#x} "{d}/fb.bin"'):
            reply = hmp(mon, c)
            lines.append('[runner] monitor: ' + c + ' -> ' + ' '.join(reply.split())[-160:])
        hmp(mon, 'quit')
    except OSError as e:
        lines.append(f'[runner] monitor error: {e}')
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
    (out / 'serial.log').write_text('\n'.join(lines) + '\n')
    ring_text = ''
    if (d / 'ring.bin').exists():
        rb = (d / 'ring.bin').read_bytes()
        magic, idx = struct.unpack_from('<II', rb, 4)
        buf = rb[16:16 + RING_SIZE]
        if ring_img:
            begin = PHONE_SHIM_START
            end_pos = idx % RING_SIZE
            text = buf[begin:end_pos] if end_pos >= begin else buf[begin:] + buf[:end_pos]
            (out / 'ring-full.bin').write_bytes(rb)
        else:
            text = buf[:idx] if idx <= RING_SIZE else buf[idx % RING_SIZE:] + buf[:idx % RING_SIZE]
        ring_text = text.decode(errors='replace')
        (out / 'ring.txt').write_text(ring_text)
    if (d / 'fb.bin').exists():
        png(out / 'screen.png', (d / 'fb.bin').read_bytes(), FB_W, FB_H, 4)
    want = expected(n)
    pos, ordered, missing = 0, True, []
    for tag in want:
        at = ring_text.find(tag, pos)
        if at < 0:
            ordered = False
            missing.append(tag)
        else:
            pos = at + len(tag)
    tags = [l for l in ring_text.splitlines() if l.startswith('[LAB]') or re.fullmatch(r'RB\d', l)]
    receipt = {'test': n, 'engine': 'r5q virtual twin (QEMU virt EL2 + uH stand-in); EMULATION ONLY',
               'outcome': outcome, 'seconds': elapsed, 'markers_expected': want,
               'markers_confirmed': ordered, 'markers_missing': missing, 'ring_stage_tags': tags,
               'last_ring_line': ring_text.strip().splitlines()[-1] if ring_text.strip() else None,
               'twin_events': [l for l in lines if l.startswith('[TWIN]')][-12:],
               'cache_model': [l for l in lines if l.startswith('[TWIN-CACHE]')][-30:],
               'limits': 'no CPU cache model, real Mu UEFI replaced by a stand-in, real uH replaced by its decompiled configuration'}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


DONOR = pathlib.Path('/var/tmp/codex-ios-r5q/work/linux-source')
IOS_MILESTONES = [('loader', '== XNULOADER v3 =='), ('platform r5q', 'platform r5q'),
                  ('window', 'phys window'), ('kernel entry', 'jumping to'),
                  ('XNU banner', 'Darwin Kernel Version'), ('IOKit', 'IOKit'),
                  ('root mounted', 'BSD root:'), ('launchd', 'launchd'),
                  ('SpringBoard', 'SpringBoard'), ('home screen', 'systemShellDidFinishLaunching'),
                  ('panic', 'panic('), ('kernel returned', 'kernel returned')]


# iOS mode defaults to the TrustZone layer (TWIN_SECURE=0 for the plain EL2 twin); TWIN_SMP = QEMU cores
IOS_SECURE = os.environ.get('TWIN_SECURE', '1') == '1'
IOS_SMP = int(os.environ.get('TWIN_SMP', '1'))


def build_ios():
    """twin.elf for iOS mode: -DIOS_MODE, linked at 0x40200000 (QEMU RAM below the phone's DRAM)."""
    work = pathlib.Path('/var/tmp/codex-ios-r5q/work/twin-runs-ios' + ('' if IOS_SECURE else '-nosecure'))
    work.mkdir(parents=True, exist_ok=True)
    out = work / 'twin.elf'
    srcs = [HERE / 'twin.c', HERE / 'el2.S', HERE / 'link.ld', pathlib.Path(__file__)]
    if out.exists() and all(out.stat().st_mtime > s.stat().st_mtime for s in srcs):
        return work, out
    ld = (HERE / 'link.ld').read_text().replace('. = 0xFC000000;', '. = 0x40200000;')   # above QEMU's DTB
    assert '0x40200000' in ld
    (work / 'link-ios.ld').write_text(ld)
    flags = ['--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-nostdlib', '-mgeneral-regs-only',
             '-mstrict-align', '-O1', '-Wall', '-Wno-unused-function', '-DIOS_MODE=1'] + (['-DSECURE_EL3=1'] if IOS_SECURE else [])
    subprocess.run(['clang', *flags, '-c', str(HERE / 'twin.c'), '-o', str(work / 'twin.o')], check=True)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(HERE / 'el2.S'), '-o', str(work / 'el2.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(work / 'link-ios.ld'), str(work / 'el2.o'), str(work / 'twin.o'), '-o', str(out)], check=True)
    return work, out


def run_ios(secs=600, shots=(60, 180, 300, 420, 540), cmdline_extra=''):
    """Boot iOS on the twin: production r5q loader + source XNU + iOS 16.5 userland. EMULATION ONLY."""
    work, twin = build_ios()
    # TWIN_LOADER: loader-twin-ios.efi (production + the twin's UART, default) or loader-twin-phone.efi
    # (exactly what the phone would run: no UART, every loader line mirrored into the Samsung RAM ring)
    loader = pathlib.Path(os.environ.get('TWIN_LOADER') or BUILD / 'twin-ios/loader-twin-ios.efi').resolve()
    # TWIN_KERNEL: another kernel (e.g. port/build/xnu-r5q.macho, the r5q overlay build); TWIN_OUT_TAG: results subfolder suffix
    kernel = pathlib.Path(os.environ.get('TWIN_KERNEL') or BUILD / 'xnu-source-rebuild.macho').resolve()
    out = BUILD / 'twin-ios' / (loader.stem + ('-secure' if IOS_SECURE else '') + (f'-smp{IOS_SMP}' if IOS_SMP > 1 else '')
                                + os.environ.get('TWIN_OUT_TAG', ''))
    out.mkdir(parents=True, exist_ok=True)
    root, system, ram_img = DONOR / 'userland/root.apfs', DONOR / 'userland/system.apfs', DONOR / 'qemu/ram.img'
    for f in (loader, kernel, root, system, ram_img):
        assert f.exists(), f
    d = work / 'run'
    subprocess.run(['rm', '-rf', str(d)])
    d.mkdir(parents=True)
    payload = d / 'payload.bin'
    drive = os.environ.get('TWIN_INPUT') == '1'      # touch panel + buttons, QMP at <run>/qmp.sock (twin_ctl.py)
    if drive:
        cmdline_extra = ('virtio_input=1 ' + cmdline_extra).strip()
    cmdline = f'loader_return_after={secs} epoch={int(time.time())} {cmdline_extra}'.strip()
    subprocess.run(['python3', str(DONOR / 'xnuloader/mkpayload.py'), '-o', str(payload),
                    f'loader.efi={loader}', f'kernel={kernel}', f'ramdisk={root}',
                    '--rdres', f'{system.stat().st_size:x}', '--cmdline', cmdline], check=True, stdout=subprocess.DEVNULL)
    assert payload.stat().st_size <= 0x4000000
    mon = d / 'mon.sock'
    ring_img = phone_ring_image(d)
    # guest RAM 0x40000000-0x280000000 (9 GiB): the donor's sparse ram.img already holds system.apfs at
    # 0xFC000000 (= the loader's resident address for a window at 0xF4000000); mapped private, never written
    cmd = ['/usr/bin/bwrap', '--unshare-all', '--die-with-parent', '--new-session',
           '--ro-bind', '/usr', '/usr', '--ro-bind', '/bin', '/bin', '--ro-bind', '/lib', '/lib', '--ro-bind', '/lib64', '/lib64',
           '--ro-bind', '/etc/alternatives', '/etc/alternatives', '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
           '--ro-bind', str(twin), '/twin.elf', '--ro-bind', str(ram_img), '/ram.img', '--bind', str(d), str(d),
           '/usr/bin/qemu-system-aarch64', '-M', 'virt,gic-version=3,virtualization=on', '-cpu', 'cortex-a76',
           '-smp', str(IOS_SMP), '-m', '9G', '-accel', 'tcg,tb-size=384',
           '-object', 'memory-backend-file,id=ram,size=9G,mem-path=/ram.img,share=off,readonly=on,rom=off',
           '-machine', 'memory-backend=ram',
           '-device', f'loader,file={payload},addr=0xF0000000,force-raw=on',
           '-device', 'loader,file=/twin.elf,cpu-num=0',
           '-nic', 'none', '-display', 'none', '-serial', 'stdio', '-monitor', f'unix:{mon},server,nowait', '-no-reboot']
    if ring_img:
        cmd[cmd.index('-nic'):cmd.index('-nic')] = ['-device', f'loader,file={ring_img},addr={RING:#x},force-raw=on']
    if IOS_SECURE:
        secure_machine(cmd, work)
    if drive:
        cmd[cmd.index('-nic'):cmd.index('-nic')] = [
            '-global', 'virtio-mmio.force-legacy=false', '-device', 'virtio-tablet-device', '-device', 'virtio-keyboard-device',
            '-qmp', f'unix:{d}/qmp.sock,server,nowait']
        (work / 'current-run').write_text(str(d) + '\n' + str(out) + '\n')
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    start = time.monotonic()
    pending = sorted(shots)
    lines, partial, outcome, reached = [], b'', None, {}
    log = (out / 'serial.log').open('w')
    while outcome is None:
        now = time.monotonic() - start
        if pending and now >= pending[0]:
            at = pending.pop(0)
            try:
                hmp(mon, f'pmemsave {FB:#x} {FB_W * FB_H * 4:#x} "{d}/fb.bin"')
                png(out / f'screen-{at}s.png', (d / 'fb.bin').read_bytes(), FB_W, FB_H, 3)
            except OSError as e:
                lines.append(f'[runner] screenshot at {at}s failed: {e}')
        if now > secs + 150:
            outcome = 'time limit (the kernel neither returned nor reset)'
            break
        r, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not r:
            if proc.poll() is not None:
                outcome = f'QEMU exited ({proc.returncode})'
            continue
        data = os.read(proc.stdout.fileno(), 65536)
        if not data:
            outcome = 'QEMU closed its output'
            break
        partial += data
        *done, partial = partial.split(b'\n')
        for raw in done:
            line = raw.decode(errors='replace').rstrip('\r')
            lines.append(line)
            log.write(line + '\n')
            for name, tag in IOS_MILESTONES:
                if name not in reached and tag in line:
                    reached[name] = round(time.monotonic() - start, 1)
                    print(f'  [{reached[name]:7.1f} s] {name}: {line[:150]}', flush=True)
            if line.startswith('[TWIN] RESET'):
                outcome = 'reset via PS_HOLD (the loader rebooted after the kernel came back)'
            elif line.startswith('[TWIN] HANG') or line.startswith('[TWIN] PARKED'):
                outcome = outcome or 'stopped: EL2 or firmware stand-in parked (see serial log)'
        log.flush()
    log.close()
    try:
        hmp(mon, f'pmemsave {FB:#x} {FB_W * FB_H * 4:#x} "{d}/fb.bin"')
        png(out / 'screen-final.png', (d / 'fb.bin').read_bytes(), FB_W, FB_H, 3)
        hmp(mon, f'pmemsave {RING:#x} {RING_LEN:#x} "{d}/ring.bin"')
        hmp(mon, 'quit')
    except OSError as e:
        lines.append(f'[runner] monitor error: {e}')
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
    ring_text = ''
    if (d / 'ring.bin').exists():
        # this boot's part of the Samsung RAM log: what `last_kmsg` would show after the reset
        rb = (d / 'ring.bin').read_bytes()
        idx = struct.unpack_from('<I', rb, 8)[0]
        buf = rb[16:16 + RING_SIZE]
        begin = PHONE_SHIM_START if ring_img else 0
        end_pos = idx % RING_SIZE
        text = buf[begin:end_pos] if end_pos >= begin else buf[begin:] + buf[:end_pos]
        ring_text = text.decode(errors='replace')
        (out / 'ring.txt').write_text(ring_text)
    found_in_ring = {name: tag in ring_text for name, tag in IOS_MILESTONES}
    receipt = {'mode': 'ios', 'ring_bytes_this_boot': len(ring_text), 'milestones_in_ram_ring': found_in_ring,
               'engine': 'r5q virtual twin (QEMU virt EL2 + uH stand-in), production loader; EMULATION ONLY',
               'outcome': outcome, 'seconds': round(time.monotonic() - start, 1), 'milestones_s': reached,
               'loader': str(loader), 'kernel': str(kernel), 'cmdline_payload': cmdline,
               'twin_events': [l for l in lines if l.startswith('[TWIN]')][:80],
               'r5q_hardware_touched': [l for l in lines if l.startswith('[TWIN] MMIO')],
               'last_lines': lines[-25:]}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


if __name__ == '__main__':
    if sys.argv[1:2] == ['ios']:
        secs = int(sys.argv[2]) if len(sys.argv) > 2 else 600
        every = int(os.environ.get('TWIN_SHOT_EVERY', '0'))      # e.g. 10: a frame every 10 s (for a timeline)
        shots = tuple(range(every, secs + 1, every)) if every else (60, 180, 300, 420, 540, 660, 780)
        if os.environ.get('TWIN_INPUT') == '1':
            shots = ()                                          # twin_ctl.py takes them while driving
        r = run_ios(secs, tuple(s for s in shots if s < secs + 120), os.environ.get('TWIN_CMDLINE', ''))
        print(f"ios: {r['outcome']} | {r['seconds']} s | milestones {r['milestones_s']}", flush=True)
        sys.exit(0)
    for arg in sys.argv[1:]:
        r = run(int(arg))
        print(f"test {r['test']:2}: {r.get('outcome')} | markers {'OK' if r.get('markers_confirmed') else 'MISSING ' + str(r.get('markers_missing'))} | {r.get('seconds')} s | last: {r.get('last_ring_line')}", flush=True)
