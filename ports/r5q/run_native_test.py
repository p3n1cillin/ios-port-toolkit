"""Run one authorized recovery diagnostic, capture its log, and restore on return.

No retries or test chaining. A timeout leaves an explicit needs-physical-access
receipt and cannot launch another firmware write. Each operator retains its
device, battery, root, bootloader and partition-hash guards.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('bundle', type=pathlib.Path)
p.add_argument('--operator', required=True)
p.add_argument('--expect', action='append', default=[])
# Test 51+: a 2.4 GB USB copy plus a 240 s kernel run needs up to 20 minutes before Android returns
p.add_argument('--timeout', type=int, default=180, choices=range(60, 1801))
# the marker whose count must rise to prove this run's boot logged it (default: the first --expect);
# use a test-specific one, since shared markers may already be in the previous boot's log (Test 32)
p.add_argument('--anchor')
# Test 44+: byte offset of the firmware's log slot in the test recovery image (read back before Restore)
p.add_argument('--log-slot', type=lambda s: int(s, 0))
args = p.parse_args()
anchor = args.anchor or (args.expect[0] if args.expect else None)
assert anchor is None or anchor in args.expect, '--anchor must be one of the --expect markers'
bundle = args.bundle.resolve(strict=True)
operator = (bundle / args.operator).resolve(strict=True)
assert operator.parent == bundle and operator.suffix == '.ps1'
serial = '<SERIAL>'
adb = shutil.which('adb')
pwsh = shutil.which('pwsh') or shutil.which('powershell')
assert adb and pwsh
manifest_path = bundle / 'manifest.json'
manifest = json.loads(manifest_path.read_text())
run = bundle / 'validation' / time.strftime('automatic-%Y%m%d-%H%M%S')
run.mkdir(parents=True, exist_ok=False)
receipt = {'status': 'preflight', 'serial': serial, 'original_recovery_restored': True,
           'expected_markers': args.expect, 'timeout_seconds': args.timeout}

def record():
    (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    manifest['status'] = receipt['status']
    manifest['original_recovery_restored'] = receipt['original_recovery_restored']
    manifest['latest_automatic_run'] = str(run.relative_to(bundle))
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')

def command(argv, timeout=15):
    return subprocess.run(argv, capture_output=True, timeout=timeout)

def shell(text):
    result = command([adb, '-s', serial, 'shell', text])
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[:500])
    return result.stdout.decode(errors='replace').strip()

def getlog():
    result = command([adb, '-s', serial, 'exec-out', "su -c 'cat /proc/last_kmsg'"], 20)
    if result.returncode or not 1000 <= len(result.stdout) <= 8 * 1024 * 1024:
        raise RuntimeError('Could not capture bounded last_kmsg')
    return result.stdout

SLOT_MAGIC = b'R5QLOGSLOT/V1\0\0\0'
SLOT_RING = 0x200000

def getslot():
    """The firmware's saved RAM log from the slot in the test recovery image, unrolled; b'' if never saved."""
    assert args.log_slot % 4096 == 0
    blocks = 1 + SLOT_RING // 4096
    result = command([adb, '-s', serial, 'exec-out',
                      f"su -c 'dd if=/dev/block/by-name/recovery bs=4096 skip={args.log_slot // 4096} count={blocks} 2>/dev/null'"], 60)
    raw = result.stdout
    (run / 'log-slot.bin').write_bytes(raw)
    info = {'bytes_read': len(raw), 'magic': raw[:16] == SLOT_MAGIC, 'state': raw[32:40].rstrip(b'\0').decode(errors='replace')}
    receipt['log_slot'] = info
    if result.returncode or len(raw) != blocks * 4096 or not info['magic'] or info['state'] != 'SAVED':
        return b''
    info['saves'], info['ring_write_index'], info['ring_magic'] = struct.unpack_from('<III', raw, 40)
    info['why'] = raw[64:128].split(b'\0')[0].decode(errors='replace')
    ring = raw[4096:]
    widx = struct.unpack_from('<I', ring, 8)[0]
    data, cap = ring[16:], SLOT_RING - 16
    text = data[:widx] if widx <= cap else data[widx % cap:] + data[:widx % cap]
    (run / 'firmware-log.txt').write_bytes(text)
    info['text_bytes'] = len(text)
    return text

