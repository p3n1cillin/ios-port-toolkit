"""Compile the proposed board loader in an isolated, bounded Linux namespace."""
import base64
import hashlib
import pathlib
import struct
import subprocess
import sys

source = pathlib.Path(sys.argv[1]).resolve(strict=True)
destination = pathlib.Path(sys.argv[2]).resolve()
options = {(): 'build-sandbox.sh', ('--first-test',): 'build-first-test.sh', ('--test5',): 'build-test5.sh', ('--test6',): 'build-test6.sh', ('--test7',): 'build-test7.sh', ('--test8',): 'build-test8.sh', ('--test12',): 'build-test12.sh', ('--test13',): 'build-test13.sh', ('--test14',): 'build-test14.sh', ('--test15',): 'build-test15.sh', ('--test16',): 'build-test16.sh', ('--window-harness',): 'build-window-harness.sh', ('--qemu',): 'build-qemu.sh', ('--test18',): 'build-test18.sh', ('--test29',): 'build-test29.sh', ('--test30',): 'build-test30.sh', ('--qemu-abl',): 'build-qemu-abl.sh', ('--test31',): 'build-test31.sh', ('--test32',): 'build-test32.sh', ('--test33',): 'build-test33.sh', ('--test34',): 'build-test34.sh', ('--test35',): 'build-test35.sh', ('--test36',): 'build-test36.sh', ('--test37',): 'build-test37.sh', ('--test38',): 'build-test38.sh', ('--test39',): 'build-test39.sh', ('--test40',): 'build-test40.sh', ('--test41',): 'build-test41.sh', ('--test42',): 'build-test42.sh', ('--test43',): 'build-test43.sh', ('--test44',): 'build-test44.sh', ('--test45',): 'build-test45.sh', ('--test46',): 'build-test46.sh', ('--test47',): 'build-test47.sh', ('--test48',): 'build-test48.sh', ('--test49',): 'build-test49.sh', ('--test49-twin',): 'build-test49-twin.sh', ('--test50',): 'build-test50.sh', ('--test50-twin',): 'build-test50-twin.sh', ('--test51',): 'build-test51.sh', ('--test52',): 'build-test52.sh', ('--test53',): 'build-test53.sh', ('--test54',): 'build-test54.sh', ('--test55',): 'build-test55.sh', ('--test56',): 'build-test56.sh', ('--test57',): 'build-test57.sh', ('--test58',): 'build-test58.sh', ('--test59',): 'build-test59.sh', ('--test60',): 'build-test60.sh', ('--test61',): 'build-test61.sh', ('--test62',): 'build-test62.sh', ('--test63',): 'build-test63.sh', ('--test64',): 'build-test64.sh', ('--test65',): 'build-test65.sh', ('--test66',): 'build-test66.sh', ('--test67',): 'build-test67.sh', ('--test68',): 'build-test68.sh', ('--test69',): 'build-test69.sh', ('--test70',): 'build-test70.sh', ('--test71',): 'build-test71.sh', ('--test72',): 'build-test72.sh', ('--test73',): 'build-test73.sh', ('--test74',): 'build-test74.sh', ('--test75',): 'build-test75.sh', ('--test76',): 'build-test76.sh', ('--test77',): 'build-test77.sh', ('--test78',): 'build-test78.sh', ('--test79',): 'build-test79.sh', ('--test80',): 'build-test80.sh', ('--test81',): 'build-test81.sh', ('--test82',): 'build-test82.sh', ('--test83',): 'build-test83.sh', ('--test84',): 'build-test84.sh', ('--test85',): 'build-test85.sh', ('--test86',): 'build-test86.sh', ('--test87',): 'build-test87.sh', ('--test88',): 'build-test88.sh', ('--test89',): 'build-test89.sh', ('--test90',): 'build-test90.sh', ('--test92',): 'build-test92.sh', ('--test93',): 'build-test93.sh', ('--test94',): 'build-test94.sh', ('--test95',): 'build-test95.sh', ('--test96',): 'build-test96.sh', ('--test97',): 'build-test97.sh', ('--test98',): 'build-test98.sh', ('--test99',): 'build-test99.sh', ('--test100',): 'build-test100.sh', ('--test101',): 'build-test101.sh', ('--test102',): 'build-test102.sh', ('--test103',): 'build-test103.sh', ('--test104',): 'build-test104.sh', ('--test105',): 'build-test105.sh', ('--test106',): 'build-test106.sh', ('--test107',): 'build-test107.sh', ('--test108',): 'build-test108.sh', ('--test109',): 'build-test109.sh', ('--test110',): 'build-test110.sh', ('--twin-ios',): 'build-twin-ios.sh', ('--twin-phone',): 'build-twin-phone.sh'}
emu_checkpoint = None
stage_env = []
if sys.argv[3:4] == ['--stage-test']:
    # Tests 19-23: return-path stage bisection with a PS_HOLD self-reset (build-stage-test.sh).
    number, stage = int(sys.argv[4]), int(sys.argv[5])
    assert 19 <= number <= 29 and 0 <= stage <= 5 and len(sys.argv) == 6
    build_script = 'build-stage-test.sh'
    stage_env = ['--setenv', 'R5Q_N', str(number), '--setenv', 'R5Q_STAGE', str(stage)]
