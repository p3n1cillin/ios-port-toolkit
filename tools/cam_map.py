"""Every camera part a Qualcomm phone's device tree describes, with the bus, pins, address and supplies you need to
reach it from a port's own (bit-banged) I2C -- the map that took the S10 Lite four tests to find by scanning.

    python cam_map.py live.dts            # a table per CCI controller
    python cam_map.py live.dts --json     # the same as JSON (for a profile or a driver generator)

For each `qcom,cci` controller: its masters (master m = the m-th DATA/CLK pair of its `gpios`, e.g. CCI 1 master 1
= GPIO33/34 on SM8150). For each device under it (sensor, actuator, EEPROM, OIS, flash): its 7-bit address
(`slave-addr` is 8-bit in Qualcomm trees: 0x18 -> 0x0c), the CCI controller and master it is on, its GPIOs (by
label), and each supply: the regulator's name, the voltage the device asks for, and the PMIC behind it (an I2C
PMIC's bus and address, or an RPMh resource).

Warnings it prints (each one cost the r5q port at least one test):
  * a part linked to a sensor (actuator-src, ois-src, eeprom-src) on a different controller or master: scanning the
    sensor's bus will not find it (r5q Test 428: the lens motor was on GPIO33/34, the sensor on 17/18);
  * a supply whose regulator node and the device disagree on the voltage, or a regulator on an I2C PMIC (your
    driver must set its voltage register itself: check it with pmic_ldo.py -- r5q Test 427: 0.7 V written where
    1.8 V was meant clipped every dim pixel to black).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dtslib  # noqa: E402

KINDS = (('qcom,cam-sensor', 'sensor'), ('qcom,actuator', 'actuator'), ('qcom,eeprom', 'eeprom'),
         ('qcom,ois', 'ois'), ('qcom,camera-flash', 'flash'), ('qcom,cam-res-mgr', None))
LINKS = ('actuator-src', 'ois-src', 'eeprom-src', 'led-flash-src')


def kind_of(node):
    for comp, kind in KINDS:
        if node.compatible_has(comp):
            return kind
    if node.has('slave-addr') or node.has('cci-master'):
        return node.base.split(',')[-1]
    return None


def gpio_list(tree, node, key='gpios'):
    """[(controller name, pin, flags)] using each controller's #gpio-cells."""
    raw, out, i = node.cells(key), [], 0
    while i < len(raw):
        ctl = tree.phandle(raw[i])
        n = ctl.cell('#gpio-cells', 2) if ctl is not None else 2
        args = raw[i + 1:i + 1 + n]
        out.append((ctl.base if ctl is not None else f'phandle {raw[i]:#x}', args[0] if args else None,
                    args[1] if len(args) > 1 else 0))
        i += 1 + n
    return out


def pmic_of(reg_node):
    """Who makes this regulator: an I2C PMIC (bus node, address, compatible) or an RPMh/SPMI resource."""
    if 'gdsc' in reg_node.base.lower() or 'gdsc' in reg_node.string('regulator-name', '').lower():
        return 'GDSC (a clock controller power domain, not a voltage)'
    n = reg_node
    while n is not None:
        comp = n.strings('compatible')
        if n.has('qcom,resource-name'):
            return f'RPMh {n.string("qcom,resource-name")}'
        if comp and n.parent is not None and n.unit is not None and any('pmic' in c or 'regulator' in c for c in comp):
            bus = n.parent
            bus_kind = 'i2c-gpio' if bus.compatible_has('i2c-gpio') else (bus.strings('compatible') or [bus.base])[0]
            if bus.compatible_has('i2c') or bus.base.startswith('i2c'):
                return f'I2C PMIC {comp[0]} @ {n.unit:#04x} on {bus.base}@{bus.unit if bus.unit is not None else ""} ({bus_kind})'
        if comp and any('spmi' in c for c in comp):
            return 'SPMI PMIC'
        n = n.parent
    return '?'


def supplies(tree, node):
    names = node.strings('regulator-names')
    vmin, vmax = node.cells('rgltr-min-voltage'), node.cells('rgltr-max-voltage')
    out = []
    for i, name in enumerate(names):
        ph = node.cell(f'{name}-supply')
        reg = tree.phandle(ph) if ph is not None else None
        want = (vmin[i] if i < len(vmin) else None, vmax[i] if i < len(vmax) else None)
        entry = {'supply': name, 'want_uv': want[0], 'want_max_uv': want[1]}
        if reg is not None:
            entry['regulator'] = reg.string('regulator-name', reg.base)
            entry['reg_min_uv'] = reg.cell('regulator-min-microvolt')
            entry['reg_max_uv'] = reg.cell('regulator-max-microvolt')
            entry['pmic'] = pmic_of(reg)
        out.append(entry)
    return out


