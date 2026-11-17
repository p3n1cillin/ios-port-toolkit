"""SoC data for non-Qualcomm phone SoCs from Linux sources: MediaTek and Samsung Exynos / Google Tensor.

    python soc_other.py --linux <tree> --mediatek mt6893 [--mediatek mt6765 ...]
    python soc_other.py --linux <tree> --exynos gs101 [--exynos exynos990 ...]
    python soc_other.py --linux <tree> --all-phone

MediaTek (drivers/pinctrl/mediatek/pinctrl-mtk-<soc>.h, arch/arm64/boot/dts/mediatek/<soc>.dtsi):
  every pin's mux modes (MTK_FUNCTION(n, "NAME")); a DT pin state is `pinmux = <(pin << 8) | mode>`
  (dt-bindings/pinctrl/mt65xx.h); the pio block base(s) and the I2C controllers from the dtsi.
Exynos / Tensor (drivers/pinctrl/samsung/pinctrl-exynos-arm64.c, the SoC dtsi):
  pin banks per pin controller ("gpa0": 8 pins at offset 0x0 ...); a DT pin state is
  `samsung,pins = "gpp0-1"; samsung,pin-function = <N>`; a bank's registers start at controller base +
  bank offset (CON +0x0: 4 bits a pin, DAT +0x4, PUD +0x8, DRV +0xC on these SoCs -- check the TRM).
What this data does NOT give: MediaTek/Exynos clock controllers (no single GCC-like vote register), PMIC
access (MediaTek PWRAP, Samsung ACPM/SPEEDY), and the boot chain (no Mu-Silicium UEFI): see ARCHITECTURES.md.
"""
import argparse
import json
import pathlib
import re
import sys

MTK_PHONE = ['mt6765', 'mt6779', 'mt6795', 'mt6797', 'mt6878', 'mt6893', 'mt8183', 'mt8186', 'mt8192', 'mt8195']
EXYNOS_PHONE = ['gs101', 'exynos990', 'exynos2200', 'exynos9810', 'exynos8895', 'exynos850', 'exynos7885', 'exynos7870']


def _dtsi_regs(dtsi: str, node_re: str):
    out = []
    for m in re.finditer(node_re, dtsi):
        # the node's own text (child nodes left out), comments removed
        depth, own, i = 1, [], m.end()
        while i < len(dtsi) and depth:
            ch = dtsi[i]
            depth += (ch == '{') - (ch == '}')
            if depth == 1 and ch not in '{}':
                own.append(ch)
            i += 1
        body = re.sub(r'/[*].*?[*]/|//[^\n]*', '', ''.join(own), flags=re.S)
        rm = re.search(r'\breg\s*=\s*((?:<[^>]*>\s*,?\s*)+);', body)
        names = re.search(r'\breg-names\s*=\s*([^;]*);', body)
        regs = []
        if rm:
            for cells in re.findall(r'<([^>]*)>', rm.group(1)):
                c = [int(x, 16) for x in cells.split() if re.fullmatch(r'0x[0-9a-fA-F]+|\d+', x)]
                if len(c) >= 4:
                    regs.append(hex((c[0] << 32) | c[1]))
                elif c:
                    regs.append(hex(c[0]))
        out.append({'node': m.group(0).rstrip('{ ').strip(), 'reg': regs,
                    'reg_names': re.findall(r'"([^"]+)"', names.group(1)) if names else []})
    return out


def mediatek(linux: pathlib.Path, soc: str):
    hdr = linux / f'drivers/pinctrl/mediatek/pinctrl-mtk-{soc}.h'
    dtsi = linux / f'arch/arm64/boot/dts/mediatek/{soc}.dtsi'
    out = {'soc': soc, 'vendor': 'mediatek', '_generated_by': 'toolkit/tools/soc_other.py',
           '_sources': {'pins': hdr.name}, 'pin_state': 'pinmux = <(pin << 8) | mode> (mt65xx.h)'}
    pins = {}
    if hdr.exists():
        text = hdr.read_text(errors='replace')
        for m in re.finditer(r'MTK_PIN\(\s*(\d+),\s*"(\w+)",(.*?)\n\t\)', text, re.S):
            funcs = {int(n): name for n, name in re.findall(r'MTK_FUNCTION\((\d+),\s*"([^"]+)"\)', m.group(3))}
            pins[m.group(1)] = {'name': m.group(2), 'functions': [funcs.get(i) for i in range(max(funcs) + 1)] if funcs else []}
    out['pins'] = pins
    if dtsi.exists():
        d = dtsi.read_text(errors='replace')
        out['_sources']['dtsi'] = dtsi.name
        pio = _dtsi_regs(d, r'\bpio:\s*pinctrl@[0-9a-f]+\s*\{')
        out['pio'] = pio[0] if pio else None
        out['i2c'] = [{'node': x['node'], 'base': x['reg'][0] if x['reg'] else None}
                      for x in _dtsi_regs(d, r'\bi2c\d+:\s*i2c@[0-9a-f]+\s*\{')]
        gic = _dtsi_regs(d, r'\bgic:\s*interrupt-controller@[0-9a-f]+\s*\{')
        out['gic'] = {'dist': gic[0]['reg'][0], 'redist': gic[0]['reg'][1] if len(gic[0]['reg']) > 1 else None} \
            if gic and gic[0]['reg'] else None
    return out


