"""Match board-owned XBL modules to the SM8150 reference's INF descriptions."""
import hashlib
import json
import pathlib
import re
import shutil
import struct
import sys
import uuid

dump = pathlib.Path(sys.argv[1]).resolve(strict=True)
mu = pathlib.Path(sys.argv[2]).resolve(strict=True)
reference = mu / "Binaries/cepheus"
destination = mu / "Binaries/r5q"
if destination.exists():
    raise SystemExit("The board firmware directory already exists. Review it before restaging.")

modules = {}
for header_path in dump.rglob("header.bin"):
    header = header_path.read_bytes()
    if len(header) < 24 or header[18] not in {3, 4, 5, 6, 7, 8, 9}:
        continue
    directory = header_path.parent
    info = (directory / "info.txt").read_text(errors="replace")
    if not re.search(r"^Type:\s*File\s*$", info, re.MULTILINE):
        continue
    guid = str(uuid.UUID(bytes_le=header[:16])).upper()
    modules[guid] = directory

staged = []
missing = []
for inf in reference.rglob("*.inf"):
    text = inf.read_text()
    match = re.search(r"^\s*FILE_GUID\s*=\s*([0-9a-fA-F-]{36})", text, re.MULTILINE)
    if not match:
        continue
    guid = match.group(1).upper()
    module = modules.get(guid)
    relative = inf.relative_to(reference)
    if module is None:
        missing.append(str(relative))
        continue
    files = re.findall(r"^\s*(PE32|TE|DXE_DEPEX)\|([^|\r\n]+)\|", text, re.MULTILINE)
    payloads = []
    for kind, filename in files:
        if pathlib.PurePosixPath(filename).name != filename:
            raise ValueError("Unexpected nested binary filename")
        section_name = {"PE32": "PE32 image section", "TE": "TE image section",
                        "DXE_DEPEX": "DXE dependency section"}[kind]
        sections = [path for path in module.rglob("body.bin") if path.parent.name.endswith(section_name)]
        if len(sections) != 1:
            raise ValueError(f"Expected one {kind} section for {guid}, found {len(sections)}")
        payload = sections[0].read_bytes()
        if kind == "PE32":
            if payload[:2] != b"MZ":
                raise ValueError("Unexpected executable header")
            offset = struct.unpack_from("<I", payload, 0x3C)[0]
            if struct.unpack_from("<H", payload, offset + 4)[0] != 0xAA64:
                raise ValueError("Unexpected firmware architecture")
        payloads.append((filename, payload))
    output = destination / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    for filename, payload in payloads:
        (output.parent / filename).write_bytes(payload)
    staged.append({"inf": str(relative), "guid": guid,
                   "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in payloads}})

manifest = {"source": "This phone's own XBL", "modules": staged, "unmatched_reference_modules": missing,
            "binary_patches_applied": False}
destination.mkdir(parents=True, exist_ok=True)
(destination / "extraction-manifest.json").write_text(json.dumps(manifest, indent=2))
print(f"Staged {len(staged)} matching board-owned modules")
print("Reference modules absent from this board:")
for name in missing:
    print(name)
print("Required firmware compatibility patches still need review before boot testing.")
