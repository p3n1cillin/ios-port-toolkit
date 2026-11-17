"""EMULATION ONLY: boot a kernel under QEMU virt (GICv3, Cortex-A76) with the donor loader.

usage: run_qemu_kernel.py ROOT KERNEL LABEL [SECONDS]

Kernel-only payload (no iOS root image): shows how far XNU itself gets on the generic qemu-virt
platform. QEMU runs inside bwrap with no network (-nic none, --unshare-all) on a disposable copy.
The phone is never involved; nothing here is bundled for hardware.
"""
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
kernel_path = pathlib.Path(sys.argv[2]).resolve(strict=True)
label = sys.argv[3]
assert label.replace('-', '').isalnum()
seconds = int(sys.argv[4]) if len(sys.argv) > 4 else 60
checkpoint = int(sys.argv[5]) if len(sys.argv) > 5 else None   # stop and return at this kernel checkpoint
donor = pathlib.Path('/var/tmp/codex-ios-r5q/work/linux-source')
work = pathlib.Path('/var/tmp/codex-ios-r5q/work/qemu-kernel-runs') / label
out = root / 'port/build/qemu-boot' / label
sha = lambda data: hashlib.sha256(data).hexdigest()

shutil.rmtree(work, ignore_errors=True)
(work / 'esp/EFI/BOOT').mkdir(parents=True)
loader = (donor / 'prebuilt/loader.efi').read_bytes()
(work / 'esp/EFI/BOOT/BOOTAA64.EFI').write_bytes(loader)
shutil.copyfile('/usr/share/AAVMF/AAVMF_VARS.fd', work / 'vars.fd')
kernel = kernel_path.read_bytes()


def item(name, data):
    return struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)


cmdline = f'loader_return_after={seconds} -v'
body = item('kernel', kernel) + item('cmdline', cmdline.encode() + b'\0')
if checkpoint is not None:
    body += item('checkpoint', f'{checkpoint}\0'.encode())
body += bytes(24)
(work / 'payload.bin').write_bytes(struct.pack('<II', 0x50554e58, len(body)) + body)

command = ['/usr/bin/prlimit', '--as=17179869184', '--nproc=256', '--nofile=512', '--',
           '/usr/bin/bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64', '/etc/alternatives']:
    command += ['--ro-bind', path, path]
command += ['--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--tmpfs', '/var/tmp', '--bind', str(work), '/run-dir',
            '--chdir', '/run-dir', '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'HOME', '/tmp',
            '/usr/bin/timeout', str(seconds + 60), '/usr/bin/qemu-system-aarch64',
            '-M', 'virt,gic-version=3', '-cpu', 'cortex-a76', '-smp', '1', '-m', '4G',
            '-accel', 'tcg,tb-size=384',
            '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd',
            '-drive', 'if=pflash,format=raw,file=vars.fd',
            '-drive', 'file=fat:rw:esp,format=raw,if=virtio',
            '-device', 'loader,file=payload.bin,addr=0xF0000000,force-raw=on',
            '-device', 'ramfb', '-nic', 'none',
            '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot']
start = time.monotonic()
result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=seconds + 90)
elapsed = round(time.monotonic() - start, 1)
log = result.stdout.decode(errors='replace').replace('\r', '')
out.mkdir(parents=True, exist_ok=True)
(out / 'serial.log').write_text(log)
markers = {name: bool(re.search(pattern, log)) for name, pattern in [
    ('loader_started', r'XNULOADER'),
    ('kernel_entered', r'jumping to'),
    ('darwin_banner', r'Darwin Kernel Version'),
    ('kprintf', r'kprintf initialized|vm_page_bootstrap|zone_init'),
    ('iokit', r'IOKit|IOPlatformExpert'),
    ('bsd_init', r'bsd_init|BSD root|root device|Waiting on'),
    ('panic', r'panic\(|Panicked'),
    ('kernel_returned', r'kernel returned|STOPPED AT|EARLY KERNEL EXCEPTION'),
]}
if checkpoint is not None:
    markers['stopped_at_requested_checkpoint'] = f'STOPPED AT CHECKPOINT {checkpoint}\n' in log
    markers['early_exception'] = 'EARLY KERNEL EXCEPTION' in log
receipt = {'scope': 'EMULATION ONLY: QEMU virt, cortex-a76, GICv3, 1 core, 4 GiB, no network, kernel-only payload '
                    '(no iOS root image); generic qemu-virt platform, not r5q hardware',
           'kernel': kernel_path.name, 'kernel_sha256': sha(kernel), 'donor_loader_sha256': sha(loader),
           'cmdline': cmdline, 'checkpoint': checkpoint, 'qemu_exit': result.returncode, 'seconds': elapsed, 'log_lines': log.count('\n'),
           'markers': markers, 'hardware_approved': False, 'phone_partitions_written': False}
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
if result.stderr:
    sys.stderr.write(result.stderr.decode(errors='replace')[-2000:])
