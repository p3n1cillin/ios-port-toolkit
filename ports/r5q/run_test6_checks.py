"""Run emulated ARM64 binary checks in a no-network, resource-limited namespace."""
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
venv = pathlib.Path('/var/tmp/codex-ios-r5q/work/mu-venv')
tests = ['test_test6_kernel.py']
for name in tests:
    command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=120', '--nproc=128',
               '--nofile=128', '--fsize=1048576', '--', '/usr/bin/bwrap',
               '--unshare-all', '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
    for path in ['/usr', '/bin', '/lib', '/lib64']:
        command += ['--ro-bind', path, path]
    command += ['--proc', '/proc', '--dev', '/dev', '--size', '16777216', '--tmpfs', '/scratch',
                '--ro-bind', str(root / 'port/build'), '/inputs', '--ro-bind', str(venv), '/venv',
                '--ro-bind', str(root / name), '/test.py', '--chdir', '/scratch', '--clearenv',
                '--setenv', 'PATH', '/venv/bin:/usr/bin:/bin', '--setenv', 'HOME', '/scratch',
                '--setenv', 'TMPDIR', '/scratch', '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
                '/usr/bin/timeout', '120', '/venv/bin/python', '/test.py']
    result = subprocess.run(command, env={'PATH': '/usr/bin:/bin'}, capture_output=True, timeout=130)
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    if len(result.stdout) > 65536:
        raise ValueError('Unexpected test output')
    data = json.loads(result.stdout)
    (root / 'port/build' / (name.removesuffix('.py') + '.json')).write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps(data, indent=2))
