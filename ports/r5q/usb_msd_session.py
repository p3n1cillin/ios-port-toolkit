"""PC side of the phone's USB RAM drive (Test 46+), run alongside run_native_test.py.

Waits for the volume labelled R5QRAM (the phone's firmware serving a FAT32 RAM disk through Qualcomm UsbMsdDxe),
copies the payload files, then DONE.TXT with "<NAME> <size> <crc32>" lines; the firmware checks each file and logs
MATCH/MISMATCH into its log slot. Only ordinary file copies onto that removable RAM drive: nothing on the PC changes.
"""
import argparse
import json
import os
import pathlib
import subprocess
import time
import zlib

p = argparse.ArgumentParser()
p.add_argument('out', type=pathlib.Path)
p.add_argument('--wait', type=int, default=240)
p.add_argument('--test-mib', type=int, default=256, help='random TEST.BIN size (0: none)')
p.add_argument('--file', action='append', default=[], help='extra payload file (8.3 name kept)')
p.add_argument('--install', action='store_true',
               help='Test 70+: ask the firmware to write the delivered system image into the phone\'s userdata partition '
                    '(ERASES Android\'s data; only with the owner\'s explicit go-ahead)')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
report = {'started': time.strftime('%Y-%m-%d %H:%M:%S'), 'files': []}

def save():
    (args.out / 'usb-msd-report.json').write_text(json.dumps(report, indent=2) + '\n')

def drive():
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "(Get-Volume -FileSystemLabel R5QRAM -ErrorAction SilentlyContinue | "
                        "Where-Object DriveLetter | Select-Object -First 1).DriveLetter"],
                       capture_output=True, timeout=20)
    letter = r.stdout.decode(errors='replace').strip()
    return letter[:1] if letter else None

def copy_unbuffered(src, name, root):
    """A file whose size is a multiple of 4096, written with FILE_FLAG_NO_BUFFERING | WRITE_THROUGH in 8 MiB
    page-aligned writes: Windows then hands the drive large transfers instead of its small cached ones
    (the buffered copy ran at exactly 10.0 MiB/s in Tests 51-63)."""
    import ctypes
    import mmap
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    GENERIC_WRITE, CREATE_ALWAYS = 0x40000000, 2
    NO_BUFFERING, WRITE_THROUGH = 0x20000000, 0x80000000
    CHUNK = 8 << 20
    h = k32.CreateFileW(str(root / name), GENERIC_WRITE, 0, None, CREATE_ALWAYS, NO_BUFFERING | WRITE_THROUGH, None)
    if h in (None, wintypes.HANDLE(-1).value):
        raise OSError(ctypes.get_last_error(), 'CreateFileW')
    buf = mmap.mmap(-1, CHUNK)                     # page-aligned, as unbuffered I/O requires
    addr = ctypes.addressof(ctypes.c_char.from_buffer(buf))
    t0 = time.monotonic()
    crc = total = 0
    try:
        with open(src, 'rb') as reader:
            while True:
                n = reader.readinto(memoryview(buf))
                if not n:
                    break
                crc = zlib.crc32(memoryview(buf)[:n], crc)
                written = wintypes.DWORD(0)
                if not k32.WriteFile(h, addr, n, ctypes.byref(written), None) or written.value != n:
                    raise OSError(ctypes.get_last_error(), 'WriteFile at %d' % total)
                total += n
                if total % (256 << 20) == 0:
                    report['progress'] = {'name': name, 'MiB': total >> 20, 'seconds': round(time.monotonic() - t0, 1),
                                          'mode': 'unbuffered'}
                    save()
    finally:
        k32.CloseHandle(h)
    dt = time.monotonic() - t0
    report['files'].append({'name': name, 'bytes': total, 'crc32': '%08x' % crc, 'seconds': round(dt, 2),
                            'MiB_per_s': round(total / 1048576 / dt, 1) if dt else None, 'mode': 'unbuffered 8 MiB'})
    save()
    return '%s %d %08x' % (name, total, crc)


def copy(src, name, root):
    """src: bytes, or a path streamed in 8 MiB chunks (the iOS system volume is 2.4 GB)."""
    if not isinstance(src, (bytes, bytearray)) and os.path.getsize(src) % 4096 == 0 and os.name == 'nt':
        try:
            return copy_unbuffered(src, name, root)
        except OSError as e:
            report['unbuffered_failed'] = str(e)
            save()
    t0 = time.monotonic()
    crc = 0
    total = 0
    reader = open(src, 'rb') if not isinstance(src, (bytes, bytearray)) else None
    with open(root / name, 'wb') as f:
        i = 0
        while True:
            chunk = reader.read(8 << 20) if reader else src[i:i + (8 << 20)]
            i += 8 << 20
            if not chunk:
                break
            f.write(chunk)
            crc = zlib.crc32(chunk, crc)
            total += len(chunk)
            if total % (256 << 20) == 0:
                report['progress'] = {'name': name, 'MiB': total >> 20, 'seconds': round(time.monotonic() - t0, 1)}
                save()
        f.flush()
        os.fsync(f.fileno())
    if reader:
        reader.close()
    dt = time.monotonic() - t0
    report['files'].append({'name': name, 'bytes': total, 'crc32': '%08x' % crc, 'seconds': round(dt, 2),
                            'MiB_per_s': round(total / 1048576 / dt, 1) if dt else None})
    save()
    return '%s %d %08x' % (name, total, crc)

deadline = time.monotonic() + args.wait
letter = None
while time.monotonic() < deadline and not letter:
    letter = drive()
    if not letter:
        time.sleep(1)
if not letter:
    report['result'] = 'no R5QRAM volume within %d s' % args.wait
    save()
    raise SystemExit(2)
root = pathlib.Path(letter + ':\\')
report['drive'] = letter
report['found_at'] = time.strftime('%H:%M:%S')
report['listing_before'] = sorted(os.listdir(root))
save()
lines = []
if args.test_mib:
    lines.append(copy(os.urandom(args.test_mib << 20), 'TEST.BIN', root))
for f in args.file:   # "path" or "path=NAME.EXT" (an 8.3 name: system.apfs=SYSTEM.APF)
    src, _, name = f.partition('=')
    lines.append(copy(pathlib.Path(src), (name or pathlib.Path(src).name).upper(), root))
# Test 66+: the time, for iOS's clock (the firmware patches the kernel's epoch= with it, plus the seconds since)
with open(root / 'EPOCH.TXT', 'wb') as f:
    report['epoch'] = int(time.time())
    f.write(b'%d\r\n' % report['epoch'])
    f.flush()
    os.fsync(f.fileno())
if args.install:   # the exact token the firmware looks for (R5Q_INSTALL_TOKEN in R5qUsbRamDisk.c)
    with open(root / 'INSTALL.TXT', 'wb') as f:
        f.write(b'WRITE-IOS-SYSTEM-IMAGE-TO-USERDATA\r\n')
        f.flush()
        os.fsync(f.fileno())
    report['install'] = True
lines_text = ('\r\n'.join(lines) + '\r\n').encode()
with open(root / 'DONE.TXT', 'wb') as f:
    f.write(lines_text)
    f.flush()
    os.fsync(f.fileno())
report['done_txt'] = lines_text.decode()
report['result'] = 'files copied; the firmware log has the phone-side check'
save()
print(json.dumps(report, indent=2))
