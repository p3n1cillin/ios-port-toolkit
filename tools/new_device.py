"""Start a profile for a new phone: every field of the format, filled from its live device tree where possible.

    python new_device.py <codename> --dts live.dts [--soc sm8250] [--model SM-XXXX] [--conf <donor>/common/devices/<codename>.conf]

Writes toolkit/devices/<codename>.json (refuses to overwrite) with:
  - every field profile_spec.FIELDS knows, null where unknown
  - what extract_profile.py found in the tree (with its evidence in <codename>.evidence.json)
  - the safety defaults (recovery only, never_write list, min battery)
and <codename>.local.json for the private values (serial, paths). Then run validate_profile.py on it:
the checklist is the order in which to gather the rest (README, "What to gather").
"""
import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import extract_profile  # noqa: E402
import dtslib  # noqa: E402
import profile_spec as spec  # noqa: E402

SAFE_DEFAULTS = {
    'flash.test_partition': 'recovery',
    'flash.never_write': ['boot', 'vbmeta', 'vbmeta_samsung', 'dtbo', 'xbl', 'xbl_config', 'abl', 'aop', 'tz', 'hyp',
                          'keymaster', 'param', 'misc', 'efs', 'sec_efs', 'persist', 'userdata', 'super'],
    'flash.enter_test': 'reboot-recovery helper: reboot(2) RESTART2 "recovery" (no BCB/misc write)',
    'flash.requires_root': True,
    'flash.min_battery_percent': 30,
    'bootimg.kernel_load_pa': None,
    'log_slot.blocks': 513,
    'memory.payload.max': '0x04000000',
    'display.dsi0_offset': '0x94000',
    'display.intf_offset': '0x6B800',
    'display.pipe_offsets': ['0x05000', '0x07000', '0x09000', '0x0B000', '0x25000', '0x27000', '0x29000', '0x2B000'],
    'display.ios_points': [375, 812],
    'loader.flag_prefix': 'R5Q_',
}


def put(d, path, value):
    parts = path.split('.')
    for p in parts[:-1]:
        d = d.setdefault(p, {})
    d.setdefault(parts[-1], value)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('codename')
    ap.add_argument('--dts', type=pathlib.Path, required=True)
    ap.add_argument('--soc')
    ap.add_argument('--model')
    ap.add_argument('--conf', type=pathlib.Path, help="the donor's common/devices/<codename>.conf, if it has one")
    ap.add_argument('--out-dir', type=pathlib.Path, default=HERE.parent / 'devices')
    a = ap.parse_args()
    out = a.out_dir / f'{a.codename}.json'
    if out.exists():
        raise SystemExit(f'{out} exists: edit it, or remove it first')
    tree = dtslib.load(a.dts)
    soc = a.soc or extract_profile.detect_soc(tree)
    _, socdata = extract_profile.soc_data(soc, HERE.parent / 'socs')
    found = extract_profile.extract(tree, socdata, soc)
    prof = {'schema': 'iosport-device/1',
            '_about': f'{a.codename}: started by tools/new_device.py from {a.dts.name}. Values from the device tree are '
                      'CANDIDATES (evidence in the .evidence.json next to this file); check each before a driver uses it.'}
    prof.update(found.profile)
    if a.conf:
        # the donor's profile for this phone (common/devices/<codename>.conf): its values win over the tree's guesses
        import conf_bridge
        donor = conf_bridge.import_conf(a.conf.read_text(errors='replace'), a.codename)
        donor.pop('_about', None)
        donor.pop('schema', None)
        prof = spec.merge(prof, donor)
        prof['_about'] += f' Donor profile {a.conf.name} merged on top.'
    put(prof, 'device.codename', a.codename)
    put(prof, 'device.prefix', a.codename)
    put(prof, 'device.loader_platform', a.codename)
    if a.model:
        prof['device']['model'] = a.model
    if soc:
        prof['device']['soc'] = soc
    for path, value in SAFE_DEFAULTS.items():
        put(prof, path, value)
    for path, *_ in spec.FIELDS:
        put(prof, path, None)
    put(prof, 'log_slot.magic', (a.codename.upper() + 'LOGSLOT/V1')[:15])
    out.write_text(json.dumps(prof, indent=1) + '\n', newline='\n')
    (a.out_dir / f'{a.codename}.evidence.json').write_text(
        json.dumps({'evidence': found.evidence, 'notes': found.notes}, indent=1) + '\n', newline='\n')
    local = a.out_dir / f'{a.codename}.local.json'
    if not local.exists():
        local.write_text(json.dumps({'_about': 'PRIVATE: never share', 'local': {
            'serial': None, 'work_win': None, 'work_wsl': None, 'bundles_win': None,
            'backups_dir': 'backups/<serial>'}}, indent=1) + '\n', newline='\n')
    print(f'wrote {out}, {a.codename}.evidence.json, {local.name}')
    for n in found.notes:
        print('note:', n)
    print(f'next: python {HERE / "validate_profile.py"} {out}')


if __name__ == '__main__':
    main()
