"""Test 81 from Test 80 (the clock); was: Test 80 from Test 79: the data volume kept on the phone (var_store), and a live log that keeps its newest part.
kernel xnu-r5q-t80 (r5q USERDATA as darwin, writes past 7.8 GiB); root with /redwood/bin/var_store; loader: ufs=2
ufs_write=1 in live mode and the live log composed in the 80 MiB buffer; cmdline + ufs=2 ufs_write=1 var_store=1."""
import pathlib, shutil

R = pathlib.Path(__file__).resolve().parents[2]          # (the port: pipeline/gen/ -> work/ios-r5q)
A, B = 310, 311


def rd(p): return (R / p).read_bytes().decode('utf-8').replace(chr(13) + chr(10), chr(10))


def wr(p, s):
    assert chr(13) not in s
    (R / p).write_bytes(s.encode('utf-8'))


def ren(s):
    for x, y in [(f'test{A}', f'test{B}'), (f'Test {A}', f'Test {B}'), (f'TEST{A}', f'TEST{B}')]:
        s = s.replace(x, y)
    return s


if not (R / f'port/test{B}').exists():
    shutil.copytree(R / f'port/test{A}', R / f'port/test{B}')
for n in ['R5qUsbRamDisk.c', 'test_ramdisk_host.c']:
    shutil.copyfile(R / 'port/r5qfull' / n, R / f'port/test{B}' / n)
shutil.copyfile(R / f'port/build/r5q-test{A}-uefi.fd', R / f'port/build/r5q-test{B}-uefi.fd')
for a in ['build_test{}_shim.py', 'package_test{}.py', 'test_test{}_shim.py', 'run_test{}_checks.py', 'check_test{}.sh',
          'bundle_test{}.sh']:
    if (R / a.format(A)).exists():
        wr(a.format(B), ren(rd(a.format(A))).replace(f'bundle_native_test.py {A} ', f'bundle_native_test.py {B} ')
           .replace(f'--firmware-number {A}', f'--firmware-number {B}'))
shutil.copyfile(R / 'port/build/xnu-r5q-t311.macho', R / f'port/build/test{B}-xnu.macho')
shutil.copyfile(R / 'port/build/test275-root.apfs.new', R / f'port/build/test{B}-root.apfs')
x = R / 'port/xnuloader'
(x / f'build-test{B}.sh').write_bytes((x / f'build-test{A}.sh').read_bytes().replace(b'TEST310', b'TEST311'))
b = R / 'build_loader_linux.py'
t = b.read_text()
if "'--test311'" not in t:
    t = t.replace("('--test310',): 'build-test310.sh', ", "('--test310',): 'build-test310.sh', ('--test311',): 'build-test311.sh', ")
    b.write_text(t, newline='\n')
assert 'var_store=1' in rd(f'build_test{B}_shim.py')
print('generated test311: camera: bandwidth votes opt-in (off); unpaced 3 short + 150 s session soak')