def operate(action):
    result = command([pwsh, '-NoProfile', '-File', str(operator), '-Action', action], 90)
    (run / (action.lower() + '.txt')).write_bytes(result.stdout + result.stderr)
    print(result.stdout.decode(errors='replace'), flush=True)
    if result.returncode:
        raise RuntimeError(f'{action} failed; see its saved transcript')

before_boot = shell('cat /proc/sys/kernel/random/boot_id')
before = getlog()
receipt['before_log_sha256'] = hashlib.sha256(before).hexdigest()
receipt['before_marker_counts'] = {tag: before.count(tag.encode()) for tag in args.expect}
record()
android_returned = False
try:
    receipt.update(status='test launched; awaiting Android', original_recovery_restored=False)
    manifest['flashed'] = True
    record()
    operate('Start')
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        state = command([adb, '-s', serial, 'get-state'], 5)
        if state.returncode == 0 and state.stdout.strip() == b'device':
            try:
                boot = shell('cat /proc/sys/kernel/random/boot_id')
                if boot and boot != before_boot and 'uid=0' in shell("su -c 'id'"):
                    android_returned = True
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
        time.sleep(3)
    if not android_returned:
        receipt['status'] = 'needs physical access: Android did not return before timeout'
        record()
        raise SystemExit(2)
    log = getlog()
    (run / 'last_kmsg.bin').write_bytes(log)
    if args.log_slot is not None:
        # a cold reset wipes last_kmsg: the firmware's own saved copy of the ring is the run's log then
        log = log + b'\n== FIRMWARE LOG SLOT ==\n' + getslot()
    receipt['after_log_sha256'] = hashlib.sha256(log).hexdigest()
    receipt['after_marker_counts'] = {tag: log.count(tag.encode()) for tag in args.expect}
    receipt['markers_advanced'] = bool(args.expect) and all(
        receipt['after_marker_counts'][tag] > receipt['before_marker_counts'][tag]
        for tag in args.expect)
    # The ring may discard an older copy of shared release/reset markers.
    # Require a newly advanced test-specific anchor, then its ordered tail.
    position = log.rfind(args.expect[0].encode()) if args.expect else -1
    ordered = position >= 0
    for tag in args.expect[1:]:
        position = log.find(tag.encode(), position + 1) if ordered else -1
        ordered = ordered and position >= 0
    receipt['ordered_marker_sequence'] = ordered
    receipt['anchor'] = anchor
    receipt['markers_confirmed'] = bool(args.expect) and ordered and (
        receipt['after_marker_counts'][anchor] > receipt['before_marker_counts'][anchor])
    receipt['status'] = ('hardware markers confirmed' if receipt['markers_confirmed']
                         else 'automatic Android return; required native markers unproven')
except Exception as exc:
    receipt['status'] = 'error: ' + str(exc)
    raise
finally:
    # Restore also on a failed log capture, or a failed Start while Android remains available.
    state = command([adb, '-s', serial, 'get-state'], 5)
    if state.returncode == 0 and state.stdout.strip() == b'device':
        try:
            operate('Restore')
            hashes = shell("su -c 'sha256sum /dev/block/by-name/recovery /dev/block/by-name/boot /dev/block/by-name/vbmeta'")
            expected = [manifest['original_recovery']['sha256'],
                        manifest['original_boot_sha256'], manifest['original_vbmeta_sha256']]
            if [line.split()[0] for line in hashes.splitlines()] != expected:
                raise RuntimeError('Post-restore partition baseline mismatch')
            (run / 'baseline.txt').write_text(hashes + '\n')
            receipt['original_recovery_restored'] = True
            receipt['baseline_verified'] = True
        except Exception as exc:
            receipt['restore_error'] = str(exc)
    record()
    print(json.dumps(receipt, indent=2), flush=True)

if not receipt.get('baseline_verified'):
    raise SystemExit(3)
if not receipt.get('markers_confirmed'):
    raise SystemExit(4)
