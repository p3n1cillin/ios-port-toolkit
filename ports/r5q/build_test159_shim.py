"""Assemble the Test 159 boot shim in the bounded build sandbox."""
import base64
import pathlib
import struct
import subprocess
import sys
import time

port = pathlib.Path(sys.argv[1]).resolve(strict=True)
build = port / 'build'
build.mkdir(exist_ok=True)
loader = (build / 'r5q-test159-loader.efi').read_bytes()
def item(name, data):
    return struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
body = item('loader.efi', loader) + item('kernel', (build / 'test159-xnu.macho').read_bytes())
body += item('ramdisk', (build / 'test159-root.apfs').read_bytes())   # iOS 16.5 redwood volume: md1 (the resident system image is md0)
body += item('rdres', b'948c0000\0')   # system.apfs (2492203008 bytes), put at 0xFC000000 by the firmware's USB RAM drive
body += item('checkpoint', b'0\0') + item('cmdline', b'loader_return_after=86400 epoch=%d touchscreen=1 cpus=8 cpufreq=1 fbscale=2 bootcpu=7 ufs=2 ufs_write=1 var_store=1\0' % int(time.time())) + bytes(24)
payload = struct.pack('<II', 0x50554e58, len(body)) + body
assert len(payload) < 0x03F00000
(build / 'r5q-test159-payload.bin').write_bytes(payload)

def assemble(image_size, helper=False):
    script = f'''set -eu
clang --target=aarch64-linux-gnu -c /src/BootShim.S -o /scratch/shim.o \\
 -DFD_BASE=0x9FC00000 -DFD_SIZE=0x300000 -DPAYLOAD_BASE=0xF0000000 \\
 -DPAYLOAD_MAX=0x04000000 -DTEST_IMAGE_SIZE={image_size}
llvm-objcopy -O binary /scratch/shim.o /scratch/shim.bin
llvm-nm -n /scratch/shim.o >&2
base64 -w0 /scratch/shim.bin
'''
    if helper:
        script = '''set -eu
clang --target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -static \\
 -Wl,-e,_start,--build-id=none /src/reboot-recovery.S -o /scratch/helper
llvm-strip --strip-all /scratch/helper
base64 -w0 /scratch/helper
'''
    command = ['/usr/bin/prlimit', '--as=1073741824', '--cpu=30', '--nproc=1024',
               '--nofile=64', '--fsize=1048576', '--', '/usr/bin/bwrap', '--unshare-all',
               '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
    for path in ['/usr', '/bin', '/lib', '/lib64']:
        command += ['--ro-bind', path, path]
    command += ['--proc', '/proc', '--dev', '/dev', '--size', '8388608', '--tmpfs', '/scratch',
                '--ro-bind', str(port / 'test159'), '/src', '--chdir', '/scratch', '--clearenv',
                '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'HOME', '/scratch',
                '--setenv', 'TMPDIR', '/scratch', '--setenv', 'LC_ALL', 'C',
                '/usr/bin/timeout', '30', '/bin/sh', '-c', script]
    result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=40)
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    if not helper:
        (build / 'r5q-test159-shim-symbols.txt').write_bytes(result.stderr)
    binary = base64.b64decode(result.stdout, validate=True)
    if helper:
        if binary[:4] != b'\x7fELF' or len(binary) > 65536:
            raise ValueError('Invalid recovery entry helper')
        return binary
    if len(binary) > 4096 or len(binary) % 16 or binary[56:60] != b'ARM\x64':
        raise ValueError('Invalid ARM64 boot shim')
    return binary

shim = assemble(0)
image_size = (len(shim) + 0x300000 + len(payload) + 4095) & ~4095
shim = assemble(image_size)
assert struct.unpack_from('<Q', shim, 16)[0] == image_size
(build / 'r5q-test159-shim.bin').write_bytes(shim)
fd_path = build / 'r5q-test159-uefi.fd'
fd = fd_path.read_bytes()
assert len(fd) == 0x300000
image = shim + fd + payload
image += bytes(image_size - len(image))
(build / 'r5q-test159-kernel.bin').write_bytes(image)
(build / 'reboot-recovery').write_bytes(assemble(0, helper=True))
print(f'Test 159 ARM64 shim {len(shim)} bytes; FD {fd_path.name}; payload {len(payload)} bytes; complete Image {len(image)} bytes')
