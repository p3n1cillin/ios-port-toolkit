"""Build a Qualcomm SoC data file (toolkit/socs/<soc>.json) from Linux sources.

What it reads (any recent Linux tree; the project has linux-7.0.14 in WSL under
/var/tmp/codex-ios-r5q/work/redwood-src/xnu-deps/build/ath10k/linux-7.0.14.tar.xz):

  drivers/pinctrl/qcom/pinctrl-<soc>.c        every GPIO: its TLMM tile and its mux functions
  drivers/clk/qcom/gcc-<soc>.c                every GCC branch: halt (CBCR) register, vote register + bit,
                                              every RCG: its command register
  include/dt-bindings/clock/qcom,gcc-<soc>.h  the clock numbers a device tree's `clocks = <&gcc N>` uses

What it cannot read from those files is where each TLMM tile is: that comes from the SoC's dtsi
(upstream) or the device's uefiplat.cfg, and is kept in TILE_BASES below (add a SoC there).

    python qcom_socdata.py --linux <linux-tree> --soc sm8150 -o ../socs/sm8150.json

The extractor (extract_profile.py) uses the result to turn a device tree's pin names and clock
numbers into the register addresses a bring-up driver needs.
"""
import argparse
import json
import pathlib
import re
import sys

# TLMM tile bases (physical). Source: the SoC's upstream dtsi `tlmm: pinctrl@...` reg/reg-names,
# cross-checked with a phone's uefiplat.cfg where one was available.
TILE_BASES = {
    'sm8150': {'_source': 'upstream sm8150.dtsi tlmm reg-names; r5q uefiplat.cfg TLMM_WEST/EAST/NORTH/SOUTH',
               'west': 0x03100000, 'east': 0x03500000, 'north': 0x03900000, 'south': 0x03D00000},
    'sm8250': {'_source': 'upstream sm8250.dtsi tlmm reg-names; y2q redwood_sec_ts.c (north 0x0f900000, south 0x0f500000)',
               'west': 0x0F100000, 'south': 0x0F500000, 'north': 0x0F900000},
    'sdm845': {'_source': 'upstream sdm845.dtsi tlmm 0x03400000 + pinctrl-sdm845.c NORTH 0x500000 / SOUTH 0x900000 / EAST 0x100000',
               'north': 0x03900000, 'south': 0x03D00000, 'east': 0x03500000},
}
GCC_BASE = {'sm8150': 0x00100000, 'sm8250': 0x00100000, 'sdm845': 0x00100000, 'sm8350': 0x00100000,
            'sm8450': 0x00100000, 'sm7150': 0x00100000, 'sm6350': 0x00100000}
# The GPIO config/IO registers: 0x1000 per pin inside its tile on all of these SoCs (Linux REG_SIZE).
GPIO_STRIDE = 0x1000


def parse_pinctrl(text: str):
    tiles = None
    m = re.search(r'static const char \* const \w+_tiles\[\] = \{(.*?)\};', text, re.S)
    if m:
        tiles = re.findall(r'"(\w+)"', m.group(1))
    pins = {}
    for m in re.finditer(r'(?:\[\d+\]\s*=\s*)?\bPINGROUP\((\d+),([^)]*)\)', text):
        idx = int(m.group(1))
        args = [m.group(1)] + [a.strip() for a in m.group(2).split(',')]
        if tiles or args[1] in ('NORTH', 'SOUTH', 'EAST', 'WEST'):   # sdm845 passes the tile as a base offset
            tile = args[1].lower()
            funcs = args[2:]
        else:
            tile = None
            funcs = args[1:]
        pins[idx] = {'tile': tile, 'functions': ['gpio'] + [None if f == '_' else f for f in funcs]}
    return tiles, pins


def parse_gcc(text: str, header: str | None):
    branches, rcgs = {}, {}
    for m in re.finditer(r'static struct clk_branch (\w+) = \{(.*?)\n\};', text, re.S):
        var, body = m.group(1), m.group(2)
        e = {}
        for key in ['halt_reg', 'enable_reg', 'hwcg_reg']:
            k = re.search(r'\.' + key + r'\s*=\s*(0x[0-9a-fA-F]+)', body)
            if k:
                e[key] = int(k.group(1), 16)
        k = re.search(r'\.enable_mask\s*=\s*BIT\((\d+)\)', body)
        if k:
            e['enable_bit'] = int(k.group(1))
        k = re.search(r'\.halt_check\s*=\s*(\w+)', body)
        if k:
            e['halt_check'] = k.group(1)
        k = re.search(r'\.name\s*=\s*"(\w+)"', body)
        e['name'] = k.group(1) if k else var
        branches[var] = e
    for m in re.finditer(r'static struct clk_rcg2 (\w+) = \{(.*?)\n\};', text, re.S):
        var, body = m.group(1), m.group(2)
        k = re.search(r'\.cmd_rcgr\s*=\s*(0x[0-9a-fA-F]+)', body)
        if k:
            rcgs[var] = {'cmd_rcgr': int(k.group(1), 16)}
    table = dict(re.findall(r'\[(GCC_\w+)\]\s*=\s*&(\w+)\.clkr', text))
    numbers = {}
    if header:
        for name, num in re.findall(r'#define\s+(GCC_\w+)\s+(\d+)', header):
            numbers[int(num)] = name
    clocks = {}
    for num, name in sorted(numbers.items()):
        var = table.get(name)
        if var is None:
            continue
        if var in branches:
            clocks[str(num)] = dict(binding=name, kind='branch', **{k: (hex(v) if isinstance(v, int) and k.endswith('reg') else v)
                                                                    for k, v in branches[var].items()})
        elif var in rcgs:
            clocks[str(num)] = {'binding': name, 'kind': 'rcg', 'cmd_rcgr': hex(rcgs[var]['cmd_rcgr'])}
    return clocks


