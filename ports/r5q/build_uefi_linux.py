"""Build phone UEFI with no host credentials, network, or writable source."""
import base64
import hashlib
import pathlib
import subprocess
import sys

mu = pathlib.Path('/var/tmp/codex-ios-r5q/work/Mu-Silicium')
venv = mu.parent / 'mu-venv'
scripts = pathlib.Path(sys.argv[1]).resolve(strict=True)
destination = pathlib.Path(sys.argv[2]).resolve()
board = {('--first-test',): 'r5qtest', ('--full',): 'r5qfull', ('--full-debug',): 'r5qfull'}.get(tuple(sys.argv[3:]), 'r5q')   # --full: r5qfullPkg
target = 'DEBUG' if sys.argv[3:] == ['--full-debug'] else 'RELEASE'   # DEBUG: messages to the panel + RAM ring
command = ['/usr/bin/prlimit', '--as=8589934592', '--cpu=600', '--nproc=2048',
           '--nofile=512', '--fsize=268435456', '--', '/usr/bin/bwrap',
           '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64', '/etc/alternatives']:
    command += ['--ro-bind', path, path]
command += ['--proc', '/proc', '--dev', '/dev', '--size', '4294967296', '--tmpfs', '/scratch',
            '--ro-bind', str(mu), '/source', '--ro-bind', str(venv), '/venv',
            '--ro-bind', str(scripts), '/scripts', '--chdir', '/scratch', '--clearenv']
for key, value in {'PATH': '/venv/bin:/usr/bin:/bin', 'HOME': '/scratch/home',
                   'TMPDIR': '/scratch/tmp', 'LC_ALL': 'C',
                   'PYTHONDONTWRITEBYTECODE': '1', 'SOURCE_DATE_EPOCH': '1794690000'}.items():
    command += ['--setenv', key, value]
command += ['--setenv', 'R5Q_BOARD', board]
command += ['--setenv', 'R5Q_TARGET', target]
command += ['/usr/bin/timeout', '600', '/bin/bash', '/scripts/build-uefi.sh']
result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=620)
log_path = destination.with_suffix('.build.log')
log_path.parent.mkdir(parents=True, exist_ok=True)
log_path.write_bytes(result.stderr[-200000:])
sys.stderr.buffer.write(result.stderr[-6000:])
if result.returncode:
    raise SystemExit(result.returncode)
binary = base64.b64decode(result.stdout, validate=True)
if len(binary) != 0x300000 or binary[0x28:0x2c] != b'_FVH':
    raise SystemExit('Unexpected UEFI FD format or size')
destination.write_bytes(binary)
print(f'Built UEFI FD: {len(binary)} bytes, SHA256 {hashlib.sha256(binary).hexdigest()}')
