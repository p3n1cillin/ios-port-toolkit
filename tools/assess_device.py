"""How portable is this phone? A feasibility report from its live device tree (and/or profile) and its brand.

    python assess_device.py --dts live.dts --brand xiaomi [--json]
    python assess_device.py --profile ../devices/r5q.json

It answers, from data (not guesses):
  1  the SoC and its vendor; Linux-derived SoC data in socs/ (pins, clocks/pin banks) or an alias
  2  a Mu-Silicium UEFI for that SoC (socs/mu-silicium.json) and which devices already have one: the
     closest package to copy for the boot chain (BootShim -> UEFI FD -> loader.efi)
  3  how the brand enters a test and comes back (brands.json): recovery write+restore, fastboot boot (RAM
     only), or blocked
  4  generic kernel requirements: GICv3 (the donor kernel's interrupt driver is GICv3-only), PSCI CPU bring-up,
     the virtual-timer INTID (19 on SM8150, 27 elsewhere), core layout
  5  what the extractor resolved for touch, battery, buttons, cpufreq, Wi-Fi
and a tier:
  A  the method is proven on this SoC (a device runs iOS natively with this chain)
  B  Qualcomm with a Mu-Silicium UEFI and full SoC data: the S10 Lite path applies step for step
  C  a UEFI exists but SoC data is partial (MediaTek, Exynos/Tensor): expect driver work per peripheral
  D  no UEFI for the SoC: a Mu-Silicium silicon bring-up comes first (a project of its own)
  X  blocked (bootloader cannot be unlocked, or no GICv3/PSCI)
"""
import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
TK = HERE.parent
sys.path.insert(0, str(HERE))
import dtslib  # noqa: E402
import extract_profile  # noqa: E402
import profile_spec  # noqa: E402

# devices where this boot chain runs iOS natively (this project, or the donor it builds on)
PROVEN = {'sm8150': 'Samsung Galaxy S10 Lite (r5q): this project, Tests 1-68',
          'sm8250': 'Samsung Galaxy S20+ (y2q): the donor port',
          'sm7325': 'POCO X5 Pro (redwood): the donor port (HackiOS)'}
ALIAS_UEFI = {'exynos990': 's5e9830', 'exynos9810': 's5e9810', 'exynos9820': 's5e9820', 'exynos7885': 's5e7885',
              'exynos7870': 's5e7870', 'exynos2400': 's5e9945', 'sc7280': 'sm7325', 'sc7180': 'sm7125'}


def load_json(p):
    return json.loads(p.read_text()) if p.exists() else {}


