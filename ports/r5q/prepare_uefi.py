"""Create the R5Q board package from measured Samsung firmware configuration."""
import csv
import json
import pathlib
import re
import shutil
import sys
import uuid

mu = pathlib.Path(sys.argv[1]).resolve(strict=True)
configuration = pathlib.Path(sys.argv[2]).read_text(encoding="ascii")
reference = mu / "Platforms/Xiaomi/cepheusPkg"
target = mu / "Platforms/Samsung/r5qPkg"
if target.exists():
    raise SystemExit("The R5Q package already exists. Review it before regenerating.")
if not (reference / "cepheus.dsc").is_file():
    raise SystemExit("The pinned SM8150 reference package is missing")

regions = []
parameters = []
section = ""
for line in configuration.splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    if line.startswith("[") and line.endswith("]"):
        section = line[1:-1]
        continue
    if section in {"MemoryMap", "RegisterMap"}:
        values = next(csv.reader([line], skipinitialspace=True))
        if len(values) != 8:
            raise ValueError(f"Unexpected memory entry {line}")
        base, size = int(values[0], 0), int(values[1], 0)
        name = values[2].strip()
        attributes = [value.strip() for value in values[3:]]
        if base < 0 or size <= 0 or base + size > 1 << 64:
            raise ValueError("Invalid memory extent")
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) for value in attributes):
            raise ValueError("Unexpected memory attribute")
        regions.append((name, base, size, attributes))
    elif section == "ConfigParameters" and "=" in line:
        name, value = (part.strip() for part in line.split("=", 1))
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            try:
                parameters.append((name, int(value, 0)))
            except ValueError:
                pass  # String properties are supplied through the platform PCDs.

expected_fd = ("UEFI FD", 0x9FC00000, 0x300000)
if not any(region[:3] == expected_fd for region in regions):
    raise SystemExit("The firmware has an unexpected UEFI FD reservation")
if not any(region[:3] == ("Display Reserved", 0x9C400000, 0x2000000) for region in regions):
    raise SystemExit("The framebuffer reservation does not match the measured board")

# The live Samsung FDT reserves these in the otherwise unlisted gap before MLVM.
regions.extend([
    ("Samsung TIMA", 0xB0000000, 0x200000, ["AddMem", "MEM_RES", "SYS_MEM_CAP", "Reserv", "WRITE_BACK_XN"]),
    ("Samsung RKP", 0xB0200000, 0x200000, ["AddMem", "MEM_RES", "SYS_MEM_CAP", "Reserv", "WRITE_BACK_XN"]),
])
if len(regions) > 255 or len(parameters) > 255:
    raise SystemExit("The platform tables exceed their count fields")

shutil.copytree(reference, target)
for path in list(target.rglob("*")):
    if path.is_file():
        text = path.read_text(encoding="utf-8")
        text = text.replace("cepheus", "r5q").replace("Platforms/Xiaomi", "Platforms/Samsung")
        text = text.replace('"Xiaomi"', '"Samsung"').replace('"Mi 9"', '"Galaxy S10 Lite"')
        text = text.replace('"Mi_9_r5q"', '"SM-G770F"')
        path.write_text(text, encoding="utf-8", newline="\n")
        if "cepheus" in path.name:
            path.rename(path.with_name(path.name.replace("cepheus", "r5q")))

dsc = target / "r5q.dsc"
text = dsc.read_text()
text = text.replace("20DA5DDD-A8A7-4C83-9853-7716FE7BCDD6", str(uuid.uuid4()).upper())
text = text.replace("PcdFrameBufferHeight|2340", "PcdFrameBufferHeight|2400")
text = text.replace("PcdCPUCoresStackBase|0x9FFB0000", "PcdCPUCoresStackBase|0x9FF90000")
text = text.replace("  r5q/AcpiTables.inf\n", "")
dsc.write_text(text, newline="\n")

memory_lines = ["#include <Library/MemoryMapLib.h>", "", "// Derived from this R5Q's uefiplat.cfg and live FDT.",
                "STATIC EFI_MEMORY_REGION_DESCRIPTOR gMemoryDescriptor[] = {"]
for name, base, size, attributes in regions:
    memory_lines.append(f"  {{{json.dumps(name)}, 0x{base:08X}, 0x{size:08X}, " + ", ".join(attributes) + "},")
memory_lines.extend(["};", "", "VOID GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count)",
                     "{", "  *Map = gMemoryDescriptor;", "  *Count = ARRAY_SIZE (gMemoryDescriptor);", "}", ""])
(target / "Library/MemoryMapLib/MemoryMapLib.c").write_text("\n".join(memory_lines))

config_lines = ["#include <Library/ConfigurationMapLib.h>", "", "STATIC EFI_CONFIGURATION_ENTRY_DESCRIPTOR gConfigurationDescriptor[] = {"]
for name, value in parameters:
    config_lines.append(f"  {{{json.dumps(name)}, 0x{value:X}}},")
config_lines.extend(["};", "", "VOID GetConfigurationMap (OUT EFI_CONFIGURATION_ENTRY_DESCRIPTOR **Map, OUT UINT8 *Count)",
                     "{", "  *Map = gConfigurationDescriptor;", "  *Count = ARRAY_SIZE (gConfigurationDescriptor);", "}", ""])
(target / "Library/ConfigurationMapLib/ConfigurationMapLib.c").write_text("\n".join(config_lines))

config = (mu / "Resources/Configs/cepheus.toml").read_text().replace("header_version = 1", "header_version = 2")
(mu / "Resources/Configs/r5q.toml").write_text(config)
print(f"Created {target} with {len(regions)} regions and {len(parameters)} numeric firmware parameters")
print("The board package still requires its own extracted firmware drivers and a build/boot test.")
