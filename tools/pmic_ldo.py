"""Voltage registers of the small I2C PMICs a port's drivers program themselves (camera, display, touch supplies).

    python pmic_ldo.py decode s2mpb03 3 0xac        # LDO3 = 0xac: on, 1.800 V
    python pmic_ldo.py encode s2mpb03 3 1.8         # the value for 1.8 V (and the register to write it to)
    python pmic_ldo.py dts live.dts                 # every regulator on a known PMIC: DT voltage -> register value
    python pmic_ldo.py log FILE --chip s2mpb03 [--dts live.dts]   # decode every "LDOn 0xNN" a log printed;
                                                    # with --dts: flag the ones the tree does not ask for
    python pmic_ldo.py selftest

Why: r5q Test 427. The back camera saw nothing dim until it warmed up; every exposure and clock fix missed it.
The driver wrote S2MPB03 LDO3 as 0x80|0: LDO3 counts from 0.7 V, so that is 0.70 V where the sensor's analog
1.8 V was meant. Nothing reports a wrong voltage; only decoding the value does. Run `dts` before writing a driver
and `log` on its first boot. The encodings are in pmic_ldo.json (add a chip there, with its source header).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
TABLE = json.loads((HERE / 'pmic_ldo.json').read_text(encoding='utf-8'))
CHIPS = {k: v for k, v in TABLE.items() if not k.startswith('_')}


def chip(name):
    c = CHIPS.get(name.lower())
    if c is None:
        raise SystemExit(f'unknown PMIC {name!r}; known: {", ".join(CHIPS)} (add one to pmic_ldo.json)')
    return c


def ldo(c, n):
    d = c['ldos'].get(str(int(n)))
    if d is None:
        raise SystemExit(f'no LDO{n} on this chip (it has {", ".join(c["ldos"])})')
    return d


def decode(c, n, value):
    d = ldo(c, n)
    sel = value & d['vsel_mask']
    uv = d['min_uv'] + d['step_uv'] * sel
    en = d['enable']
    state = ''
    if en['reg'] == d['reg']:
        state = 'on' if value & (1 << en['bit']) else 'OFF'
    return uv, sel, state


def encode(c, n, uv, on=True):
    d = ldo(c, n)
    sel = round((uv - d['min_uv']) / d['step_uv'])
    if sel < 0 or sel > d['vsel_mask']:
        lo = d['min_uv'] / 1e6
        hi = (d['min_uv'] + d['step_uv'] * d['vsel_mask']) / 1e6
        raise SystemExit(f'LDO{n} cannot make {uv / 1e6:.4f} V (its range is {lo:.4f}-{hi:.4f} V)')
    exact = d['min_uv'] + d['step_uv'] * sel
    value = sel
    if on and d['enable']['reg'] == d['reg']:
        value |= 1 << d['enable']['bit']
    return value, exact, d


def describe_encode(c, n, uv):
    value, exact, d = encode(c, n, uv)
    en = d['enable']
    s = f'LDO{n} {uv / 1e6:.4f} V: write {value:#04x} to register {d["reg"]:#04x}'
    if exact != uv:
        s += f' (nearest step: {exact / 1e6:.4f} V)'
    if en['reg'] != d['reg']:
        s += f'; switch it on with bit {en["bit"]} of register {en["reg"]:#04x} (read-modify-write)'
    return s


def cmd_dts(path):
    sys.path.insert(0, str(HERE))
    import dtslib
    tree = dtslib.load(path)
    by_comp = {c['compatible']: (name, c) for name, c in CHIPS.items()}
    found = 0
    for node in tree.walk():
        comp = node.strings('compatible')
        hit = next((by_comp[x] for x in comp if x in by_comp), None)
        if hit is None:
            if any(re.search(r'pmic|regulator', x) for x in comp) and node.parent is not None and \
                    (node.parent.compatible_has('i2c') or node.parent.base.startswith('i2c')) and node.unit is not None:
                print(f'{node.path}: {comp[0]} @ {node.unit:#04x}: no encoding table -- add it to pmic_ldo.json '
                      f'from its Linux header before your driver writes it')
            continue
        name, c = hit
        bus = node.parent
        print(f'{name} @ {node.unit:#04x} on {bus.path if bus else "?"}')
        for r in node.walk():
            rn = r.string('regulator-name')
            m = re.search(r'ldo(\d+)$', rn or '')
            if not m or m.group(1) not in c['ldos']:
                continue
            lo, hi = r.cell('regulator-min-microvolt'), r.cell('regulator-max-microvolt')
            if lo is None:
                continue
            found += 1
            print(f'  {rn:<16} DT {lo / 1e6:.3f}' + (f'-{hi / 1e6:.3f}' if hi and hi != lo else '') + ' V')
            print(f'      {describe_encode(c, m.group(1), lo)}')
            if hi and hi != lo:
                print(f'      {describe_encode(c, m.group(1), hi)}')
    if not found:
        print('no regulator on a known I2C PMIC in this tree')


def dt_ranges(path, c):
    """{ldo number: [(min_uv, max_uv), ...]} for every regulator of this chip in a device tree (all instances)."""
    sys.path.insert(0, str(HERE))
    import dtslib
    out = {}
    for node in dtslib.load(path).walk():
        if c['compatible'] not in node.strings('compatible'):
            continue
        for r in node.walk():
            m = re.search(r'ldo(\d+)$', r.string('regulator-name') or '')
            lo = r.cell('regulator-min-microvolt')
            if m and lo is not None:
                out.setdefault(m.group(1), []).append((lo, r.cell('regulator-max-microvolt') or lo))
    return out


def cmd_log(path, c, dts=None):
    """Lines like 'LDO3 0xac' or 'LDO2 0x9c LDO6 0xa8': every value decoded; with a DT, a value that is on and
    outside every voltage the tree gives that LDO is flagged."""
    pat = re.compile(r'\bLDO(\d+)\s*(?:[=:]\s*)?(0x[0-9a-fA-F]{1,2})\b')
    ranges = dt_ranges(dts, c) if dts else {}
    seen, flagged = set(), 0
    for line in pathlib.Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        hits = pat.findall(line)
        if not hits or line in seen:
            continue
        seen.add(line)
        parts = []
        for n, v in hits:
            if n not in c['ldos']:
                continue
            uv, sel, state = decode(c, n, int(v, 16))
            note = ''
            outside = n in ranges and not any(lo - 1000 <= uv <= hi + 1000 for lo, hi in ranges[n])
            if ranges and state == 'on' and outside:
                want = ', '.join(f'{lo / 1e6:.3f}' + (f'-{hi / 1e6:.3f}' if hi != lo else '') for lo, hi in sorted(set(ranges[n])))
                note = f' <<< WRONG: the device tree gives LDO{n} {want} V'
                flagged += 1
            parts.append(f'LDO{n} {v} = {uv / 1e6:.3f} V{" " + state if state else ""}{note}')
        if parts:
            print(f'{"; ".join(parts)}    <- {line.strip()[:110]}')
    if dts:
        print(f'{flagged} value(s) on and outside the device tree\'s voltages')
    return 1 if flagged else 0


def selftest():
    bad = 0
    for name, c in CHIPS.items():
        for n, uv, value in c.get('known_good', []):
            got, _exact, _d = encode(c, n, uv)
            back, _sel, _state = decode(c, n, value)
            ok = got == value and back == uv
            bad += not ok
            print(f'{"PASS" if ok else "FAIL"}  {name} LDO{n} {uv / 1e6:.3f} V <-> {value:#04x}'
                  + ('' if ok else f' (encode gave {got:#04x}, decode {back / 1e6:.4f} V)'))
    # the r5q Test 427 bug: 0x80|0 on S2MPB03 LDO3 is 0.70 V, not 1.8 V
    uv, _sel, state = decode(CHIPS['s2mpb03'], 3, 0x80)
    ok = uv == 700000 and state == 'on'
    bad += not ok
    print(f'{"PASS" if ok else "FAIL"}  the Test 427 bug decodes: S2MPB03 LDO3 0x80 = {uv / 1e6:.2f} V {state}')
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('decode')
    d.add_argument('chip')
    d.add_argument('ldo')
    d.add_argument('value', type=lambda s: int(s, 0))
    e = sub.add_parser('encode')
    e.add_argument('chip')
    e.add_argument('ldo')
    e.add_argument('volts', type=float)
    t = sub.add_parser('dts')
    t.add_argument('dts', type=pathlib.Path)
    lg = sub.add_parser('log')
    lg.add_argument('file', type=pathlib.Path)
    lg.add_argument('--chip', default='s2mpb03')
    lg.add_argument('--dts', type=pathlib.Path, help='flag values outside the voltages this device tree gives')
    sub.add_parser('selftest')
    sub.add_parser('list')
    a = ap.parse_args(argv)
    if a.cmd == 'decode':
        uv, sel, state = decode(chip(a.chip), a.ldo, a.value)
        print(f'{a.chip} LDO{a.ldo} = {a.value:#04x}: step {sel}, {uv / 1e6:.4f} V{", " + state if state else ""}')
    elif a.cmd == 'encode':
        print(describe_encode(chip(a.chip), a.ldo, round(a.volts * 1e6)))
    elif a.cmd == 'dts':
        cmd_dts(a.dts)
    elif a.cmd == 'log':
        return cmd_log(a.file, chip(a.chip), a.dts)
    elif a.cmd == 'selftest':
        return selftest()
    elif a.cmd == 'list':
        for name, c in CHIPS.items():
            print(f'{name} ({c["compatible"]}): LDO{", LDO".join(c["ldos"])} -- {c["source"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
