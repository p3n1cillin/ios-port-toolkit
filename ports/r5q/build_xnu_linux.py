"""Build the pinned open-source kernel in a no-network, bounded disposable tree."""
import base64
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import time

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
destination = pathlib.Path(sys.argv[2]).resolve()
workspace = pathlib.Path('/var/tmp/codex-ios-r5q/work')
apple = workspace / 'apple-xnu-8796.121.2'
donor = workspace / 'linux-source'
cache = workspace / 'xnu-source-cache'
tools = (workspace / 'theos/toolchain/linux/iphone').resolve(strict=True)
sdk = (workspace / 'theos/sdks/iPhoneOS16.5.sdk').resolve(strict=True)
sources = json.loads((root / 'research/xnu-source-staging.json').read_text())
sdk_input = json.loads((root / 'research/macos-sdk-input.json').read_text())
assert all(row['status'] == 'verified' for row in sources['inputs'])
assert hashlib.sha256((cache / 'MacOSX13.3.sdk.tar.xz').read_bytes()).hexdigest() == sdk_input['sha256']
environment = {'PATH': '/usr/bin:/bin', 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null'}
for source in sources['inputs']:
    actual = subprocess.check_output(['/usr/bin/git', '-C', source['path'], 'rev-parse', 'HEAD'], env=environment).decode().strip()
    assert actual == source['commit'], source['name']
command = ['/usr/bin/prlimit', '--as=8589934592', '--cpu=2400', '--nproc=2048',
           '--nofile=512', '--fsize=268435456', '--', '/usr/bin/bwrap',
           '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64', '/etc/alternatives']:
    command += ['--ro-bind', path, path]
command += ['--proc', '/proc', '--dev', '/dev', '--size', '8589934592', '--tmpfs', '/scratch',
            '--ro-bind', str(apple), '/apple', '--ro-bind', str(donor), '/donor',
            '--ro-bind', str(cache), '/cache', '--ro-bind', str(tools), '/cctools',
            '--ro-bind', str(sdk), '/ios-sdk', '--ro-bind', str(root/'kernel-build'), '/scripts',
            '--chdir', '/scratch', '--clearenv']
at = command.index('--chdir')
for name in ['passwd', 'group', 'nsswitch.conf']:
    command[at:at] = ['--ro-bind', str(root/'kernel-build'/name), '/etc/'+name]
    at += 3
for name, value in {'PATH': '/usr/bin:/bin', 'HOME': '/scratch/home', 'TMPDIR': '/scratch/tmp',
                    'LC_ALL': 'C', 'USER': 'builder', 'LOGNAME': 'builder',
                    'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
                    'SOURCE_DATE_EPOCH': '1794690000'}.items():
    command += ['--setenv', name, value]
command += ['/usr/bin/timeout', '2400', '/bin/bash', '/scripts/build-xnu-sandbox.sh']
start = time.monotonic()
result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=2460)
destination.parent.mkdir(parents=True, exist_ok=True)
log = destination.with_suffix('.build.log')
log.write_bytes(result.stderr[-200000:])
receipt = {'status': 'build did not produce a verified linked kernel', 'sandbox_exit': result.returncode,
           'seconds': round(time.monotonic()-start, 1), 'inputs': sources['inputs'], 'sdk_archive': sdk_input,
           'donor_patch_sha256': hashlib.sha256((donor/'xnu/redwood.patch').read_bytes()).hexdigest(),
           'sandbox': {'network': False, 'address_space_bytes': 8589934592, 'scratch_bytes': 8589934592,
                       'wall_seconds': 2400, 'processes': 2048, 'file_size_bytes': 268435456},
           'hardware_approved': False, 'phone_partitions_written': False}
if not result.returncode:
    data = base64.b64decode(result.stdout, validate=True)
    assert 1024*1024 < len(data) < 64*1024*1024
    magic, cpu, subtype, kind, commands, command_bytes = struct.unpack_from('<6I', data)
    assert (magic, cpu, kind) == (0xfeedfacf, 0x100000c, 2)
    assert 0 < commands < 256 and command_bytes <= len(data)-32
    status = re.search(rb'XNU_MAKE_EXIT=(\d+)', result.stderr)
    assert status
    make_exit = int(status.group(1))
    counts = {name: int(re.search(rb'XNU_%s=(\d+)' % name.upper().encode(), result.stderr).group(1))
              for name in ['unexpected_failures', 'export_list_failures', 'export_complaints']}
    destination.write_bytes(data)
    if make_exit == 0:
        status_text = 'linked kernel; make checks passed'
    elif counts['unexpected_failures'] == 0 and counts['export_list_failures'] > 0:
        # Documented by the donor (xnu/README.md): post-link export-list checks only, not compilation.
        status_text = 'linked kernel; only the documented post-link export-list checks failed'
    else:
        status_text = 'linked kernel; make failures require review'
    receipt.update(status=status_text, failure_counts=counts,
                   make_exit=make_exit, kernel_bytes=len(data), kernel_sha256=hashlib.sha256(data).hexdigest(),
                   return_slot_markers=data.count(b'XNULDRST'))
destination.with_suffix('.receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps({key:value for key,value in receipt.items() if key not in ['inputs','sdk_archive']}, indent=2))
if result.returncode:
    sys.stderr.buffer.write(result.stderr[-6000:])
    raise SystemExit(result.returncode)