def cci_masters(tree, cci):
    pins = gpio_list(tree, cci)
    labels = cci.strings('gpio-req-tbl-label')
    masters = []
    for m in range(len(pins) // 2):
        sda, scl = pins[2 * m], pins[2 * m + 1]
        lab = labels[2 * m:2 * m + 2] if len(labels) >= 2 * m + 2 else []
        masters.append({'master': m, 'sda': sda[1], 'scl': scl[1], 'controller': sda[0], 'labels': lab})
    return masters


def build(tree):
    ccis = sorted(tree.compatible('qcom,cci'), key=lambda n: n.cell('cell-index', 0))
    by_index = {c.cell('cell-index', i): c for i, c in enumerate(ccis)}
    out, by_phandle = [], {}
    for cci in ccis:
        idx = cci.cell('cell-index', 0)
        entry = {'cci': idx, 'base': (cci.reg() or [(None, None)])[0][0], 'masters': cci_masters(tree, cci),
                 'devices': []}
        for d in cci.children:
            kind = kind_of(d)
            if kind is None or not d.status_ok:
                continue
            on_cci = d.cell('cci-device', idx)
            master = d.cell('cci-master', 0)
            ms = cci_masters(tree, by_index.get(on_cci, cci))
            pins = ms[master] if master < len(ms) else None
            sa = d.cell('slave-addr')
            dev = {'kind': kind, 'node': d.name, 'cell_index': d.cell('cell-index'),
                   'addr7': (sa >> 1) if sa is not None else d.unit, 'slave_addr_8bit': sa, 'cci': on_cci,
                   'master': master, 'sda': pins['sda'] if pins else None, 'scl': pins['scl'] if pins else None,
                   'csiphy': d.cell('csiphy-sd-index'), 'gpios': [], 'supplies': supplies(tree, d), 'links': {}}
            labels = d.strings('gpio-req-tbl-label')
            for i, (ctl, pin, _f) in enumerate(gpio_list(tree, d)):
                dev['gpios'].append({'label': labels[i] if i < len(labels) else f'#{i}', 'pin': pin, 'controller': ctl})
            for link in LINKS:
                ph = d.cell(link)
                if ph is not None:
                    dev['links'][link] = ph
            ph = d.cell('phandle')
            if ph is not None:
                by_phandle[ph] = dev
            entry['devices'].append(dev)
        out.append(entry)
    # resolve links and warn about parts off their sensor's bus
    warnings = []
    for entry in out:
        for dev in entry['devices']:
            for link, ph in list(dev['links'].items()):
                target = by_phandle.get(ph)
                if target is None:
                    dev['links'][link] = f'phandle {ph:#x} (not a CCI device here)'
                    continue
                dev['links'][link] = f'{target["kind"]} {target["node"]}'
                if (target['sda'], target['scl']) != (dev['sda'], dev['scl']):
                    warnings.append(f'{dev["kind"]} {dev["node"]} (GPIO{dev["sda"]}/{dev["scl"]}): its {target["kind"]} '
                                    f'{target["node"]} is on another bus, CCI {target["cci"]} master {target["master"]} '
                                    f'= GPIO{target["sda"]}/{target["scl"]}, address {target["addr7"]:#04x}: scanning the '
                                    f'sensor\'s bus will not find it')
            for s in dev['supplies']:
                if s.get('reg_min_uv') is not None and s['want_uv'] and not (
                        s['reg_min_uv'] <= s['want_uv'] <= (s['reg_max_uv'] or s['reg_min_uv'])):
                    warnings.append(f'{dev["node"]} {s["supply"]}: asks {s["want_uv"] / 1e6:.3f} V, its regulator '
                                    f'{s["regulator"]} allows {s["reg_min_uv"] / 1e6:.3f}-{s["reg_max_uv"] / 1e6:.3f} V')
    return out, warnings


def volts(uv):
    return '-' if not uv else f'{uv / 1e6:.3f} V'


def print_table(cams, warnings):
    i2c_pmic_supplies = set()
    for entry in cams:
        base = f'{entry["base"]:#x}' if entry['base'] is not None else '?'
        print(f'CCI {entry["cci"]} @ {base}')
        for m in entry['masters']:
            print(f'  master {m["master"]}: SDA GPIO{m["sda"]}, SCL GPIO{m["scl"]} ({m["controller"]}; '
                  f'{" ".join(m["labels"])})')
        for d in entry['devices']:
            addr = f'{d["addr7"]:#04x}' if d['addr7'] is not None else '?'
            extra = f', CSI-PHY {d["csiphy"]}' if d['csiphy'] is not None else ''
            print(f'  {d["kind"]:<9} {d["node"]:<22} 7-bit {addr} on CCI {d["cci"]} master {d["master"]} '
                  f'(GPIO{d["sda"]}/{d["scl"]}){extra}')
            for g in d['gpios']:
                print(f'      gpio  {g["label"]:<18} {g["controller"]} {g["pin"]}')
            for s in d['supplies']:
                if 'regulator' not in s:
                    continue
                print(f'      power {s["supply"]:<18} {s["regulator"]:<20} {volts(s["want_uv"]):<9} {s.get("pmic", "")}')
                if s.get('pmic', '').startswith('I2C PMIC'):
                    i2c_pmic_supplies.add(s['regulator'])
            for link, what in d['links'].items():
                print(f'      link  {link:<18} {what}')
    if i2c_pmic_supplies:
        print(f'\nOn I2C PMICs (your driver sets their voltage registers: check each value with pmic_ldo.py): '
              f'{", ".join(sorted(i2c_pmic_supplies))}')
    for w in warnings:
        print(f'WARNING: {w}')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dts', type=pathlib.Path)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    cams, warnings = build(dtslib.load(a.dts))
    if not cams:
        print('no qcom,cci controller in this tree (not a Qualcomm CAMSS phone, or the camera nodes are in an overlay)')
        return 1
    if a.json:
        print(json.dumps({'cci': cams, 'warnings': warnings}, indent=1))
    else:
        print_table(cams, warnings)
    return 0


if __name__ == '__main__':
    sys.exit(main())
