"""Index a Mu-Silicium checkout: which devices have a UEFI port, on which silicon package, and which SoC that is.

    python3 mu_silicium_index.py <Mu-Silicium checkout> -o ../socs/mu-silicium.json

The boot chain of this port needs a UEFI for the phone (BootShim -> UEFI FD -> loader.efi). Mu-Silicium
(Project Silicium) is where those come from; a phone whose SoC has a silicon package there is a realistic
target, one whose SoC has none needs a UEFI port first (ARCHITECTURES.md). Per silicon package the SoC part
number is read from the package's own files (the most frequent SMxxxx/SDMxxx/SCxxxx/MTxxxx/S5Exxxx/... mention),
per device the package its .dsc/.fdf includes.
"""
import argparse
import collections
import json
import pathlib
import re
import sys

PART = re.compile(r'\b(SM\d{4}|SDM\d{3}|SC\d{4}[A-Z]*|QCM\d{4}|MSM\d{4}|MT\d{4}[A-Z]?|S5E\d{4}|T210B01|T210)\b')


def soc_of(pkg: pathlib.Path):
    count = collections.Counter()
    for f in pkg.rglob('*'):
        if f.is_file() and f.suffix.lower() in ('.dsc', '.dec', '.fdf', '.inc', '.dsc.inc', '.fdf.inc', '.h', '.md',
                                                '.c', '.cfg', '.inf', '.txt', '.py', '.toml'):
            try:
                count.update(m.upper() for m in PART.findall(f.read_text(errors='replace')))
            except OSError:
                pass
    return count.most_common(3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mu', type=pathlib.Path)
    ap.add_argument('-o', '--out', type=pathlib.Path, required=True)
    a = ap.parse_args()
    silicon = {}
    for vendor in sorted((a.mu / 'Silicon').iterdir()):
        if not vendor.is_dir():
            continue
        for pkg in sorted(vendor.iterdir()):
            if pkg.is_dir() and pkg.name.endswith('Pkg') and pkg.name not in (
                    'QcomPkg', 'MediaTekPkg', 'NvidiaPkg', 'SiliciumPkg', 'SamsungPkg', 'SamsungNullPkg',
                    'SiliciumNullPkg', 'OpensslPkg'):
                top = soc_of(pkg)
                silicon[pkg.name] = {'vendor': vendor.name, 'soc_guess': top[0][0].lower() if top else None,
                                     'mentions': top}
    devices = []
    for vendor in sorted((a.mu / 'Platforms').iterdir()):
        if not vendor.is_dir():
            continue
        for dev in sorted(vendor.iterdir()):
            if not (dev.is_dir() and dev.name.endswith('Pkg')):
                continue
            text = ''
            for f in list(dev.glob('*.dsc')) + list(dev.glob('*.fdf')) + list(dev.glob('*.toml')):
                text += f.read_text(errors='replace')
            # devices include their SoC package's settings: `!include KonaPkg/KonaPkg.dsc.inc` (or Silicon/<V>/<X>Pkg)
            pkgs = sorted(set(re.findall(r'(?:!include\s+|Silicon/\w+/)(\w+Pkg)/', text)) & set(silicon))
            devices.append({'brand': vendor.name, 'device': dev.name[:-3], 'silicon': pkgs,
                            'soc': silicon[pkgs[0]]['soc_guess'] if pkgs else None})
    by_soc = collections.defaultdict(list)
    for d in devices:
        if d['soc']:
            by_soc[d['soc']].append(f'{d["brand"]}/{d["device"]}')
    out = {'_generated_by': 'toolkit/tools/mu_silicium_index.py', '_about': 'which SoCs/devices have a Mu-Silicium UEFI',
           'silicon': silicon, 'devices': devices, 'devices_by_soc': dict(sorted(by_soc.items()))}
    a.out.write_text(json.dumps(out, indent=1) + '\n')
    print(f'{len(silicon)} silicon packages, {len(devices)} devices, {len(by_soc)} SoCs with devices')
    for pkg, s in silicon.items():
        print(f'  {s["vendor"]:9} {pkg:14} -> {s["soc_guess"]}   {s["mentions"]}')


if __name__ == '__main__':
    sys.exit(main())
