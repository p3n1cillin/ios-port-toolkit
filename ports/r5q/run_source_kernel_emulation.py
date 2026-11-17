"""Emulate the source-built XNU through the unmodified Test 15 EFI -> checkpoint1 -> return test.

Host-only. Rebuilds the Test 15 payload around (a) the pinned prebuilt kernel, which must
reproduce the shipped r5q-test15-payload.bin byte for byte, and (b) the source-built kernel,
then runs test_test15_roundtrip.py unchanged against each in the same bounded sandbox that
run_test15_checks.py uses. Nothing here touches the phone or produces a flashable image.
"""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
build = root / 'port/build'
out = build / 'source-kernel-emulation'
venv = pathlib.Path('/var/tmp/codex-ios-r5q/work/mu-venv')
PREBUILT_SHA = '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
sha = lambda data: hashlib.sha256(data).hexdigest()

loader = (build / 'r5q-test15-loader.efi').read_bytes()
shipped = (build / 'r5q-test15-payload.bin').read_bytes()
prebuilt = (build / 'test15-xnu.macho').read_bytes()
source = (build / 'xnu-source-rebuild.macho').read_bytes()
source_receipt = json.loads((build / 'xnu-source-rebuild.receipt.json').read_text())
assert sha(prebuilt) == PREBUILT_SHA
assert sha(source) == source_receipt['kernel_sha256'], 'source kernel does not match its build receipt'
assert source.count(b'XNULDRST') == 1


def payload(kernel):
    # Identical layout to build_test15_shim.py.
    def item(name, data):
        return struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
    body = item('loader.efi', loader) + item('kernel', kernel)
    body += item('checkpoint', b'1\0') + item('cmdline', b'-v\0') + bytes(24)
    data = struct.pack('<II', 0x50554e58, len(body)) + body
    assert len(data) < 0x03F00000
    return data


rebuilt_prebuilt = payload(prebuilt)
assert rebuilt_prebuilt == shipped, 'payload reconstruction does not reproduce the shipped Test 15 payload'

receipt = {'scope': 'host emulation only: actual relocated Test 15 EFI loader + kernel instructions in Unicorn '
                    'ARM64 MAX to XNU checkpoint1 and loader context return; no phone MMU/cache/hypervisor model',
           'test_script_sha256': sha((root / 'test_test15_roundtrip.py').read_bytes()),
           'loader_sha256': sha(loader), 'payload_reconstruction_matches_shipped': True,
           'hardware_approved': False, 'phone_partitions_written': False, 'variants': []}
for name, kernel in [('prebuilt', prebuilt), ('source-build', source)]:
    inputs = out / name
    inputs.mkdir(parents=True, exist_ok=True)
    data = payload(kernel)
    (inputs / 'r5q-test15-loader.efi').write_bytes(loader)
    (inputs / 'r5q-test15-payload.bin').write_bytes(data)
    command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=120', '--nproc=128',
               '--nofile=128', '--fsize=1048576', '--', '/usr/bin/bwrap',
               '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
    for path in ['/usr', '/bin', '/lib', '/lib64']:
        command += ['--ro-bind', path, path]
    command += ['--proc', '/proc', '--dev', '/dev', '--size', '16777216', '--tmpfs', '/scratch',
                '--ro-bind', str(inputs), '/inputs', '--ro-bind', str(venv), '/venv',
                '--ro-bind', str(root / 'test_test15_roundtrip.py'), '/test.py', '--chdir', '/scratch',
                '--clearenv', '--setenv', 'PATH', '/venv/bin:/usr/bin:/bin', '--setenv', 'HOME', '/scratch',
                '--setenv', 'TMPDIR', '/scratch', '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
                '/usr/bin/timeout', '120', '/venv/bin/python', '/test.py']
    result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=130)
    row = {'variant': name, 'kernel_sha256': sha(kernel), 'kernel_bytes': len(kernel),
           'payload_sha256': sha(data), 'exit': result.returncode}
    if result.returncode == 0 and len(result.stdout) <= 65536:
        detail = json.loads(result.stdout)
        row['result'] = detail['result']
        row['cases'] = [{'case': c['case'], 'result': c['result'],
                         'console_tail': c['console'][-600:]} for c in detail['cases']]
    else:
        row['result'] = 'failed'
        row['stderr_tail'] = result.stderr[-4000:].decode(errors='replace')
    receipt['variants'].append(row)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
raise SystemExit(0 if all(v['result'] == 'passed' for v in receipt['variants']) else 1)
