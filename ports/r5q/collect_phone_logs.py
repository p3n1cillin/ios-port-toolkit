"""The installed phone's logs, over USB (firmware fulldbg25+, Test 72+). When a start finds that the start before it never
reached iOS, the firmware shows "LAST START FAILED: LOGS ON THE USB DRIVE" and serves the R5QRAM drive for 90 s with
LOGINFO.TXT, LOGPREV.BIN (the failed start's saved log) and LOGNOW.BIN (this start's, so far). This takes them, writes
them out as text, gives the phone the time (EPOCH.TXT) and says it is done (DONE.TXT), and the phone goes on to iOS.

usage: python collect_phone_logs.py OUT_DIR [--wait SECONDS]
Only reads from the phone's RAM drive and writes those two small files to it.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('out', type=pathlib.Path)
p.add_argument('--wait', type=int, default=600, help='how long to wait for the drive')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
RING = 0x200000


def drive():
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "(Get-Volume -FileSystemLabel R5QRAM -ErrorAction SilentlyContinue | "
                        "Where-Object DriveLetter | Select-Object -First 1).DriveLetter"], capture_output=True, timeout=20)
    letter = r.stdout.decode(errors='replace').strip()
    return letter[:1] if letter else None


def unroll(raw):
    """A saved slot (header block + the 2 MiB RAM ring) as text: the ring's [8:12] is how many bytes were ever written."""
    ring = raw[4096:4096 + RING]
    if len(ring) < 16 or ring[4:8] != b'LOGM':
        return b''
    widx = struct.unpack_from('<I', ring, 8)[0]
    data, cap = ring[16:], RING - 16
    return data[:widx] if widx <= cap else data[widx % cap:] + data[:widx % cap]


deadline = time.monotonic() + args.wait
letter = None
print(f'waiting up to {args.wait} s for the phone\'s R5QRAM drive ...', flush=True)
while time.monotonic() < deadline and not letter:
    letter = drive()
    if not letter:
        time.sleep(1)
if not letter:
    raise SystemExit('no R5QRAM drive appeared')
root = pathlib.Path(letter + ':\\')
stamp = time.strftime('%Y%m%d-%H%M%S')
dest = args.out / f'phone-logs-{stamp}'
dest.mkdir()
for name in ['LOGINFO.TXT', 'LOGPREV.BIN', 'LOGNOW.BIN']:
    src = root / name
    if src.exists():
        data = src.read_bytes()
        (dest / name.lower()).write_bytes(data)
        if name.endswith('.BIN'):
            text = unroll(data)
            (dest / (name.lower()[:-4] + '.txt')).write_bytes(text)
            header = data[:96]
            boot, state, saves = struct.unpack_from('<III', header, 16) if header[:9] == b'R5QUFSLOG' else (0, 0, 0)
            why = header[32:96].split(b'\0')[0].decode(errors='replace')
            print(f'{name}: start {boot}, state {state}, save {saves} ({why}); {len(text)} bytes of log -> {dest / (name.lower()[:-4] + ".txt")}')
        else:
            print(data.decode(errors='replace'))
for name, content in [('EPOCH.TXT', b'%d\r\n' % int(time.time())), ('DONE.TXT', b'LOGS-TAKEN 0 0\r\n')]:
    with open(root / name, 'wb') as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
print(f'logs in {dest}; the phone goes on to iOS')
