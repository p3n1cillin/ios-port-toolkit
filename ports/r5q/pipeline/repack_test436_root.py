"""Compile the updated media shim and validate it inside the Test 436 root volume."""

import base64
import hashlib
import json
from pathlib import Path
import subprocess


WORK = Path("/var/tmp/codex-ios-r5q/work")
BUILD = Path(__file__).resolve().parents[1] / "port/build"
SOURCE = BUILD / "test436-root.apfs.new"
DEST = BUILD / "test436-root-verified.apfs"

script = r"""
set -eu
cp -a /donor/userland/root /scratch/root
clang -target arm64-apple-ios16.5 -isysroot /sdk -fuse-ld=lld \
    -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 -dynamiclib \
    -install_name /redwood/lib/libredwood-media.dylib \
    -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
    -framework VideoToolbox -framework IOSurface -lobjc -Wl,-undefined,dynamic_lookup \
    -I/openh264/src/codec/api/wels -o /scratch/libredwood-media.dylib \
    /donor/userland/src/inject_media.c /donor/userland/src/camera_device.c \
    /donor/userland/src/camera_isp.c /donor/userland/src/vt_h264_enc.c \
    /openh264/libopenh264_enc.a -lc++
install -m 755 /scratch/libredwood-media.dylib /scratch/root/lib/libredwood-media.dylib
apfs=/donor/tools/apfs
python3 "$apfs/mkapfs.py" -z --plain fw -o /scratch/root.apfs redwood=/scratch/root >&2
python3 - /input-root.apfs /scratch/root.apfs <<'PY' >&2
import hashlib, sys
sys.path.insert(0, '/donor/tools/apfs')
from apfs import Container, pick_volume

def media_files(image):
    container = Container(image)
    try:
        volume = pick_volume(container, None)
        result = {}
        for pathname in ('lib/libredwood-media.dylib', 'lib/RedwoodAudio.driver/RedwoodAudio'):
            oid, _ = volume.lookup(pathname)
            result[pathname] = hashlib.sha256(volume.inode(oid).contents()).hexdigest()
        return result
    finally:
        container.close()

old, new = map(media_files, sys.argv[1:])
for pathname, digest in new.items():
    with open('/scratch/root/' + pathname, 'rb') as source:
        assert hashlib.sha256(source.read()).hexdigest() == digest, pathname
assert old['lib/RedwoodAudio.driver/RedwoodAudio'] == new['lib/RedwoodAudio.driver/RedwoodAudio']
assert old['lib/libredwood-media.dylib'] != new['lib/libredwood-media.dylib']
for pathname, digest in new.items():
    print(f'COMPONENT {digest} {pathname}')
PY
base64 -w0 /scratch/root.apfs
"""

command = [
    "/usr/bin/prlimit", "--as=4294967296", "--cpu=120", "--nproc=1024",
    "--nofile=128", "--fsize=134217728", "--", "/usr/bin/bwrap",
    "--unshare-all", "--die-with-parent", "--new-session", "--cap-drop", "ALL",
]
for directory in ("/usr", "/bin", "/lib", "/lib64", "/etc/alternatives"):
    command += ["--ro-bind", directory, directory]
for source, target in (
    (WORK / "linux-source", "/donor"),
    (WORK / "sdks-source/iPhoneOS16.5.sdk", "/sdk"),
    (WORK / "openh264-ios", "/openh264"),
    (SOURCE, "/input-root.apfs"),
):
    command += ["--ro-bind", str(source), target]
command += [
    "--proc", "/proc", "--dev", "/dev", "--size", "536870912", "--tmpfs", "/scratch",
    "--clearenv", "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "HOME", "/scratch",
    "--setenv", "TMPDIR", "/scratch", "--setenv", "PYTHONDONTWRITEBYTECODE", "1",
    "--chdir", "/scratch", "/usr/bin/timeout", "120", "/bin/sh", "-c", script,
]
result = subprocess.run(command, env={"PATH": "/usr/bin:/bin"}, capture_output=True, timeout=135)
log = result.stderr.decode(errors="replace")
DEST.with_suffix(".build.log").write_text(log, encoding="utf-8")
if result.returncode:
    raise SystemExit(log[-4000:])
output = base64.b64decode(result.stdout, validate=True)
assert output[32:36] == b"NXSB"
DEST.write_bytes(output)
components = {}
for line in log.splitlines():
    if line.startswith("COMPONENT "):
        _, sha256, path = line.split()
        components[path] = sha256
assert len(components) == 2
report = {
    "image": DEST.name,
    "sha256": hashlib.sha256(output).hexdigest(),
    "bytes": len(output),
    "apfs_component_sha256": components,
}
DEST.with_suffix(".receipt.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, indent=2))