elif sys.argv[3:4] == ['--emu-checkpoint']:
    # Emulation-only Test 15 profile with a later kernel checkpoint (see run_source_kernel_emulation.py).
    emu_checkpoint = int(sys.argv[4])
    assert 1 <= emu_checkpoint <= 99 and len(sys.argv) == 5
    build_script = 'build-emu-checkpoint.sh'
else:
    build_script = options[tuple(sys.argv[3:])]
command = [
    "/usr/bin/prlimit", "--as=2147483648", "--cpu=120", "--nproc=256",
    "--nofile=128", "--fsize=33554432", "--",
    "/usr/bin/bwrap", "--unshare-all", "--die-with-parent", "--new-session",
    "--cap-drop", "ALL", "--ro-bind", "/usr", "/usr", "--ro-bind", "/bin", "/bin",
    "--ro-bind", "/lib", "/lib", "--ro-bind", "/lib64", "/lib64",
    "--proc", "/proc", "--dev", "/dev",
    "--size", "8388608", "--tmpfs", "/tmp",
    "--size", "67108864", "--tmpfs", "/scratch",
    "--ro-bind", str(source), "/src", "--chdir", "/scratch",
    "--clearenv", "--setenv", "PATH", "/usr/bin:/bin",
    "--setenv", "HOME", "/scratch", "--setenv", "TMPDIR", "/scratch",
    "--setenv", "LC_ALL", "C",
    "/usr/bin/timeout", "120", "/bin/sh", "/src/" + build_script,
]
if stage_env:
    at = command.index('--clearenv') + 1
    command[at:at] = stage_env
if emu_checkpoint is not None:
    at = command.index('--clearenv') + 1
    command[at:at] = ['--setenv', 'R5Q_CP', str(emu_checkpoint)]
if sys.argv[3:] == ['--window-harness']:
    at = command.index('--chdir')
    command[at:at] = ['--ro-bind', '/var/tmp/codex-ios-r5q/work/Mu-Silicium/Mu_Basecore/MdePkg/Include', '/mde']
result = subprocess.run(command, env={"PATH": "/usr/bin:/bin"},
                        capture_output=True, timeout=135, check=False)
if result.returncode or b'terminate called after throwing' in result.stderr or b'LLVM ERROR' in result.stderr:
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(result.returncode or 1)
binary = base64.b64decode(result.stdout, validate=True)
if len(binary) > 1048576 or binary[:2] != b"MZ":
    raise SystemExit("The compiler did not produce the expected small PE image")
pe_offset = struct.unpack_from("<I", binary, 0x3c)[0]
if binary[pe_offset:pe_offset + 4] != b"PE\0\0":
    raise SystemExit("Invalid PE signature")
if struct.unpack_from("<H", binary, pe_offset + 4)[0] != 0xAA64:
    raise SystemExit("Unexpected PE architecture")
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(binary)
print(f"Compiled AArch64 EFI loader, {len(binary)} bytes")
print(f"SHA-256 {hashlib.sha256(binary).hexdigest()}")
if result.stderr:
    sys.stderr.buffer.write(result.stderr)