def parse_qup_ses(dtsi: str):
    """Upstream dtsi: every QUP serial engine (i2c/spi/serial node) address -> its GCC SE clock binding.

    Downstream (vendor) device trees number GCC clocks differently from upstream dt-bindings (seen on the
    Galaxy S20+), so the extractor resolves a bus's clocks by its engine's ADDRESS through this table
    rather than by the clock numbers in the vendor tree."""
    out = {}
    for m in re.finditer(r'\b(?:i2c|spi|serial)@([0-9a-f]+)\s*\{', dtsi):
        body = dtsi[m.end():m.end() + 1500]
        c = re.search(r'clocks\s*=\s*<&gcc\s+(GCC_QUPV3_WRAP\d_S\d+_CLK)>', body)
        if c:
            out[hex(int(m.group(1), 16))] = c.group(1)
    return out


def _node_regs(dtsi: str, label: str):
    """reg (base addresses) and reg-names of the node `<label>: name@addr { ... }` in a dtsi."""
    m = re.search(rf'(?<![\w-]){label}:\s*[\w,-]+@([0-9a-f]+)\s*\{{', dtsi)
    if not m:
        return None, [], []
    # the node's own text: everything at depth 1, child nodes left out
    depth, own, i = 1, [], m.end()
    while i < len(dtsi) and depth:
        ch = dtsi[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        elif depth == 1:
            own.append(ch)
        i += 1
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', ''.join(own), flags=re.S)
    rm = re.search(r'\breg\s*=\s*((?:<[^>]*>\s*,?\s*)+);', body)
    bases = []
    if rm:
        for cells in re.findall(r'<([^>]*)>', rm.group(1)):
            c = [int(x, 16) for x in cells.split() if x.startswith('0x') or x.isdigit()]
            if len(c) >= 4:
                bases.append((c[0] << 32) | c[1])     # #address-cells = 2
            elif c:
                bases.append(c[0])
    nm = re.search(r'\breg-names\s*=\s*([^;]*);', body)
    names = re.findall(r'"([^"]+)"', nm.group(1)) if nm else []
    return int(m.group(1), 16), bases, names


def tile_bases(soc: str, pinctrl_text: str, dtsi_text: str | None, tiles):
    """Where each TLMM tile is: the dtsi's tlmm reg/reg-names (one region per tile), or the driver's
    tile offsets (#define NORTH 0x...) from the single tlmm region, or the hand table above."""
    if dtsi_text:
        unit, bases, names = _node_regs(dtsi_text, 'tlmm')
        if bases and names and len(names) == len(bases) and set(names) <= {'north', 'south', 'east', 'west', 'center'}:
            return dict({n: hex(b) for n, b in zip(names, bases)}, _source=f'{soc}.dtsi tlmm reg/reg-names')
        offs = dict((k.lower(), int(v, 16)) for k, v in
                    re.findall(r'#define\s+(NORTH|SOUTH|EAST|WEST|CENTER)\s+(0x[0-9a-fA-F]+)', pinctrl_text))
        if bases and offs:
            return dict({k: hex(bases[0] + v) for k, v in offs.items()},
                        _source=f'{soc}.dtsi tlmm reg + pinctrl-{soc}.c tile offsets')
        if bases and not tiles:
            return {'all': hex(bases[0]), '_source': f'{soc}.dtsi tlmm reg (one tile)'}
    if soc in TILE_BASES:
        return {k: (hex(v) if isinstance(v, int) else v) for k, v in TILE_BASES[soc].items()}
    return {'_todo': 'tile bases unknown: add the dtsi or extend TILE_BASES'}


def build(linux: pathlib.Path, soc: str):
    pinctrl = linux / f'drivers/pinctrl/qcom/pinctrl-{soc}.c'
    gcc = linux / f'drivers/clk/qcom/gcc-{soc}.c'
    hdr = linux / f'include/dt-bindings/clock/qcom,gcc-{soc}.h'
    if not hdr.exists():
        hdr = linux / f'include/dt-bindings/clock/qcom,{soc}-gcc.h'    # the newer naming
    dtsi = linux / f'arch/arm64/boot/dts/qcom/{DTSI_NAMES.get(soc, soc)}.dtsi'
    dtsi_text = dtsi.read_text(errors='replace') if dtsi.exists() else None
    out = {'soc': soc, 'vendor': 'qcom', '_generated_by': 'toolkit/tools/qcom_socdata.py',
           '_sources': {'pinctrl': pinctrl.name, 'gcc': gcc.name, 'bindings': hdr.name},
           'gpio_stride': hex(GPIO_STRIDE)}
    if dtsi_text:
        gic = _node_regs(dtsi_text, 'intc')
        if gic[1]:
            out['gic'] = {'dist': hex(gic[1][0]), 'redist': hex(gic[1][1]) if len(gic[1]) > 1 else None}
    if pinctrl.exists():
        text = pinctrl.read_text(errors='replace')
        tiles, pins = parse_pinctrl(text)
        out['tlmm'] = {'tiles': tiles, 'bases': tile_bases(soc, text, dtsi_text, tiles),
                       'pins': {str(k): v for k, v in sorted(pins.items())}}
    if gcc.exists():
        gbase = _node_regs(dtsi_text, 'gcc')[0] if dtsi_text else None
        out['gcc'] = {'base': hex(gbase or GCC_BASE.get(soc, 0x00100000)),
                      'clocks': parse_gcc(gcc.read_text(errors='replace'),
                                          hdr.read_text(errors='replace') if hdr.exists() else None)}
        if not hdr.exists():
            out['gcc']['_warning'] = 'no dt-bindings header: clock numbers cannot be resolved'
    if dtsi_text:
        out['qup_se_clocks'] = parse_qup_ses(dtsi_text)
        out['_sources']['dtsi'] = dtsi.name
    return out


# Linux renamed some SoC dtsi files to code names
DTSI_NAMES = {'sc7280': 'kodiak', 'x1e80100': 'hamoa', 'sdm660': 'sdm630', 'qcs615': 'talos'}

# Phone/tablet-class Qualcomm SoCs Linux knows (pinctrl + gcc), and the marketing parts that share their silicon.
PHONE_SOCS = ['msm8953', 'msm8996', 'msm8998', 'sdm660', 'sdm670', 'sdm845', 'sm4450', 'sm6115', 'sm6125', 'sm6350',
              'sm6375', 'sm7150', 'sc7180', 'sc7280', 'sm8150', 'sm8250', 'sm8350', 'sm8450', 'sm8550', 'sm8650',
              'sm8750', 'sc8180x', 'sc8280xp', 'x1e80100']
ALIASES = {
    '_about': 'part numbers / vendor-tree code names -> the Linux SoC whose data applies (same silicon or die family)',
    'sm7325': 'sc7280', 'sm7325-ae': 'sc7280', 'qcm6490': 'sc7280', 'yupik': 'sc7280',
    'sm7125': 'sc7180', 'atoll': 'sc7180', 'sm7225': 'sm6350', 'lagoon': 'sm6350',
    'sm8150p': 'sm8150', 'msmnile': 'sm8150', 'sm8250-ac': 'sm8250', 'kona': 'sm8250', 'sm8325': 'sm8350',
    'lahaina': 'sm8350', 'sm8475': 'sm8450', 'waipio': 'sm8450', 'cape': 'sm8450', 'kalama': 'sm8550',
    'pineapple': 'sm8650', 'sun': 'sm8750', 'sm6115p': 'sm6115', 'bengal': 'sm6115', 'trinket': 'sm6125',
    'sdm636': 'sdm660', 'sdm630': 'sdm660', 'sdmmagpie': 'sm7150', 'sm7150-ac': 'sm7150', 'sdm710': 'sdm670',
    'sdm855': 'sm8150', 'sm7250': None, 'lito': None, 'sm7350': None, 'sm7450': None, 'sm7550': None,
    # Samsung Exynos part numbers (Mu-Silicium names them S5Exxxx) -> Linux names (tools/soc_other.py data)
    's5e9830': 'exynos990', 's5e9810': 'exynos9810', 's5e7885': 'exynos7885', 's5e7870': 'exynos7870',
    's5e9925': 'exynos2200', 's5e8895': 'exynos8895', 's5e3830': 'exynos850', 'whitechapel': 'gs101',
}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--linux', type=pathlib.Path, required=True, help='root of a Linux source tree')
    p.add_argument('--soc', action='append', help='e.g. sm8150 (repeatable)')
    p.add_argument('--all-phone', action='store_true', help='every SoC in PHONE_SOCS, plus socs/aliases.json')
    p.add_argument('-o', '--out-dir', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / 'socs')
    args = p.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    socs = (args.soc or []) + (PHONE_SOCS if args.all_phone else [])
    if not socs:
        p.error('--soc or --all-phone')
    if args.all_phone:
        (args.out_dir / 'aliases.json').write_text(json.dumps(ALIASES, indent=1) + '\n', newline='\n')
    for soc in socs:
        data = build(args.linux, soc)
        path = args.out_dir / f'{soc}.json'
        path.write_text(json.dumps(data, indent=1) + '\n', newline='\n')
        print(f'{path}: {len(data.get("tlmm", {}).get("pins", {}))} pins, '
              f'{len(data.get("gcc", {}).get("clocks", {}))} clocks')


if __name__ == '__main__':
    sys.exit(main())
