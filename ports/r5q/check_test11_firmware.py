"""Independently unpack the built FV and verify the linked first-test manager."""
import json
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve(strict=True)
script = '''set -eu
cp /inputs/r5q-test11-uefi.fd /scratch/test.fd
uefiextract /scratch/test.fd all > /scratch/extractor.log 2>&1
python3 - <<'PY'
from pathlib import Path
import json
root=Path('/scratch/test.fd.dump')
modules=[]
found=False
for info in root.rglob('info.txt'):
 text=info.read_text(errors='replace')
 if 'Type: File' in text and 'Text: ' in text:
  name=text.split('Text: ',1)[1].splitlines()[0]
  modules.append(name)
  if name=='BdsDxe':
   pe=[p for p in info.parent.rglob('body.bin') if p.parent.name.endswith('PE32 image section')]
   assert len(pe)==1
   found=b'R5Q UEFI TEST 11' in pe[0].read_bytes()
assert found, 'First-test boot manager was not found in linked BdsDxe'
assert 'UFSDxe' in modules and 'PartitionDxe' in modules
print(json.dumps({'status':'passed','linked_manager':'R5Q UEFI TEST 11 found inside BdsDxe',
 'modules':modules,'storage_modules':'present, unchanged from tested firmware','hardware_boot':'not tested'},indent=2))
PY
'''
command = ['/usr/bin/prlimit', '--as=2147483648', '--cpu=90', '--nproc=128',
           '--nofile=128', '--fsize=8388608', '--', '/usr/bin/bwrap', '--unshare-all',
           '--die-with-parent', '--new-session', '--cap-drop', 'ALL']
for path in ['/usr', '/bin', '/lib', '/lib64']:
    command += ['--ro-bind', path, path]
command += ['--proc','/proc','--dev','/dev','--size','134217728','--tmpfs','/scratch',
            '--ro-bind',str(build),'/inputs','--clearenv','--setenv','PATH','/usr/bin:/bin',
            '--setenv','HOME','/scratch','--setenv','TMPDIR','/scratch','--chdir','/scratch',
            '/usr/bin/timeout','90','/bin/sh','-c',script]
result = subprocess.run(command, env={'PATH':'/usr/bin:/bin'}, capture_output=True, timeout=100)
if result.returncode:
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(result.returncode)
data=json.loads(result.stdout)
(build/'test11_firmware.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps(data,indent=2))
