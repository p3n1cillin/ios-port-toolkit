"""Enlarge the payload while preserving Test 5 firmware and diagnostic app exactly."""
import hashlib
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
build = root / 'port/build'
fd = (build / 'r5q-test5-uefi.fd').read_bytes()
loader = (build / 'r5q-test5-loader.efi').read_bytes()
assert hashlib.sha256(fd).hexdigest() == '6ad31c6737b735567931f68f72df512b80b8b3656d6ef785487fa7cc4da94960'
assert hashlib.sha256(loader).hexdigest() == '853d2d351b584d8b1642038e7d2d6d430d999d514fea188581d15b0642e8ced9'
kernel = (build / 'test8-xnu.macho').read_bytes()
assert hashlib.sha256(kernel).hexdigest() == '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'

def item(name, data):
    return struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)

body = item('loader.efi', loader) + item('kernel', kernel)
body += item('checkpoint', b'1\0') + item('cmdline', b'-v\0') + bytes(24)
payload = struct.pack('<II', 0x50554e58, len(body)) + body
shim = (build / 'r5q-test8-shim.bin').read_bytes()
size = struct.unpack_from('<Q', shim, 16)[0]
image = shim + fd + payload
assert len(image) <= size == 18825216
image += bytes(size - len(image))
for name, data in [('shim.bin', shim), ('uefi.fd', fd), ('loader.efi', loader),
                   ('payload.bin', payload), ('kernel.bin', image)]:
    (build / ('r5q-test9-' + name)).write_bytes(data)
print('Test 9 large-payload control assembled; XNU is unused data, not executed.')