def assess(tree=None, profile=None, brand=None):
    r = {'checks': [], 'blockers': [], 'tier': None}
    ok = lambda name, good, detail: r['checks'].append({'check': name, 'ok': good, 'detail': detail})
    cand = {}
    notes = []
    soc = None
    if tree is not None:
        soc = extract_profile.detect_soc(tree)
        data_soc, socdata = extract_profile.soc_data(soc, TK / 'socs')
        out = extract_profile.extract(tree, socdata, soc)
        cand, notes = out.profile, out.notes
    if profile:
        soc = soc or profile_spec.get(profile, 'device.soc')
        brand = brand or profile_spec.get(profile, 'device.brand')
        cand = profile_spec.merge(cand, profile)
    data_soc, socdata = extract_profile.soc_data(soc, TK / 'socs')
    vendor = (socdata or {}).get('vendor', 'qcom' if soc and soc.startswith(('sm', 'sdm', 'sc', 'msm', 'qcm')) else None)
    r['soc'], r['vendor'] = soc, vendor
    ok('SoC data (Linux-derived)', bool(socdata),
       f'socs/{data_soc}.json' + (f' (alias of {soc})' if data_soc and data_soc != soc else '') if socdata
       else f'none for {soc}: tools/qcom_socdata.py or tools/soc_other.py on a Linux tree that has it')

    mu = load_json(TK / 'socs' / 'mu-silicium.json')
    by_soc = mu.get('devices_by_soc', {})
    keys = [k for k in {soc, data_soc, ALIAS_UEFI.get(soc), ALIAS_UEFI.get(data_soc)} if k]
    uefi = sorted({d for k in keys for d in by_soc.get(k, [])})
    ok('Mu-Silicium UEFI for this SoC', bool(uefi), ', '.join(uefi[:8]) + (' ...' if len(uefi) > 8 else '')
       if uefi else 'none: the boot chain needs a UEFI port (Mu-Silicium silicon package) first')

    brands = load_json(TK / 'brands.json')
    b = brands.get((brand or '').lower())
    if b:
        ok(f'brand {brand}: entry method', b['entry_method'] != 'blocked',
           f'{b["entry_method"]} (fastboot boot: {b["fastboot_boot"]}); unlock: {b["unlock"]} '
           f'{"Unlocking wipes the phone: the owner decides (SAFETY R7)." if b.get("unlock_wipes_data") else ""}')
        if b['entry_method'] == 'blocked':
            r['blockers'].append(f'{brand}: bootloader cannot be unlocked')
    else:
        ok('brand', False, f'unknown brand {brand!r}: see brands.json' if brand else 'pass --brand')

    gic = cand.get('gic', {})
    if tree is not None:
        g3 = next((n for n in tree.walk() if n.compatible_has('arm,gic-v3')), None)
        g2 = next((n for n in tree.walk() if n.compatible_has('arm,gic-400', 'arm,cortex-a15-gic')), None)
        ok('GICv3 interrupt controller', g3 is not None,
           'arm,gic-v3' if g3 else ('GICv2 (gic-400): the donor kernel drives GICv3 only: a GICv2 driver is needed'
                                    if g2 else 'not found'))
        if g3 is None and g2 is not None:
            r['blockers'].append('GICv2: needs a GICv2 interrupt driver in the kernel')
        em = cand.get('cpu', {}).get('enable_method')
        ok('PSCI CPU bring-up', em == 'psci', f'enable-method {em!r}' + ('' if em == 'psci' else
                                                                         ': the loader/kernel SMP path assumes PSCI'))
    vt = gic.get('vtimer_intid')
    if vt is not None:
        ok('virtual timer INTID', True, f'{vt}' + ('' if vt == 27 else f': pass gic_vtimer={vt} (XNU assumes 27)'))
    for part, path in [('touch', 'touch.compatible'), ('touch pins', 'touch.sda_gpio'), ('battery gauge', 'battery.gauge'),
                       ('power key (PON)', 'buttons.pon.peripheral_id'), ('volume up', 'buttons.volume_up.gpio'),
                       ('cpufreq', 'cpufreq.kind'), ('Wi-Fi', 'wifi.kind')]:
        v = profile_spec.get(cand, path)
        if path == 'touch.sda_gpio' and v is None:
            v = profile_spec.get(cand, 'touch.miso_gpio')
        ok(part, v is not None, str(v) if v is not None else 'not found in the tree (merged live tree? see notes)')

    if r['blockers']:
        r['tier'] = 'X'
    elif soc in PROVEN or data_soc in PROVEN:
        r['tier'] = 'A'
        r['proven_by'] = PROVEN.get(soc) or PROVEN.get(data_soc)
    elif uefi and vendor == 'qcom' and socdata and socdata.get('gcc'):
        r['tier'] = 'B'
    elif uefi:
        r['tier'] = 'C'
    else:
        r['tier'] = 'D'
    r['tier_meaning'] = {'A': 'proven on this SoC', 'B': 'Qualcomm + UEFI + full SoC data: the S10 Lite path applies',
                         'C': 'a UEFI exists; SoC data partial: driver work per peripheral',
                         'D': 'no UEFI for this SoC: a Mu-Silicium silicon port first', 'X': 'blocked'}[r['tier']]
    r['notes'] = notes
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dts', type=pathlib.Path)
    ap.add_argument('--profile', type=pathlib.Path)
    ap.add_argument('--brand')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if not a.dts and not a.profile:
        ap.error('--dts and/or --profile')
    tree = dtslib.load(a.dts) if a.dts else None
    prof = profile_spec.load(a.profile, local=False) if a.profile else None
    r = assess(tree, prof, a.brand)
    if a.json:
        print(json.dumps(r, indent=1))
        return 0
    print(f'SoC {r["soc"]} ({r["vendor"]}): tier {r["tier"]} -- {r["tier_meaning"]}'
          + (f' [{r["proven_by"]}]' if r.get('proven_by') else ''))
    for c in r['checks']:
        print(f'  {"ok " if c["ok"] else "-- "} {c["check"]}: {c["detail"]}')
    for b in r['blockers']:
        print(f'  BLOCKER {b}')
    for n in r['notes']:
        print(f'  note: {n}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
