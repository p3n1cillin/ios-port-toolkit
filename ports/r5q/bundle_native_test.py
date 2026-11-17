"""Collect a checked native diagnostic into a self-contained, guarded test bundle."""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct

p = argparse.ArgumentParser()
p.add_argument('number', type=int)
p.add_argument('--firmware-number', required=True, type=int)
p.add_argument('--mode', choices=['diagnostic', 'prepare', 'checkpoint1', 'checkpoint5', 'timed-run', 'return-selftest'], required=True)
p.add_argument('--notes', required=True)
p.add_argument('--start-blocked-reason', default='')
args = p.parse_args()
root = pathlib.Path(__file__).resolve().parent
project = root.parents[1]
build = root / 'port/build'
base = project / 'outputs/S10-Lite-native-iOS-port'
out = base / f'test{args.number}'
if (out / 'manifest.json').exists():
    raise SystemExit('Refusing to overwrite a historical bundle; choose a new test number')
prefix = f'r5q-test{args.number}'
image = build / (prefix + '-recovery.img')
report = json.loads((build / (prefix + '-image-report.json')).read_text())

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

assert report['sha256'] == digest(image) and report['bytes'] == image.stat().st_size
assert report['stock_recovery_sha256'] == '332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd'
assert digest(build / (prefix + '-uefi.fd')) == digest(build / f'r5q-test{args.firmware_number}-uefi.fd')
efi = (build / (prefix + '-loader.efi')).read_bytes()
pe = struct.unpack_from('<I', efi, 60)[0]
loader_memory_bytes = struct.unpack_from('<I', efi, pe + 24 + 56)[0]
if args.number >= 15 and args.mode in ['prepare', 'checkpoint1', 'checkpoint5', 'timed-run', 'return-selftest']:
    assert loader_memory_bytes <= 65536, 'Early loader exceeds measured working heap footprint'
for label in ['shim'] + (['prepare'] if args.mode == 'prepare' else ['roundtrip'] if args.mode in ['checkpoint1', 'checkpoint5', 'timed-run', 'return-selftest'] else []):
    test = json.loads((build / f'test_test{args.number}_{label}.json').read_text())
    assert test.get('result', 'passed') == 'passed'
    assert all(c['result'] == 'passed' for c in test.get('cases', test.get('tests', [])))
    if args.number >= 16:
        inputs = test.get('input_sha256', {})
        assert inputs, 'Test report lacks input hashes'
        for name, expected in inputs.items():
            assert pathlib.Path(name).name == name and name.startswith(prefix + '-')
            assert digest(build / name) == expected, 'Test evidence does not match current inputs'
for name in ['source', 'validation', 'restore']:
    (out / name).mkdir(parents=True, exist_ok=True)
baseline = json.loads((base / 'test9/manifest.json').read_text())
for name in ['restore/recovery-original.img', 'restore/recovery-original.tar']:
    source = base / 'test9' / name
    if source.exists():
        shutil.copyfile(source, out / name)
helper = build / 'reboot-recovery'
assert digest(helper) == baseline['reboot_helper']['sha256']
shutil.copyfile(helper, out / 'reboot-recovery')
assert digest(out / 'restore/recovery-original.img') == baseline['original_recovery']['sha256']
shutil.copyfile(image, out / image.name)
manifest = {
    'image': {'name': image.name, 'bytes': image.stat().st_size, 'sha256': digest(image)},
    'original_recovery': baseline['original_recovery'],
    'reboot_helper': baseline['reboot_helper'],
    'original_boot_sha256': baseline['original_boot_sha256'],
    'original_vbmeta_sha256': baseline['original_vbmeta_sha256'],
    'status': 'checks passed; awaiting hardware run', 'flashed': False,
    'original_recovery_restored': None, 'hardware_efi_loader_entry': 'unproven',
    'mode': args.mode, 'executes_xnu': args.mode in ['checkpoint1', 'checkpoint5', 'timed-run'],
    'firmware_source_test': args.firmware_number,
    'firmware_sha256': digest(build / (prefix + '-uefi.fd')),
    'loader_sha256': digest(build / (prefix + '-loader.efi')),
    'loader_memory_bytes': loader_memory_bytes,
    'notes': args.notes,
    'start_blocked_reason': args.start_blocked_reason,
}
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
operator = (base / 'test9/Manage-Test9.ps1').read_text()
operator = operator.replace('test9', f'test{args.number}').replace('Test 9', f'Test {args.number}')
marker = (f'TEST{args.number} KERNEL PREPARATION VERIFIED' if args.mode == 'prepare' else
          f'TEST{args.number} REAL XNU CHECKPOINT 1 VERIFIED' if args.mode == 'checkpoint1' else
          f'TEST{args.number} REAL XNU CHECKPOINT 5 VERIFIED' if args.mode == 'checkpoint5' else
          f'TEST{args.number} REAL XNU TIMED RUN RETURNED' if args.mode == 'timed-run' else
          f'TEST{args.number} RETURN PATH SELF TEST VERIFIED' if args.mode == 'return-selftest' else 'TEST1 HANDOFF OK')
