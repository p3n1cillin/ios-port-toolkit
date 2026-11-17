"""Restore stock recovery.img via Samsung Download Mode (Odin/Loke protocol on COM14)."""
import hashlib
import os
import pathlib
import serial
import struct
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM14"
IMG_PATH = pathlib.Path(os.environ.get('R5Q_RECOVERY_IMG', 'recovery-original.img'))   # the phone's own original recovery
EXPECTED_SHA256 = "332e1dd4a1c87bcd929696c5e1aa4f6730795c5de440acface1572ae3ba383cd"

data = IMG_PATH.read_bytes()
assert hashlib.sha256(data).hexdigest() == EXPECTED_SHA256, "Stock recovery SHA256 mismatch!"
print(f"Loaded stock recovery.img ({len(data)} bytes, SHA256 verified)")

ser = serial.Serial(PORT, baudrate=115200, timeout=30)

def send_pkt(cmd, arg1=0, arg2=0, arg3=0):
    pkt = struct.pack("<IIII", cmd, arg1, arg2, arg3) + bytes(1024 - 16)
    ser.write(pkt)
    resp = ser.read(8)
    if len(resp) < 8:
        raise RuntimeError(f"Short response ({len(resp)} bytes) for cmd 0x{cmd:x}")
    rcmd, rarg = struct.unpack("<II", resp)
    return rcmd, rarg

# 1. Handshake
ser.write(b"ODIN")
loke = ser.read(4)
print(f"Handshake: {loke!r}")
if loke != b"LOKE":
    raise RuntimeError(f"Expected LOKE, got {loke!r}")

# 2. Begin session (0x64, 0x00, arg2=0, arg3=4 -> protocol v4)
rcmd, rarg = send_pkt(0x64, 0, 0, 4)
print(f"Begin session: cmd=0x{rcmd:x}, arg=0x{rarg:x}")

# 3. Set total bytes (0x64, 0x02, total_bytes)
rcmd, rarg = send_pkt(0x64, 2, len(data))
print(f"Total bytes: cmd=0x{rcmd:x}, arg=0x{rarg:x}")

# 4. Get PIT (0x65, 0x01)
rcmd, pit_size = send_pkt(0x65, 1)
print(f"PIT size: {pit_size} bytes")
pit_data = bytearray()
# Read PIT in 500-byte chunks
chunks = (pit_size + 499) // 500
for i in range(chunks):
    ser.write(struct.pack("<II", 0x65, 2) + struct.pack("<I", i) + bytes(1024 - 12))
    chunk = ser.read(500)
    pit_data.extend(chunk)
send_pkt(0x65, 3)

# Parse PIT for RECOVERY
magic, entry_count = struct.unpack_from("<II", pit_data, 0)
assert magic == 0x12349876, f"Bad PIT magic 0x{magic:x}"
rec_id = None
for i in range(entry_count):
    off = 28 + i * 132
    part_id = struct.unpack_from("<I", pit_data, off + 8)[0]
    part_name = pit_data[off + 36:off + 68].split(b"\x00")[0].decode("ascii", errors="ignore")
    if part_name.upper() == "RECOVERY":
        rec_id = part_id
        print(f"Found RECOVERY in PIT: part_id={rec_id}")
        break
assert rec_id is not None, "RECOVERY not found in PIT!"

# 5. Flash RECOVERY
# Request file transfer (0x66, 0x00)
rcmd, rarg = send_pkt(0x66, 0)
print(f"File transfer start: cmd=0x{rcmd:x}, arg=0x{rarg:x}")

# Send in 128KB blocks (or 32MB chunks, standard Odin v3/v4)
BLK = 131072
PKT_SZ = 131072 * 8  # 1MB per Odin sequence
offset = 0
while offset < len(data):
    seq = data[offset:offset + PKT_SZ]
    # 0x66, 0x02, seq_size
    rcmd, rarg = send_pkt(0x66, 2, len(seq))
    # Send in 128KB packets
    for sub in range(0, len(seq), BLK):
        ser.write(seq[sub:sub + BLK])
    # Wait for ack (0x66, 0x03)
    resp = ser.read(8)
    rcmd, rarg = struct.unpack("<II", resp)
    # 0x66, 0x03, 0, part_id, ...
    offset += len(seq)
    if offset % (16 * 1024 * 1024) == 0 or offset == len(data):
        print(f"Sent {offset}/{len(data)} bytes")

# End file transfer (0x66, 0x03, 0, len(data), 0, rec_id)
pkt = struct.pack("<IIIIII", 0x66, 3, 0, len(data), 0, rec_id) + bytes(1024 - 24)
ser.write(pkt)
resp = ser.read(8)
rcmd, rarg = struct.unpack("<II", resp)
print(f"Flash end: cmd=0x{rcmd:x}, arg=0x{rarg:x}")

# 6. End session & Reboot (0x67, 0x01)
rcmd, rarg = send_pkt(0x67, 1)
print(f"Rebooting device: cmd=0x{rcmd:x}, arg=0x{rarg:x}")
ser.close()
