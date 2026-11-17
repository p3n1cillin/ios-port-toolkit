"""Check bootimg.py against AOSP mkbootimg.py (every header version) and, optionally, a real stock image.

    python test_bootimg.py --mkbootimg <path to AOSP mkbootimg.py> [--stock recovery.img --test-unsigned <img> --test-kernel <bin>]

For header versions 0-4: build an image with mkbootimg from random components, replace its kernel with
bootimg.repack(), build the same image with the new kernel with mkbootimg, and require identical bytes.
vendor_boot v3/v4: build with mkbootimg and require bootimg.parse() to find every component.
With --stock: repacking the stock image with its own kernel must reproduce it up to the end of its
components; with --test-kernel/--test-unsigned (r5q: package_test68.py's unsigned image), repacking with
the test kernel must reproduce that image up to its SEANDROIDENFORCE trailer.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bootimg  # noqa: E402


def run(mk, args):
    r = subprocess.run([sys.executable, '-I', str(mk)] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-500:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mkbootimg', type=pathlib.Path, required=True)
    ap.add_argument('--stock', type=pathlib.Path)
    ap.add_argument('--test-kernel', type=pathlib.Path)
    ap.add_argument('--test-unsigned', type=pathlib.Path)
    a = ap.parse_args()
    fails = 0
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        files = {}
        for name, size in [('k1', 1234567), ('k2', 2345679), ('rd', 456789), ('dtb', 12345), ('second', 3333),
                           ('dtbo', 54321), ('vrd', 222222)]:
            files[name] = t / name
            files[name].write_bytes(os.urandom(size))
        for v in range(5):
            common = ['--header_version', v, '--ramdisk', files['rd'], '--cmdline', 'console=ttyMSM0 test=1',
                      '--os_version', '13.0.0', '--os_patch_level', '2024-01']
            if v <= 2:
                common += ['--pagesize', 4096, '--second', files['second'], '--board', 'toolkit']
            if v == 1 or v == 2:
                common += ['--recovery_dtbo', files['dtbo']]
            if v == 2:
                common += ['--dtb', files['dtb']]
            one, two = t / f'v{v}-k1.img', t / f'v{v}-k2.img'
            run(a.mkbootimg, common + ['--kernel', files['k1'], '-o', one])
            run(a.mkbootimg, common + ['--kernel', files['k2'], '-o', two])
            got = bootimg.repack(one.read_bytes(), files['k2'].read_bytes())
            want = two.read_bytes()
            ok = got == want[:len(got)] and not any(want[len(got):])
            h = bootimg.parse(one.read_bytes())
            names = [c['name'] for c in h['components'] if c['size']]
            print(f'{"PASS" if ok else "FAIL"}  boot v{v}: repack == mkbootimg ({len(got)} bytes; components {names})')
            fails += not ok
        for v in (3, 4):
            vb = t / f'vendor{v}.img'
            args = ['--header_version', v, '--vendor_boot', vb, '--vendor_ramdisk', files['vrd'], '--dtb', files['dtb'],
                    '--vendor_cmdline', 'androidboot.hardware=qcom', '--pagesize', 4096, '--board', 'toolkit']
            run(a.mkbootimg, args)
            data = vb.read_bytes()
            h = bootimg.parse(data)
            ok = (bootimg.component(data, h, 'dtb') == files['dtb'].read_bytes()
                  and files['vrd'].read_bytes() in bootimg.component(data, h, 'vendor_ramdisk'))
            print(f'{"PASS" if ok else "FAIL"}  vendor_boot v{v}: parse finds the vendor ramdisk and DTB')
            fails += not ok
    if a.stock:
        stock = a.stock.read_bytes()
        h = bootimg.parse(stock)
        same = bootimg.repack(stock, bootimg.component(stock, h, 'kernel'))
        ok = same == stock[:len(same)]
        print(f'{"PASS" if ok else "FAIL"}  stock {a.stock.name} (v{h["header_version"]}, AVB footer '
              f'{"yes" if h["avb_footer"] else "no"}): repack with its own kernel reproduces it')
        fails += not ok
        if a.test_kernel and a.test_unsigned:
            got = bootimg.repack(stock, a.test_kernel.read_bytes())
            want = a.test_unsigned.read_bytes()
            ok = want == got + b'SEANDROIDENFORCE'
            print(f'{"PASS" if ok else "FAIL"}  {a.test_unsigned.name}: repack with the test kernel + Samsung trailer '
                  f'== package_test output')
            fails += not ok
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
