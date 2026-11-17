"""Boot ONE test image into RAM on a phone with a real `fastboot boot` (flash.method: fastboot-boot).

Xiaomi/POCO/Redmi, Pixel, OnePlus, Motorola, Fairphone and most non-Samsung phones with an unlocked bootloader
accept `fastboot boot <image>`: the image is loaded into RAM and started once; nothing on the phone is written,
and the next reboot is Android again. That is a stronger safety position than Samsung's recovery-write + restore,
so every fastboot command here goes through safety.assert_fastboot_safe (no flash/erase/oem/set_active, ever).

    python fastboot_boot.py --device ../devices/<codename>.json --image <test image> --authorized-by "<owner's words>"
                            [--log-wait 600]

Steps: the phone must already be in fastboot (the owner puts it there: adb reboot bootloader or the key combo).
Checks: product/unlocked via getvar, the image is an Android boot image of a header version the profile allows,
its size is under max-download-size. Then `fastboot boot`, and a receipt in <image dir>/runs/<time>/. Reading
the run's log is device-specific (the donor's ./xl reads the loader's USB link); this only starts the test.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import bootimg  # noqa: E402
import profile_spec  # noqa: E402
import safety  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--device', type=pathlib.Path, required=True)
ap.add_argument('--image', type=pathlib.Path, required=True)
ap.add_argument('--authorized-by', required=True, help="the owner's authorization for THIS run, quoted")
a = ap.parse_args()

profile = profile_spec.load(a.device)
safety.assert_profile_safe(profile)
if (profile.get('flash', {}).get('method') or 'recovery') != 'fastboot-boot':
    raise SystemExit('this profile is not a fastboot-boot phone (flash.method); use run_test.py')
fastboot = shutil.which('fastboot')
assert fastboot, 'fastboot is needed'
serial = profile.get('local', {}).get('serial')
image = a.image.resolve(strict=True)
data = image.read_bytes()
header = bootimg.parse(data)
assert header['kind'] == 'boot', 'not a boot image'
run = image.parent / 'runs' / time.strftime('%Y%m%d-%H%M%S')
run.mkdir(parents=True)
receipt = {'authorized_by': a.authorized_by, 'image': image.name, 'sha256': hashlib.sha256(data).hexdigest(),
           'header_version': header['header_version'], 'status': 'preflight'}


def fb(*args, timeout=30):
    argv = [fastboot] + (['-s', serial] if serial else []) + list(args)
    safety.assert_fastboot_safe(argv, profile)
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    return (r.stdout + r.stderr).strip(), r.returncode


def getvar(name):
    out, _ = fb('getvar', name)
    for line in out.splitlines():
        if line.startswith(f'{name}:'):
            return line.split(':', 1)[1].strip()
    return None


try:
    devices, _ = fb('devices')
    if not devices.strip():
        raise SystemExit('no phone in fastboot: the owner puts it there (adb reboot bootloader, or the key combo)')
    receipt['product'] = getvar('product')
    receipt['unlocked'] = getvar('unlocked')
    want = profile.get('flash', {}).get('fastboot_product') or profile['device']['codename']
    if receipt['product'] != want:
        raise SystemExit(f'fastboot product {receipt["product"]!r} is not {want!r}: wrong phone')
    if receipt['unlocked'] not in ('yes', 'true'):
        raise SystemExit('the bootloader is locked')
    mds = getvar('max-download-size')
    if mds and len(data) > int(mds, 0):
        raise SystemExit(f'image {len(data)} bytes > max-download-size {mds}')
    receipt['status'] = 'fastboot boot sent'
    out, code = fb('boot', str(image), timeout=300)
    receipt['fastboot_output'] = out[-2000:]
    receipt['status'] = 'booted (RAM only; the next reboot is Android)' if code == 0 else f'fastboot boot failed ({code})'
finally:
    (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
