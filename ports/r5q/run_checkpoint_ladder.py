"""EMULATION ONLY: walk a kernel up the donor's bring-up checkpoints in Unicorn.

usage: run_checkpoint_ladder.py ROOT KERNEL LABEL [CHECKPOINT ...]

For each checkpoint (in kernel execution order) build an emulation-only Test 15-profile loader
with --emu-checkpoint N, package KERNEL exactly like build_test15_shim.py (checkpoint entry N),
and run test_checkpoint_ladder.py in the bounded, no-network sandbox. Stops at the first
checkpoint not reached. Nothing produced here is bundled or approved for the phone.
"""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys

# Order in which the donor kernel executes them (start.s, then arm_init()).
ORDER = [1, 2, 3, 4, 5, 10, 11, 30, 31, 32, 33, 34, 35, 12, 40, 41, 42, 43, 44, 13, 14, 15, 16, 17, 18, 19, 20, 21]
root = pathlib.Path(sys.argv[1]).resolve(strict=True)
kernel_path = pathlib.Path(sys.argv[2]).resolve(strict=True)
label = sys.argv[3]
assert label.replace('-', '').isalnum()
wanted = [int(x) for x in sys.argv[4:]] or ORDER
assert all(n in ORDER for n in wanted)
out = root / 'port/build/checkpoint-ladder' / label
venv = pathlib.Path('/var/tmp/codex-ios-r5q/work/mu-venv')
kernel = kernel_path.read_bytes()
sha = lambda data: hashlib.sha256(data).hexdigest()
assert kernel.count(b'XNULDRST') == 1
rows = []
for n in wanted:
    inputs = out / f'cp{n:02d}'
    inputs.mkdir(parents=True, exist_ok=True)
    loader_path = inputs / 'r5q-test15-loader.efi'
    subprocess.run([sys.executable, '-I', str(root / 'build_loader_linux.py'), str(root / 'port/xnuloader'),
                    str(loader_path), '--emu-checkpoint', str(n)], check=True, capture_output=True, timeout=150)
    loader = loader_path.read_bytes()

    def item(name, data):
        return struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
    body = item('loader.efi', loader) + item('kernel', kernel)
    body += item('checkpoint', f'{n}\0'.encode()) + item('cmdline', b'-v\0') + bytes(24)
    payload = struct.pack('<II', 0x50554e58, len(body)) + body
    assert len(payload) < 0x03F00000
    (inputs / 'r5q-test15-payload.bin').write_bytes(payload)
    (inputs / 'kernel.macho').write_bytes(kernel)
    (inputs / 'checkpoint.txt').write_text(str(n))
    command = ['/usr/bin/prlimit', '--as=3221225472', '--cpu=150', '--nproc=128',
               '--nofile=128', '--fsize=1048576', '--', '/usr/bin/bwrap',
               '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
    for path in ['/usr', '/bin', '/lib', '/lib64']:
        command += ['--ro-bind', path, path]
    command += ['--proc', '/proc', '--dev', '/dev', '--size', '16777216', '--tmpfs', '/scratch',
                '--ro-bind', str(inputs), '/inputs', '--ro-bind', str(venv), '/venv',
                '--ro-bind', str(root / 'test_checkpoint_ladder.py'), '/test.py', '--chdir', '/scratch',
                '--clearenv', '--setenv', 'PATH', '/venv/bin:/usr/bin:/bin', '--setenv', 'HOME', '/scratch',
                '--setenv', 'TMPDIR', '/scratch', '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
                '/usr/bin/timeout', '140', '/venv/bin/python', '/test.py']
    result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=150)
    try:
        row = json.loads(result.stdout)
    except ValueError:
        row = {'checkpoint': n, 'result': 'harness-error', 'exit': result.returncode,
               'stderr_tail': result.stderr[-3000:].decode(errors='replace')}
    row['loader_sha256'] = sha(loader)
    rows.append(row)
    print(f"checkpoint {n:2d}: {row['result']}", flush=True)
    if row['result'] != 'passed':
        break
receipt = {'scope': 'EMULATION ONLY (Unicorn ARM64 MAX): actual relocated loader + kernel instructions; '
                    'no phone MMIO, GIC, caches, hypervisor or timers beyond Unicorn defaults',
           'kernel': str(kernel_path.name), 'kernel_sha256': sha(kernel),
           'harness_sha256': sha((root / 'test_checkpoint_ladder.py').read_bytes()),
           'loader_source_sha256': sha((root / 'port/xnuloader/loader.c').read_bytes()),
           'highest_reached': max([r['checkpoint'] for r in rows if r['result'] == 'passed'],
                                  key=ORDER.index, default=None),
           'hardware_approved': False, 'phone_partitions_written': False, 'checkpoints': rows}
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({k: v for k, v in receipt.items() if k != 'checkpoints'}, indent=2))
last = rows[-1]
if last['result'] != 'passed':
    print(json.dumps(last, indent=2))
