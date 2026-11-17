"""One-shot USB watcher: collect the unresolved test log and restore on Android return.

Never starts a test, reboots, flashes Download Mode, or writes another partition.
The existing guarded Restore operator must pass before the recovery write.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('bundle', type=pathlib.Path)
p.add_argument('--operator', required=True)
p.add_argument('--max-wait', type=int, default=43200)
args = p.parse_args()
if not 60 <= args.max_wait <= 43200:
    p.error('--max-wait must be between 60 and 43200 seconds')
root = pathlib.Path(__file__).resolve().parent
allowed = (root.parents[1] / 'outputs/S10-Lite-native-iOS-port').resolve(strict=True)
bundle = args.bundle.resolve(strict=True)
assert bundle.parent == allowed
operator = (bundle / args.operator).resolve(strict=True)
assert operator.parent == bundle and operator.suffix == '.ps1'
manifest_file = bundle / 'manifest.json'
manifest = json.loads(manifest_file.read_text())
serial = '<SERIAL>'
adb = shutil.which('adb')
pwsh = shutil.which('pwsh') or shutil.which('powershell')
assert adb and pwsh
out = bundle / 'validation'
receipt_file = out / 'restore-on-return-receipt.json'
if receipt_file.exists() and json.loads(receipt_file.read_text()).get('baseline_verified'):
    raise SystemExit('This one-shot restore already completed')
receipt = {'status': 'waiting for Android USB/root', 'original_recovery_restored': False,
           'max_wait_seconds': args.max_wait, 'serial': serial}

def save():
    receipt_file.write_text(json.dumps(receipt, indent=2)+'\n')

def command(argv, timeout=10):
    return subprocess.run(argv, capture_output=True, timeout=timeout)

save()
print('Waiting for the unresolved test phone to return; no firmware writes while offline.', flush=True)
deadline = time.monotonic() + args.max_wait
while time.monotonic() < deadline:
    try:
        state = command([adb, '-s', serial, 'get-state'])
        if state.returncode == 0 and state.stdout.strip() == b'device':
            probe = command([adb, '-s', serial, 'shell', "su -c 'id; getprop ro.product.model'"])
            if probe.returncode == 0 and b'uid=0(root)' in probe.stdout and b'SM-G770F' in probe.stdout:
                break
    except subprocess.TimeoutExpired:
        pass
    time.sleep(5)
else:
    receipt['status'] = 'watch expired; recovery restoration still needs physical access'
    save()
    raise SystemExit(2)
receipt['status'] = 'Android returned; collecting log and restoring'
save()
log = command([adb, '-s', serial, 'exec-out', "su -c 'cat /proc/last_kmsg'"], 25)
if log.returncode == 0 and 1000 <= len(log.stdout) <= 8*1024*1024:
    (out / 'late-return-last_kmsg.bin').write_bytes(log.stdout)
    receipt['log_sha256'] = hashlib.sha256(log.stdout).hexdigest()
else:
    receipt['log_error'] = 'No valid bounded last_kmsg captured; restoring regardless'
restore = command([pwsh, '-NoProfile', '-File', str(operator), '-Action', 'Restore'], 90)
(out / 'late-restore-transcript.txt').write_bytes(restore.stdout+restore.stderr)
if restore.returncode:
    receipt['status'] = 'guarded recovery restore failed; inspect transcript'
    save()
    raise SystemExit(3)
hashes = command([adb, '-s', serial, 'shell',
    "su -c 'sha256sum /dev/block/by-name/recovery /dev/block/by-name/boot /dev/block/by-name/vbmeta'"], 20)
actual = [line.split()[0] for line in hashes.stdout.decode(errors='replace').splitlines()]
expected = [manifest['original_recovery']['sha256'], manifest['original_boot_sha256'], manifest['original_vbmeta_sha256']]
if hashes.returncode or actual != expected:
    receipt['status'] = 'post-restore baseline verification failed'
    save()
    raise SystemExit(4)
(out / 'late-return-baseline.txt').write_bytes(hashes.stdout)
receipt.update(status='original recovery restored and baseline verified',
               original_recovery_restored=True, baseline_verified=True)
save()
manifest = json.loads(manifest_file.read_text())
manifest.update(original_recovery_restored=True, android_return='late/manual return; original test result needs log assessment',
                restore_on_return_receipt='validation/restore-on-return-receipt.json')
manifest_file.write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps(receipt, indent=2), flush=True)
