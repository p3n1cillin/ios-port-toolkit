"""Make the generator's templates from a real, hardware-tested test (r5q Test 68 by default).

The templates in templates/qcom-userland/ ARE Test 68's files with every device value and every
test value replaced by an @@PLACEHOLDER@@. Each replacement states how many times it must occur,
so a change in the source files fails loudly instead of leaving a device value behind.

    python templatize.py <work/ios-r5q> [--number 68]

After running it, `gen_test.py --device r5q --spec tests/r5q/test68.json` must reproduce the
original files byte for byte (tools/selftest.py checks that).
"""
import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / 'templates' / 'qcom-userland'


def sub(text, pairs, name):
    for old, new, count in pairs:
        n = text.count(old)
        if n != count:
            raise SystemExit(f'{name}: expected {count} x {old!r}, found {n}')
        text = text.replace(old, new)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work', type=pathlib.Path)
    ap.add_argument('--number', type=int, default=68)
    ap.add_argument('--device', type=pathlib.Path, default=HERE.parent / 'devices' / 'r5q.json',
                    help='device profile of the source test (its .local.json gives the paths to take out)')
    a = ap.parse_args()
    sys.path.insert(0, str(HERE.parent / 'tools'))
    import profile_spec
    local = profile_spec.load(a.device).get('local', {})
    global BACKUPS, WSL
    BACKUPS, WSL = local['backups_dir'], local['work_wsl']
    N = a.number
    W = a.work
    rd = lambda p: (W / p).read_bytes().decode('utf-8')
    OUT.mkdir(parents=True, exist_ok=True)
    common = [(f'r5q-test{N}-', '@@PREFIX@@-test@@N@@-', None)]

    def cnt(text, s):
        return text.count(s)

    # ---- build_testN_shim.py ----
    t = rd(f'build_test{N}_shim.py')
    lines = t.split('\n')
    i0 = next(i for i, l in enumerate(lines) if l.startswith("body = item('loader.efi'"))
    i1 = next(i for i, l in enumerate(lines) if l.startswith('payload = struct.pack'))
    block = '\n'.join(lines[i0:i1])
    t = t.replace(block, '@@PAYLOAD_ITEMS@@')
    t = sub(t, [(f'r5q-test{N}-', '@@PREFIX@@-test@@N@@-', cnt(t, f'r5q-test{N}-')),
                (f"port / 'test{N}'", "port / 'test@@N@@'", 1),
                (f'Test {N}', 'Test @@N@@', 2),
                ('assert len(payload) < 0x03F00000', 'assert len(payload) < @@PAYLOAD_LIMIT@@', 1),
                ('-DFD_BASE=0x9FC00000 -DFD_SIZE=0x300000 -DPAYLOAD_BASE=0xF0000000',
                 '-DFD_BASE=@@FD_BASE@@ -DFD_SIZE=@@FD_SIZE@@ -DPAYLOAD_BASE=@@PAYLOAD_BASE@@', 1),
                ('-DPAYLOAD_MAX=0x04000000', '-DPAYLOAD_MAX=@@PAYLOAD_MAX@@', 1),
                ('len(shim) + 0x300000 + len(payload)', 'len(shim) + @@FD_SIZE@@ + len(payload)', 1),
                ('assert len(fd) == 0x300000', 'assert len(fd) == @@FD_SIZE@@', 1)], 'build_shim')
    (OUT / 'build_test@@N@@_shim.py.tmpl').write_bytes(t.encode())

    # ---- package_testN.py ----
    t = rd(f'package_test{N}.py')
    t = sub(t, [(f'r5q-test{N}-', '@@PREFIX@@-test@@N@@-', cnt(t, f'r5q-test{N}-')),
                (f'Test {N}', 'Test @@N@@', 2),
                ('validate_window(root, 0xe0000000, 0x10000000)', 'validate_window(root, @@WINDOW_CHECK@@)', 1),
                (f"root / '{BACKUPS}/recovery.img'", "root / '@@BACKUPS_DIR@@/recovery.img'", 1),
                ("'332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd'", "'@@RECOVERY_SHA256@@'", 1),
                ('assert page == 4096', 'assert page == @@PAGE_SIZE@@', 1),
                ('SLOT, SLOT_BYTES = 0x4000000, 513 * 4096', 'SLOT, SLOT_BYTES = @@SLOT_OFFSET@@, @@SLOT_BLOCKS@@ * 4096', 1),
                ("b'R5QLOGSLOT/V1' + bytes(3)", "b'@@SLOT_MAGIC@@' + bytes(@@SLOT_PAD@@)", 1)], 'package')
    (OUT / 'package_test@@N@@.py.tmpl').write_bytes(t.encode())

    # ---- test_testN_shim.py ----
    t = rd(f'test_test{N}_shim.py')
    t = sub(t, [(f'r5q-test{N}-', '@@PREFIX@@-test@@N@@-', cnt(t, f'r5q-test{N}-')),
                ('FD,PAY,SOURCE,MDSS,INFO,LOG=0x9fc00000,0xf0000000,0x80080000,0x0ae00000,0x9e300000,0xc1600000',
                 'FD,PAY,SOURCE,MDSS,INFO,LOG=@@fd_base@@,@@payload_base@@,@@kernel_load_pa@@,@@mdss_base@@,'
                 '@@bootshim_info@@,@@log_ring@@', 1),
                ('CAP=0x200000-16', 'CAP=@@ring_size@@-16', 1),
                ("struct.pack_into('<I',content,4,0x4000000)", "struct.pack_into('<I',content,4,@@payload_max_short@@)", 1),
                ("struct.pack_into('<I',content,4,0x3e00000)", "struct.pack_into('<I',content,4,@@payload_outside@@)", 1),
                ('(PAY,0x4000000)', '(PAY,@@payload_max_short@@)', 1),
                ('(LOG,0x201000)', '(LOG,@@ring_map@@)', 1),
                ("else 0x4d474f4c", "else @@ring_magic@@", 1),
                ('LOG+0x200000', 'LOG+@@ring_size@@', 3),
                ('LOG+0x200fff', 'LOG+@@ring_last@@', 1),
                ('uc.mem_map(0x9c400000,0xa00000)', 'uc.mem_map(@@fb_base@@,@@frame_map@@)', 1),
                ('[(0x25014,0x9c400000),(0x25024,4320),(0x25000,2400<<16|1080),(0x20f4,2)]',
                 '[(0x25014,@@fb_base@@),(0x25024,@@STRIDE@@),(0x25000,@@HEIGHT@@<<16|@@WIDTH@@),(0x20f4,2)]', 1),
                ("[('invalidate',FD,0x300000)]", "[('invalidate',FD,@@fd_size@@)]", 1),
                ("[('clean',FD,0x300000)", "[('clean',FD,@@fd_size@@)", 1)], 'test_shim')
    (OUT / 'test_test@@N@@_shim.py.tmpl').write_bytes(t.encode())

    # ---- run_testN_checks.py, check_testN.sh ----
    t = rd(f'run_test{N}_checks.py')
    t = sub(t, [(f'test_test{N}_shim.py', 'test_test@@N@@_shim.py', 1)], 'run_checks')
    (OUT / 'run_test@@N@@_checks.py.tmpl').write_bytes(t.encode())
    t = rd(f'check_test{N}.sh')
    t = sub(t, [(WSL, '@@WORK_WSL@@', 1),
                (f'r5q-test{N}-loader.efi --test{N}', '@@PREFIX@@-test@@N@@-loader.efi --test@@N@@', 1),
                (f'build_test{N}_shim.py', 'build_test@@N@@_shim.py', 1),
                (f'run_test{N}_checks.py', 'run_test@@N@@_checks.py', 1)], 'check')
    (OUT / 'check_test@@N@@.sh.tmpl').write_bytes(t.encode())

    # ---- bundle_testN.sh ----
    t = rd(f'bundle_test{N}.sh')
    notes = re.search(r'--notes "(.*?)" \\\n', t, re.S).group(1)
    blocked = re.search(r'--start-blocked-reason "(.*?)" \|', t, re.S).group(1)
    t = sub(t, [(WSL, '@@WORK_WSL@@', 1),
                (f'package_test{N}.py', 'package_test@@N@@.py', 1),
                (f'bundle_native_test.py {N} --firmware-number {N} --mode diagnostic',
                 'bundle_native_test.py @@N@@ --firmware-number @@FIRMWARE_NUMBER@@ --mode @@MODE@@', 1),
                (notes, '@@NOTES@@', 1), (blocked, '@@START_BLOCKED_REASON@@', 1)], 'bundle')
    (OUT / 'bundle_test@@N@@.sh.tmpl').write_bytes(t.encode())

    # ---- port/xnuloader/build-testN.sh ----
    t = rd(f'port/xnuloader/build-test{N}.sh')
    lines = t.split('\n')
    comment_end = next(i for i, l in enumerate(lines) if l == 'set -eu')
    clang = next(l for l in lines if l.startswith('clang '))
    flags_start = clang.index(' -D') + 1
    flags_end = clang.index(' -ffreestanding')
    lines = ['#!/bin/sh', '@@LOADER_COMMENT@@'] + lines[comment_end:]
    t = '\n'.join(lines)
    t = t.replace(clang[flags_start:flags_end], '@@LOADER_FLAGS@@')
    t = sub(t, [('/base:0x99000000', '/base:@@loader_pe_base@@', 1)], 'loader')
    (OUT / 'build-test@@N@@.sh.tmpl').write_bytes(t.encode())

    for f in sorted(OUT.iterdir()):
        left = re.findall(rf'(?<![0-9a-fx]){N}(?![0-9])', f.read_text())
        if left:
            print(f'warning: {f.name} still contains {N} {len(left)} time(s)', file=sys.stderr)
        print(f'{f.name}: {f.stat().st_size} bytes')


if __name__ == '__main__':
    main()