def exynos(linux: pathlib.Path, soc: str):
    src = (linux / 'drivers/pinctrl/samsung/pinctrl-exynos-arm64.c').read_text(errors='replace')
    dtsi = linux / ('arch/arm64/boot/dts/exynos/google/gs101.dtsi' if soc == 'gs101'
                    else f'arch/arm64/boot/dts/exynos/{soc}.dtsi')
    pdtsi = linux / ('arch/arm64/boot/dts/exynos/google/gs101-pinctrl.dtsi' if soc == 'gs101'
                     else f'arch/arm64/boot/dts/exynos/{soc}-pinctrl.dtsi')
    out = {'soc': soc, 'vendor': 'samsung-exynos' if soc != 'gs101' else 'google-tensor (exynos-derived)',
           '_generated_by': 'toolkit/tools/soc_other.py', '_sources': {'banks': 'pinctrl-exynos-arm64.c'},
           'pin_state': 'samsung,pins = "bank-N"; samsung,pin-function = <F>',
           'bank_registers': {'con': '0x0 (4 bits a pin)', 'dat': '0x4', 'pud': '0x8', 'drv': '0xC'}}
    # the controllers, in the order of <soc>_pin_ctrl[]; each lists its bank array
    m = re.search(rf'static (?:const )?struct samsung_pin_ctrl {soc}_pin_ctrl\[\][^=]*=\s*\{{(.*?)\n\}};', src, re.S)
    controllers = []
    if m:
        for arr in re.findall(r'\.pin_banks\s*=\s*(\w+)', m.group(1)):
            am = re.search(rf'static (?:const )?struct samsung_pin_bank_data {arr}\[\][^=]*=\s*\{{(.*?)\n\}};', src, re.S)
            banks = []
            if am:
                for bm in re.finditer(r'\w+_PIN_BANK_\w+\(\s*(\d+),\s*(0x[0-9a-fA-F]+),\s*"(\w+)"', am.group(1)):
                    banks.append({'name': bm.group(3), 'pins': int(bm.group(1)), 'offset': bm.group(2)})
            controllers.append({'banks_array': arr, 'banks': banks})
    if dtsi.exists():
        d = dtsi.read_text(errors='replace')
        out['_sources']['dtsi'] = dtsi.name
        aliases = dict(re.findall(r'\b(pinctrl\d+)\s*=\s*&(\w+);', d))
        bases = {}
        for label in aliases.values():
            r = _dtsi_regs(d, rf'\b{label}:\s*pinctrl@[0-9a-f]+\s*\{{')
            if r and r[0]['reg']:
                bases[label] = r[0]['reg'][0]
        for i, c in enumerate(controllers):
            label = aliases.get(f'pinctrl{i}')
            c['label'], c['base'] = label, bases.get(label)
        gic = _dtsi_regs(d, r'\bgic:\s*interrupt-controller@[0-9a-f]+\s*\{')
        out['gic'] = {'dist': gic[0]['reg'][0], 'redist': gic[0]['reg'][1] if len(gic[0]['reg']) > 1 else None} \
            if gic and gic[0]['reg'] else None
        out['i2c'] = [{'node': x['node'], 'base': x['reg'][0] if x['reg'] else None}
                      for x in _dtsi_regs(d, r'\bhsi2c_\d+:\s*i2c@[0-9a-f]+\s*\{')]
    out['controllers'] = controllers
    out['_note'] = ('controller i = DT alias pinctrl<i> (the driver matches them in this order). '
                    + ('' if pdtsi.exists() else 'no separate -pinctrl.dtsi found'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--linux', type=pathlib.Path, required=True)
    ap.add_argument('--mediatek', action='append', default=[])
    ap.add_argument('--exynos', action='append', default=[])
    ap.add_argument('--all-phone', action='store_true')
    ap.add_argument('-o', '--out-dir', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / 'socs')
    a = ap.parse_args()
    mtk = a.mediatek + (MTK_PHONE if a.all_phone else [])
    exy = a.exynos + (EXYNOS_PHONE if a.all_phone else [])
    for soc in mtk:
        d = mediatek(a.linux, soc)
        (a.out_dir / f'{soc}.json').write_text(json.dumps(d, indent=1) + '\n', newline='\n')
        print(f'{soc}: {len(d["pins"])} pins, pio {d.get("pio", {}) and d["pio"].get("reg")}, '
              f'{len(d.get("i2c") or [])} i2c, gic {d.get("gic")}')
    for soc in exy:
        d = exynos(a.linux, soc)
        (a.out_dir / f'{soc}.json').write_text(json.dumps(d, indent=1) + '\n', newline='\n')
        print(f'{soc}: {len(d["controllers"])} pin controllers, {sum(len(c["banks"]) for c in d["controllers"])} banks, '
              f'bases {[c.get("base") for c in d["controllers"]]}, gic {d.get("gic")}, {len(d.get("i2c") or [])} i2c')


if __name__ == '__main__':
    sys.exit(main())
