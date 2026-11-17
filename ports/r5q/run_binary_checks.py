"""Run emulated ARM64 binary checks in a no-network, resource-limited namespace."""
import hashlib
import re
import json
import pathlib
import subprocess
import sys

def run_checks(root, names):
    root = pathlib.Path(root).resolve(strict=True)
    venv = pathlib.Path('/var/tmp/codex-ios-r5q/work/mu-venv')
    tests = list(names)
    for name in tests:
        match = re.fullmatch(r'test_test(\d+)_(shim|prepare|roundtrip)\.py', name)
        if not match or (root / name).resolve(strict=True).parent != root:
            raise ValueError('Unexpected binary test name')
        command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=120', '--nproc=1024',
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
        number,mode=match.groups()
        suffixes=['shim.bin','uefi.fd','payload.bin'] if mode=='shim' else ['loader.efi','payload.bin']
        if mode == 'shim' and (root/'port/build'/f'r5q-test{number}-shim-symbols.txt').exists():
            suffixes.append('shim-symbols.txt')
        data['input_sha256']={}
        for suffix in suffixes:
            filename=f'r5q-test{number}-{suffix}'
            data['input_sha256'][filename]=hashlib.sha256((root/'port/build'/filename).read_bytes()).hexdigest()
        (root / 'port/build' / (name.removesuffix('.py') + '.json')).write_text(json.dumps(data, indent=2) + '\n')
        print(json.dumps({'test':name,'result':data.get('result','passed'),'cases':len(data.get('cases',data.get('tests',[]))),'input_sha256':data['input_sha256']},indent=2))

if __name__ == "__main__":
    run_checks(sys.argv[1],sys.argv[2:])
