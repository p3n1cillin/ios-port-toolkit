"""Put the iOS system image on a microSD card once, so the phone's firmware loads it from the card at every boot
(Test 69+: R5qFileSystemLoad) instead of the PC copying it over USB each time.

The card must be FAT32 (the firmware's FAT driver does not read exFAT); SYSTEM.APF is 2.4 GB, under FAT32's 4 GB file
limit. A card bigger than 32 GB comes as exFAT: give it a FAT32 partition of 4 GB or more first.

usage: python prepare_sdcard.py E:            (the card's drive letter)
Only these two files are written to the card: SYSTEM.APF and SYSTEM.CRC. Nothing on the PC or the phone changes.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import zlib

IMAGE = pathlib.Path(__file__).parent / 'port/build/test51-system.apfs'
KNOWN_CRC = 0xcdc272da          # the image's CRC32, as the firmware checked it over USB in Tests 51-68

p = argparse.ArgumentParser()
p.add_argument('drive', help='the card, e.g. E:')
args = p.parse_args()
root = pathlib.Path(args.drive.rstrip('\\/') + '\\')
r = subprocess.run(['powershell', '-NoProfile', '-Command',
                    f"(Get-Volume -DriveLetter {args.drive[0]}).FileSystemType"], capture_output=True, text=True)
fs = r.stdout.strip()
if fs.upper() != 'FAT32':
    raise SystemExit(f'{args.drive} is {fs or "unknown"}, not FAT32: the phone firmware reads FAT32 only')
size = IMAGE.stat().st_size
free = shutil.disk_usage(root).free
if free < size + (1 << 20) and not (root / 'SYSTEM.APF').exists():
    raise SystemExit(f'{args.drive} has {free >> 20} MB free; the image needs {size >> 20} MB')
crc = 0
with open(IMAGE, 'rb') as src, open(root / 'SYSTEM.APF', 'wb') as dst:
    while True:
        chunk = src.read(8 << 20)
        if not chunk:
            break
        crc = zlib.crc32(chunk, crc)
        dst.write(chunk)
    dst.flush()
    os.fsync(dst.fileno())
if crc != KNOWN_CRC:
    raise SystemExit(f'the image read as crc32 {crc:08x}, not {KNOWN_CRC:08x}: not finishing the card')
(root / 'SYSTEM.CRC').write_text(f'{crc:08x}\n')
print(f'{args.drive} SYSTEM.APF {size} bytes, SYSTEM.CRC {crc:08x}: the phone will load iOS from this card')
