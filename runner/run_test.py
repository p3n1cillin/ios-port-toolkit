"""Run ONE authorized hardware test of a bundle, capture its log, and restore recovery on return.

The device-independent form of work/ios-r5q/run_native_test.py + run_stage_test_auto.ps1:

    python run_test.py <bundle> --device ../devices/<codename>.json --test <generated runner/testN.json>
                       --authorized-by "<the owner's words, quoted>"

Before anything is written it runs the safety checks (safety.py: R1-R7) on the profile and the bundle,
refuses if the bundle's start guard is set, and records the authorization in the run receipt. It then:
  1. reads the previous boot's last_kmsg (marker counts before), starts the PC side of the USB RAM drive
     when the test takes the system image over USB (usb_files in testN.json)
  2. Start: the bundle's guarded operator writes the test image to recovery and enters it (reboot-recovery)
  3. waits for Android with a new boot id and root (timeout from testN.json)
  4. reads last_kmsg and the firmware's log slot from the test image in recovery (before Restore)
  5. Restore: the operator writes the original recovery back; recovery/boot/vbmeta are hash-verified
  6. markers: in order, with this test's anchor marker newer than before
Exit: 0 confirmed, 2 Android did not return (physical access: Download Mode, RECOVERY only), 3 baseline not
verified, 4 markers unproven. It never retries, never chains a second test, and never writes another partition.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import profile_spec  # noqa: E402
import safety  # noqa: E402

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('bundle', type=pathlib.Path)
p.add_argument('--device', type=pathlib.Path, required=True)
p.add_argument('--test', type=pathlib.Path, required=True, help='runner/testN.json from gen_test.py')
p.add_argument('--authorized-by', required=True, help="the owner's authorization for THIS run, quoted")
p.add_argument('--usb-session', type=pathlib.Path, help='PC side of the USB RAM drive (usb_msd_session.py)')
args = p.parse_args()

profile = profile_spec.load(args.device)
test = json.loads(args.test.read_text())
bundle = args.bundle.resolve(strict=True)
passed = safety.check_bundle(bundle, profile)
manifest_path = bundle / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
if manifest.get('start_blocked_reason'):
    raise SystemExit('The bundle is start-guarded: lift start_blocked_reason for this one run, recording the '
                     'authorization, then run again. ' + manifest['start_blocked_reason'])
serial = profile.get('local', {}).get('serial')
assert serial, 'the local overlay must give local.serial'
operator = bundle / f'Manage-Test{test["number"]}.ps1'
assert operator.exists()
adb = shutil.which('adb')
pwsh = shutil.which('pwsh') or shutil.which('powershell')
assert adb and pwsh, 'adb and PowerShell are needed'
expect, anchor = test['markers'], test['anchor']
run = bundle / 'validation' / time.strftime('automatic-%Y%m%d-%H%M%S')
run.mkdir(parents=True, exist_ok=False)
receipt = {'status': 'preflight', 'authorized_by': args.authorized_by, 'safety_checks': passed,
           'original_recovery_restored': True, 'expected_markers': expect, 'timeout_seconds': test['timeout']}


def record():
    (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    manifest['status'] = receipt['status']
    manifest['original_recovery_restored'] = receipt['original_recovery_restored']
    manifest['latest_automatic_run'] = str(run.relative_to(bundle))
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')


def command(argv, timeout=15):
    return subprocess.run(argv, capture_output=True, timeout=timeout)


def shell(text):
    safety.assert_shell_safe(text, profile)
    r = command([adb, '-s', serial, 'shell', text])
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors='replace')[:500])
    return r.stdout.decode(errors='replace').strip()


def getlog():
    r = command([adb, '-s', serial, 'exec-out', "su -c 'cat /proc/last_kmsg'"], 20)
    if r.returncode or not 1000 <= len(r.stdout) <= 8 * 1024 * 1024:
        raise RuntimeError('Could not capture bounded last_kmsg')
    return r.stdout


def getslot(offset):
    magic = profile['log_slot']['magic'].encode()
    magic += bytes(16 - len(magic))
    ring_bytes = (profile['log_slot']['blocks'] - 1) * 4096
    blocks = 1 + ring_bytes // 4096
    cmd = f"su -c 'dd if=/dev/block/by-name/recovery bs=4096 skip={offset // 4096} count={blocks} 2>/dev/null'"
    safety.assert_shell_safe(cmd, profile)
    r = command([adb, '-s', serial, 'exec-out', cmd], 60)
    raw = r.stdout
    (run / 'log-slot.bin').write_bytes(raw)
    info = {'bytes_read': len(raw), 'magic': raw[:16] == magic, 'state': raw[32:40].rstrip(b'\0').decode(errors='replace')}
    receipt['log_slot'] = info
    if r.returncode or len(raw) != blocks * 4096 or not info['magic'] or info['state'] != 'SAVED':
        return b''
    ring = raw[4096:]
    widx = struct.unpack_from('<I', ring, 8)[0]
    data, cap = ring[16:], ring_bytes - 16
    text = data[:widx] if widx <= cap else data[widx % cap:] + data[:widx % cap]
    (run / 'firmware-log.txt').write_bytes(text)
    return text


def operate(action):
    r = command([pwsh, '-NoProfile', '-File', str(operator), '-Action', action], 90)
    (run / (action.lower() + '.txt')).write_bytes(r.stdout + r.stderr)
    if r.returncode:
        raise RuntimeError(f'{action} failed; see its saved transcript')


host = None
before_boot = shell('cat /proc/sys/kernel/random/boot_id')
before = getlog()
receipt['before_marker_counts'] = {m: before.count(m.encode()) for m in expect}
record()
android_returned = False
try:
    if test.get('usb_files') and args.usb_session:
        usb_out = run / 'usb-host'
        argv = [sys.executable, '-I', str(args.usb_session), str(usb_out), '--wait', '600', '--test-mib', '0']
        for f in test['usb_files']:
            argv += ['--file', f]
        host = subprocess.Popen(argv, stdout=open(run / 'usb-host.stdout.txt', 'wb'),
                                stderr=open(run / 'usb-host.stderr.txt', 'wb'))
    receipt.update(status='test launched; awaiting Android', original_recovery_restored=False)
    manifest['flashed'] = True
    record()
    operate('Start')
    deadline = time.monotonic() + test['timeout']
    while time.monotonic() < deadline:
        st = command([adb, '-s', serial, 'get-state'], 5)
        if st.returncode == 0 and st.stdout.strip() == b'device':
            try:
                boot = shell('cat /proc/sys/kernel/random/boot_id')
                if boot and boot != before_boot and 'uid=0' in shell("su -c 'id'"):
                    android_returned = True
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
        time.sleep(3)
    if not android_returned:
        receipt['status'] = ('needs physical access: Android did not return before timeout. Download Mode: flash '
                             'ONLY the original recovery (restore/recovery-original.img), nothing else')
        record()
        raise SystemExit(2)
    log = getlog()
    (run / 'last_kmsg.bin').write_bytes(log)
    if test.get('log_slot'):
        log = log + b'\n== FIRMWARE LOG SLOT ==\n' + getslot(int(test['log_slot'], 0))
    receipt['after_marker_counts'] = {m: log.count(m.encode()) for m in expect}
    pos = log.rfind(expect[0].encode())
    ordered = pos >= 0
    for m in expect[1:]:
        pos = log.find(m.encode(), pos + 1) if ordered else -1
        ordered = ordered and pos >= 0
    receipt['ordered_marker_sequence'] = ordered
    receipt['anchor'] = anchor
    receipt['markers_confirmed'] = ordered and receipt['after_marker_counts'][anchor] > receipt['before_marker_counts'][anchor]
    receipt['status'] = ('hardware markers confirmed' if receipt['markers_confirmed']
                         else 'automatic Android return; required native markers unproven')
except SystemExit:
    raise
except Exception as exc:
    receipt['status'] = 'error: ' + str(exc)
    raise
finally:
    st = command([adb, '-s', serial, 'get-state'], 5)
    if st.returncode == 0 and st.stdout.strip() == b'device':
        try:
            operate('Restore')
            hashes = shell("su -c 'sha256sum /dev/block/by-name/recovery /dev/block/by-name/boot /dev/block/by-name/vbmeta'")
            want = [profile['baseline']['recovery_sha256'], profile['baseline']['boot_sha256'],
                    profile['baseline']['vbmeta_sha256']]
            if [line.split()[0] for line in hashes.splitlines()] != want:
                raise RuntimeError('Post-restore partition baseline mismatch')
            (run / 'baseline.txt').write_text(hashes + '\n')
            receipt['original_recovery_restored'] = True
            receipt['baseline_verified'] = True
        except Exception as exc:
            receipt['restore_error'] = str(exc)
    if host is not None:
        try:
            host.wait(30)
        except subprocess.TimeoutExpired:
            host.kill()
    record()
    print(json.dumps(receipt, indent=2), flush=True)

if not receipt.get('baseline_verified'):
    raise SystemExit(3)
if not receipt.get('markers_confirmed'):
    raise SystemExit(4)
