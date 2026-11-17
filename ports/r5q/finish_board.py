"""Remove unrelated reference peripherals and stage R5Q raw firmware data."""
import pathlib
import re
import sys
import uuid

mu = pathlib.Path(sys.argv[1]).resolve(strict=True)
dump = pathlib.Path(sys.argv[2]).resolve(strict=True)
board = mu / 'Platforms/Samsung/r5qPkg'
for path in [board / 'r5q.dsc', board / 'r5q.fdf', board / 'Include/DXE.inc', board / 'Include/APRIORI.inc']:
    text = path.read_text()
    lines = [line for line in text.splitlines() if not any(token in line for token in
             ('ChargerExDxe.inf', 'QcomChargerDxeLA.inf', 'r5q/AcpiTables.inf'))]
    path.write_text('\n'.join(lines) + '\n')

raw = {}
for header_path in dump.rglob('header.bin'):
    header = header_path.read_bytes()
    if len(header) < 24 or header[18] not in {1, 2}:
        continue
    info = (header_path.parent / 'info.txt').read_text(errors='replace')
    if not re.search(r'^Type:\s*File\s*$', info, re.MULTILINE):
        continue
    bodies = [p for p in header_path.parent.rglob('body.bin') if p.parent.name.endswith('Raw section')]
    if len(bodies) == 1:
        raw[str(uuid.UUID(bytes_le=header[:16])).upper()] = bodies[0]

# The first bootstrap inherits the already active display. It does not contain
# Xiaomi panel data, charger profiles, or battery provisioning blobs.
raw_dir = mu / 'Binaries/r5q/RawFiles'
raw_dir.mkdir(exist_ok=True)
output = []
for guid, name in [('A1E235DE-E825-4591-9623-C43175811826', 'SecParti.cfg'),
                   ('21E9BDD9-6C3F-4F10-84A5-BBEC322741F1', 'uefipil.cfg')]:
    source = raw.get(guid)
    if source is None:
        print(f'Absent stock configuration: {name}')
        continue
    (raw_dir / name).write_bytes(source.read_bytes())
    output.extend([f'FILE FREEFORM = {guid} {{', f'  SECTION UI = "{name}"',
                   f'  SECTION RAW = Binaries/r5q/RawFiles/{name}', '}', ''])
(board / 'Include/RAW.inc').write_text('\n'.join(output))
print('Prepared first-stage R5Q package without reference-phone charger/panel data')