anchor = '$adbCommand = (Get-Command adb -ErrorAction Stop).Source'
operator = operator.replace(anchor,
    "if ($Action -eq 'Start' -and $manifest.start_blocked_reason) { throw $manifest.start_blocked_reason }\n" + anchor, 1)
operator = operator.replace('R5Q UEFI TEST 5 and TEST1 HANDOFF OK',
    f'R5Q UEFI TEST {args.firmware_number} and {marker}')
(out / f'Manage-Test{args.number}.ps1').write_text(operator)
tools = ['bundle_native_test.py', 'run_native_test.py', 'run_binary_checks.py', 'validate_memory_window.py',
         'build_loader_linux.py', 'build_uefi_linux.py',
         f'build_test{args.number}_shim.py', f'package_test{args.number}.py',
         f'test_test{args.number}_shim.py', f'test_test{args.number}_prepare.py',
         f'test_test{args.number}_roundtrip.py', f'run_test{args.number}_roundtrip_checks.py',
         f'run_test{args.number}_checks.py', f'prepare_test{args.firmware_number}.py']
for name in tools:
    if (root / name).exists():
        shutil.copyfile(root / name, out / 'source' / name)
for folder in ['xnuloader', f'test{args.firmware_number}', f'test{args.number}']:
    target = out / 'source' / folder
    target.mkdir(exist_ok=True)
    for source in (root / 'port' / folder).iterdir():
        if source.is_file() and source.suffix in ['.c', '.h', '.S', '.inf', '.sh']:
            shutil.copyfile(source, target / source.name)
for suffix in ['-uefi.fd', '-loader.efi', '-shim.bin', '-image-report.json', '-image-validation.txt']:
    shutil.copyfile(build / (prefix + suffix), out / 'validation' / (prefix + suffix))
symbols = build / (prefix + '-shim-symbols.txt')
if symbols.exists():
    shutil.copyfile(symbols, out / 'validation' / symbols.name)
for label in ['shim', 'prepare', 'roundtrip']:
    source = build / f'test_test{args.number}_{label}.json'
    if source.exists():
        shutil.copyfile(source, out / 'validation' / source.name)
readme = f'''# Test {args.number}: {args.mode}

{args.notes}

This uses the exact firmware from Test {args.firmware_number}. Its dashboard and firmware
markers retain that test number. The loader marker is {marker}.
The checked image hash is {manifest['image']['sha256']}.

Start temporarily writes only recovery on <SERIAL>. The single-test controller
captures a new Android boot's raw log, restores original recovery on return, and
verifies recovery, Android boot and VBMeta hashes. Missing markers are recorded
as unproven. The original restore image and guarded operator are included.

No iOS userland is installed. Emulator results establish instruction paths with
emulated services, not real hardware behavior. See the automatic-run receipt for
the actual phone result and restoration state.
'''
(out / 'README.md').write_text(readme)
print(json.dumps({'bundle': str(out), 'image': manifest['image'], 'mode': args.mode}, indent=2))
