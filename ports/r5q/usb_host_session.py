"""PC side of the phone's UEFI fastboot session (Test 45+), run alongside run_native_test.py.

Waits for the phone's fastboot device (VID 18D1 PID D00D, bound to inbox WinUSB through MS OS 2.0 descriptors),
queries it, pushes a random payload with `fastboot stage`, checks the phone's CRC32 of what it received, then sends
`continue`. Writes a JSON report and a transcript. Never flashes anything: the session only fills phone RAM.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import time
import zlib

p = argparse.ArgumentParser()
p.add_argument('out', type=pathlib.Path)
p.add_argument('--wait', type=int, default=240)
p.add_argument('--size-mib', type=int, default=256)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
fastboot = shutil.which('fastboot')
assert fastboot
report = {'started': time.strftime('%Y-%m-%d %H:%M:%S'), 'fastboot': fastboot, 'steps': []}
log = open(args.out / 'usb-host-transcript.txt', 'w', encoding='utf-8')

def save():
    (args.out / 'usb-host-report.json').write_text(json.dumps(report, indent=2) + '\n')

def run(argv, timeout=30):
    t0 = time.monotonic()
    try:
        r = subprocess.run(argv, capture_output=True, timeout=timeout)
        out = (r.stdout + r.stderr).decode(errors='replace')
        code = r.returncode
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b'') + (e.stderr or b'')).decode(errors='replace') + '\n<timeout>'
        code = -1
    dt = time.monotonic() - t0
    log.write(f'$ {" ".join(map(str, argv))}  [{code}, {dt:.2f} s]\n{out}\n')
    log.flush()
    return code, out, dt

def pnp():
    code, out, _ = run(['powershell', '-NoProfile', '-Command',
        "Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\\VID_18D1&PID_D00D*' } | "
        "Format-List Status,Class,FriendlyName,InstanceId,Problem | Out-String -Width 200"], 20)
    return out.strip()

deadline = time.monotonic() + args.wait
seen_pnp = ''
serial = None
while time.monotonic() < deadline:
    code, out, _ = run([fastboot, 'devices'], 10)
    lines = [l for l in out.splitlines() if l.strip().endswith('fastboot')]
    if lines:
        serial = lines[0].split()[0]
        break
    info = pnp()
    if info and info != seen_pnp:
        seen_pnp = info
        report['steps'].append({'pnp_seen': info, 't': time.strftime('%H:%M:%S')})
        save()
    time.sleep(1)
report['pnp'] = pnp() or seen_pnp
if not serial:
    report['result'] = 'no fastboot device within %d s' % args.wait
    save()
    raise SystemExit(2)
report['serial'] = serial
report['found_at'] = time.strftime('%H:%M:%S')
for var in ['version', 'product', 'serialno', 'max-download-size', 'r5q-speed']:
    code, out, dt = run([fastboot, '-s', serial, 'getvar', var], 15)
    report['steps'].append({'getvar': var, 'code': code, 'out': out.strip()})
save()

payload = args.out / 'usb-payload.bin'
data = os.urandom(args.size_mib << 20)
payload.write_bytes(data)
crc = zlib.crc32(data) & 0xFFFFFFFF
report['payload'] = {'bytes': len(data), 'crc32': '%08x' % crc}
code, out, dt = run([fastboot, '-s', serial, 'stage', str(payload)], 300)
report['stage'] = {'code': code, 'seconds': round(dt, 2), 'MiB_per_s': round(len(data) / 1048576 / dt, 1) if dt else None,
                   'out': out.strip()}
save()
code, out, dt = run([fastboot, '-s', serial, 'oem', 'crc'], 60)
report['oem_crc'] = {'code': code, 'out': out.strip()}
report['crc_match'] = ('crc32=%08x' % crc) in out and ('size=%d' % len(data)) in out
save()
code, out, dt = run([fastboot, '-s', serial, 'continue'], 20)
report['continue'] = {'code': code, 'out': out.strip()}
report['result'] = 'payload verified in phone RAM' if report['crc_match'] else 'transfer done; CRC not confirmed'
save()
payload.unlink()
print(json.dumps({k: report[k] for k in ['result', 'stage', 'oem_crc', 'crc_match']}, indent=2))
