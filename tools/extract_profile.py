"""Extract candidate device-profile values from a phone's decompiled live device tree.

Get the tree from the running Android (root):  adb shell su -c 'cat /sys/firmware/fdt' > live.dtb
then  dtc -I dtb -O dts -q live.dtb -o live.dts  (keep both private: they identify the phone).

    python extract_profile.py live.dts [--soc sm8150] [-o candidate.json] [--compare ../devices/r5q.json]

What it finds (each value with its evidence: the DT path and property it came from):
  memory    reserved-memory (every region, no-map ones listed for the UEFI memory map), the splash /
            Display Reserved region, SMEM, Samsung sec_debug (RAM log ring) or ramoops
  cpu/gic   cores, MPIDR layout, small cores, GIC addresses, the virtual timer's INTID
  touch     the touchscreen node, its I2C engine, the bus pins + mux function + TLMM tile, the IRQ pin,
            its supplies (GPIO regulators or PMIC LDOs), and its clocks resolved to GCC registers
  battery   a fuel gauge on I2C, same resolution
  buttons   gpio_keys -> PMIC GPIO peripheral id; the PON block's SID/base/peripheral id; SPMI arbiter
  cpufreq   OSM / cpufreq-hw / EPSS register blocks per cluster
  display   MDSS base, TE pin (mdp_vsync), panel timing candidates
  wifi      icnss / cnss / PCIe nodes and their status
SoC facts the tree does not hold (pin -> tile, mux indexes, clock numbers -> registers) come from
toolkit/socs/<soc>.json (tools/qcom_socdata.py). Everything here is a CANDIDATE: check the report,
and measure on the phone (read-only) before a driver relies on it.
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dtslib  # noqa: E402
import profile_spec  # noqa: E402

TOUCH_COMPATIBLES = ['sec,sec_ts', 'samsung,sec_ts', 'stm,fts', 'st,fts', 'goodix', 'synaptics', 'focaltech',
                     'novatek', 'himax', 'atmel,maxtouch', 'zinitix', 'ilitek', 'elan', 'fts_ts', 'sec,sec_ts_y771']
GAUGE_HINTS = ['fuelgauge', 'fuel-gauge', 'fuel_gauge', 'max77705', 'max1720', 's2mu', 'bq27', 'sm5714-fuelgauge']


class Out:
    def __init__(self):
        self.profile = {}
        self.evidence = {}
        self.notes = []

    def put(self, path, value, evidence):
        node = self.profile
        parts = path.split('.')
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = value
        self.evidence[path] = evidence

    def note(self, text):
        self.notes.append(text)


def short(name):
    """A node name without its vendor prefix and unit address: qcom,pm8150@0 -> pm8150."""
    return name.split(',', 1)[-1].split('@', 1)[0]


def hx(v):
    return None if v is None else f'0x{v:X}'


SOCDATA = {}   # set by extract(): mux_pins needs the MediaTek mode names


def mux_pins(tree, state_phandles):
    """pinctrl state phandles -> [(pin, function)], whatever the vendor's syntax:
      Qualcomm   pins = "gpio55"; function = "qup17";          -> (55, 'qup17')
      MediaTek   pinmux = <(pin << 8) | mode>;                  -> (65, 'SDA2')   (mode name from socs data)
      Samsung    samsung,pins = "gpp0-1"; samsung,pin-function = <2>;  -> ('gpp0-1', 'func2')"""
    out = []
    for ph in state_phandles:
        node = tree.phandle(ph)
        if node is None:
            continue
        for n in node.walk():
            func = n.string('function')
            for p in n.strings('pins'):
                m = re.fullmatch(r'gpio(\d+)', p)
                if m:
                    out.append((int(m.group(1)), func))
            for v in n.cells('pinmux'):
                if isinstance(v, int):
                    pin, mode = v >> 8, v & 0xff
                    names = (SOCDATA.get('pins', {}).get(str(pin)) or {}).get('functions') or []
                    out.append((pin, names[mode] if mode < len(names) and names[mode] else f'mode{mode}'))
            fn = n.cell('samsung,pin-function')
            for p in n.strings('samsung,pins'):
                out.append((p, f'func{fn}' if fn is not None else None))
    seen, res = set(), []
    for pin, func in out:
        if (pin, func) not in seen:
            seen.add((pin, func))
            res.append((pin, func))
    return res


def soc_pin(socdata, pin, function=None):
    """(where the pin's registers are, the mux index of `function`) for any vendor's socs data."""
    socdata = socdata or {}
    vendor = socdata.get('vendor', 'qcom')
    if vendor == 'mediatek':
        info = socdata.get('pins', {}).get(str(pin))
        if not info:
            return None, None
        idx = info['functions'].index(function) if function in info['functions'] else None
        if idx is None and isinstance(function, str) and function.startswith('mode'):
            idx = int(function[4:])
        return 'pio', idx
    if 'exynos' in vendor or 'tensor' in vendor:
        bank = str(pin).rsplit('-', 1)[0]
        for c in socdata.get('controllers', []):
            for b in c['banks']:
                if b['name'] == bank:
                    base = int(c['base'], 16) + int(b['offset'], 16) if c.get('base') else None
                    idx = int(function[4:]) if isinstance(function, str) and function.startswith('func') else None
                    return (f'{c.get("label")} bank {bank} @ {hex(base)}' if base else f'{c.get("label")} bank {bank}'), idx
        return None, None
    info = socdata.get('tlmm', {}).get('pins', {}).get(str(pin))
    if not info:
        return None, None
    idx = None
    if function and function in info['functions']:
        idx = info['functions'].index(function)
    return info['tile'] or 'all', idx      # SoCs without tiles: one TLMM region ("all")


def clock_entries(tree, node, socdata):
    """clocks = <&cc N ...> with clock-names -> {name: soc gcc entry or raw}."""
    raw = node.cells('clocks')
    names = node.strings('clock-names')
    out = {}
    i = 0
    k = 0
    while i < len(raw):
        ctrl = tree.phandle(raw[i])
        ncells = ctrl.cell('#clock-cells', 1) if ctrl is not None else 1
        args = raw[i + 1:i + 1 + ncells]
        i += 1 + ncells
        name = names[k] if k < len(names) else f'clock{k}'
        k += 1
        entry = {'controller': ctrl.path if ctrl else None, 'index': args[0] if args else None}
        if ctrl is not None and ctrl.compatible_has('gcc') and args:
            gcc = (socdata or {}).get('gcc', {}).get('clocks', {}).get(str(args[0]))
            if gcc:
                entry.update(gcc)
        out[name] = entry
    return out


def i2c_bus(tree, dev, socdata, out, prefix):
    bus = dev.parent
    ev = dev.path
    out.put(f'{prefix}.compatible', dev.string('compatible'), f'{dev.path} compatible')
    regs = dev.cells('reg')
    if regs:
        out.put(f'{prefix}.i2c_address', hx(regs[0]), f'{dev.path} reg')
    if bus is None:
        return
    if bus.reg():
        out.put(f'{prefix}.se_base', hx(bus.reg()[0][0]), f'{bus.path} reg')
    pins = mux_pins(tree, bus.cells('pinctrl-0'))
    bus_pins = [(p, f) for p, f in pins if f and f.startswith('qup')] or pins
    # MediaTek names its I2C modes SDAn/SCLn: put SDA first, whatever the DT order
    if any(f and re.match(r'(SDA|SCL)', str(f)) for _, f in bus_pins):
        bus_pins = sorted(bus_pins, key=lambda pf: 0 if str(pf[1]).startswith('SDA') else 1)
    spi = bus.compatible_has('spi')
    out.put(f'{prefix}.bus', 'spi' if spi else 'i2c', f'{bus.path} compatible {bus.string("compatible")}')
    if spi and len(bus_pins) >= 4:
        # GENI SPI on Qualcomm: the engine's pins in order MISO, MOSI, CLK, CS (pinctrl-sm8350.c "qupN" groups)
        names = ['miso_gpio', 'mosi_gpio', 'clk_gpio', 'cs_gpio']
        for (pin, fn), name in zip(bus_pins[:4], names):
            out.put(f'{prefix}.{name}', pin, f'{bus.path} pinctrl-0 -> pins, GENI SPI order MISO MOSI CLK CS: CHECK '
                                             'against the schematic or the vendor driver')
        tile, idx = soc_pin(socdata, bus_pins[0][0], bus_pins[0][1])
        out.put(f'{prefix}.pin_function', idx, f'function "{bus_pins[0][1]}" index in socs pin {bus_pins[0][0]} mux list')
        out.put(f'{prefix}.pin_function_name', bus_pins[0][1], f'{bus.path} pinctrl-0 function')
        out.put(f'{prefix}.bus_tile', tile, f'socs pin {bus_pins[0][0]} tile')
    elif len(bus_pins) >= 2:
        (sda, fn), (scl, _) = bus_pins[0], bus_pins[1]
        tile, idx = soc_pin(socdata, sda, fn)
        out.put(f'{prefix}.sda_gpio', sda, f'{bus.path} pinctrl-0 -> pins (first is SDA on GENI I2C)')
        out.put(f'{prefix}.scl_gpio', scl, f'{bus.path} pinctrl-0 -> pins')
        out.put(f'{prefix}.pin_function', idx, f'function "{fn}" index in socs pin {sda} mux list')
        out.put(f'{prefix}.pin_function_name', fn, f'{bus.path} pinctrl-0 function')
        out.put(f'{prefix}.bus_tile', tile, f'socs pin {sda} tile')
    by_number = clock_entries(tree, bus, socdata)
    gcc_clocks = (socdata or {}).get('gcc', {}).get('clocks', {})
    by_binding = {e['binding']: e for e in gcc_clocks.values() if 'binding' in e}
    se_addr = bus.reg()[0][0] if bus.reg() else None
    se_name = (socdata or {}).get('qup_se_clocks', {}).get(hex(se_addr)) if se_addr is not None else None
    gcc = {}
    if se_name:
        m = re.fullmatch(r'GCC_QUPV3_WRAP(\d)_S(\d+)_CLK', se_name)
        w = m.group(1)
        names = {'se': se_name, 'se_src': se_name + '_SRC', 'm_ahb': f'GCC_QUPV3_WRAP_{w}_M_AHB_CLK',
                 's_ahb': f'GCC_QUPV3_WRAP_{w}_S_AHB_CLK', 'core': f'GCC_QUPV3_WRAP{w}_CORE_CLK',
                 'core2x': f'GCC_QUPV3_WRAP{w}_CORE_2X_CLK'}
        gcc['qup'] = f'wrapper {w}, serial engine {m.group(2)}'
        found = {k: by_binding.get(v) for k, v in names.items()}
        votes = {e.get('enable_reg') for k, e in found.items() if e and e.get('enable_reg')}
        if len(votes) == 1:
            gcc['vote_reg'] = votes.pop()
        gcc['vote_bits'] = {k: e['enable_bit'] for k, e in found.items() if e and 'enable_bit' in e}
        for k, key in [('se', 'se_cbcr'), ('m_ahb', 'm_ahb_cbcr'), ('s_ahb', 's_ahb_cbcr'),
                       ('core', 'core_cbcr'), ('core2x', 'core2x_cbcr')]:
            if found.get(k) and found[k].get('halt_reg'):
                gcc[key] = found[k]['halt_reg']
        if found.get('se_src'):
            gcc['se_rcg'] = found['se_src'].get('cmd_rcgr')
        gcc['bindings'] = names
        # the vendor tree's own clock numbers, read as upstream bindings: do they agree?
        dt_se = by_number.get('se-clk', {}).get('binding')
        gcc['dt_clock_numbers_match_upstream'] = dt_se == se_name
        if dt_se != se_name:
            out.note(f'{prefix}: {bus.path} se-clk is clock {by_number.get("se-clk", {}).get("index")} in this '
                     f'vendor tree, which upstream numbers {dt_se}; by its address the engine is {se_name}: '
                     'resolved by address (vendor clock numbering differs from upstream bindings)')
        evidence = (f'{bus.path} reg {hex(se_addr)} -> upstream {socdata.get("_sources", {}).get("dtsi")} engine '
                    f'clock {se_name} -> gcc-<soc>.c registers')
    elif (socdata or {}).get('vendor', 'qcom') != 'qcom':
        clocks = clock_entries(tree, bus, None)
        if clocks:
            out.put(f'{prefix}.clocks', {k: {'controller': v['controller'], 'index': v['index']} for k, v in clocks.items()},
                    f'{bus.path} clocks: {socdata.get("vendor")} clock controllers are not resolved to registers by '
                    'this toolkit yet: read the vendor clock driver for these indexes')
    elif by_number:
        gcc['clocks'] = by_number
        votes = {c.get('enable_reg') for c in by_number.values() if c.get('enable_reg')}
        if len(votes) == 1:
            gcc['vote_reg'] = votes.pop()
            gcc['vote_bits'] = {n: c['enable_bit'] for n, c in by_number.items() if 'enable_bit' in c}
        for name, key in [('se-clk', 'se_cbcr'), ('m-ahb', 'm_ahb_cbcr'), ('s-ahb', 's_ahb_cbcr')]:
            if by_number.get(name, {}).get('halt_reg'):
                gcc[key] = by_number[name]['halt_reg']
        evidence = (f'{bus.path} clocks read as UPSTREAM dt-binding numbers (no dtsi engine table for this SoC): '
                    'vendor trees may number differently -- check the names against the engine address')
        out.note(f'{prefix}: clocks resolved by number only; verify')
    if gcc:
        out.put(f'{prefix}.gcc', gcc, evidence)
    if bus.has('dmas'):
        out.put(f'{prefix}.transport', 'bitbang?', f'{bus.path} has dmas (GSI): if GENI_IF_DISABLE_RO says FIFO '
                                                   'disabled at run time, bit-bang the pins (r5q: yes)')


def extract(tree, socdata, soc):
    out = Out()
    SOCDATA.clear()
    SOCDATA.update(socdata or {})
    root = tree.root
    if socdata and socdata.get('vendor', 'qcom') != 'qcom':
        out.put('device.soc_vendor', socdata['vendor'], f'socs/{socdata.get("soc")}.json')
        out.put('pinctrl', {'pio': socdata.get('pio'), 'controllers': [
            {'label': c.get('label'), 'base': c.get('base'), 'banks': len(c['banks'])} for c in socdata.get('controllers', [])]
            or None, 'pin_state': socdata.get('pin_state')}, f'socs/{socdata.get("soc")}.json')
    elif soc:
        out.put('device.soc_vendor', 'qcom' if socdata else None, 'root compatible')
    model = root.string('model', '')
    out.put('device.dt_model', model, '/ model')
    m = re.search(r'Samsung (\w+) PROJECT', model)
    if m:
        out.put('device.codename', m.group(1).lower(), '/ model "Samsung <CODENAME> PROJECT"')
    out.put('device.soc', soc, '/ compatible or --soc')
    msm = root.string('qcom,msm-name')
    if msm:
        out.put('device.msm_name', msm, '/ qcom,msm-name')

    # ---- memory ----
    mem = [n for n in root.children if n.base == 'memory' or n.string('device_type') == 'memory']
    total = sum(s for n in mem for _, s in n.reg())
    if total:
        out.put('device.ram_bytes', total, '/memory reg (sum)')
        out.put('memory.dram_base', hx(min(a for n in mem for a, _ in n.reg())), '/memory reg')
    rm = tree.find('/reserved-memory')
    regions, nomap = [], []
    for n in (rm.children if rm else []):
        for a, s in n.reg():
            r = {'name': n.name.split('@')[0], 'base': hx(a), 'size': hx(s), 'no_map': n.has('no-map')}
            regions.append(r)
            if n.has('no-map'):
                nomap.append(r)
    out.put('memory.reserved', regions, '/reserved-memory/* reg (dynamic regions without reg omitted)')
    out.put('memory.secure_no_map', nomap, '/reserved-memory/* with no-map: the UEFI map must reserve every one')
    for n in (rm.children if rm else []):
        b = n.base
        reg = n.reg()
        if not reg:
            continue
        a, s = reg[0]
        if b in ('cont_splash_region', 'cont-splash-region', 'splash_region', 'framebuffer'):
            out.put('memory.display_reserved.base', hx(a), f'{n.path} reg')
            out.put('memory.display_reserved.size', hx(s), f'{n.path} reg')
            out.put('memory.bootshim_info', hx(a + s - 0x100000),
                    'last MiB of the splash region (BootShim progress record), if that MiB is outside the frame')
        elif b in ('smem', 'smem_region', 'smem_mem'):
            out.put('memory.smem.base', hx(a), f'{n.path} reg')
            out.put('memory.smem.size', hx(s), f'{n.path} reg')
        elif b.startswith('sec_debug_region'):
            out.put('memory.log_ring.base', hx(a + 0x200000),
                    f'{n.path} reg + 0x200000 (Samsung sec_log ring = /proc/last_kmsg; r5q measured). VERIFY: '
                    'read the ring header magic "LOGM" there from Android (root, /proc/last_kmsg matches its text)')
            out.put('memory.log_ring.size', hx(0x200000), 'Samsung sec_log size (r5q)')
            out.put('memory.log_ring.magic', '0x4D474F4C', 'Samsung sec_log header magic "LOGM"')
        elif b.startswith('ramoops') and 'memory.log_ring.base' not in out.evidence:
            out.put('memory.ramoops', {'base': hx(a), 'size': hx(s)}, f'{n.path} reg (non-Samsung log candidate)')
    smem_node = next((n for n in tree.walk() if n.compatible_has('qcom,smem')), None)
    if smem_node is not None and 'memory.smem.base' not in out.evidence:
        ph = smem_node.cell('memory-region')
        target = tree.phandle(ph) if ph else None
        if target is not None and target.reg():
            out.put('memory.smem.base', hx(target.reg()[0][0]), f'{smem_node.path} memory-region -> {target.path}')

    # ---- cpus / gic / timer ----
    cpus = [n for n in tree.walk() if n.string('device_type') == 'cpu']
    if cpus:
        regs = [n.cells('reg')[-1] for n in cpus if n.cells('reg')]
        out.put('cpu.cores', len(cpus), '/cpus/cpu@* count')
        if regs and all(r % 0x100 == 0 for r in regs) and max(regs) > 0:
            out.put('cpu.mpidr_core_shift', 8, f'/cpus/cpu@* reg = {[hex(r) for r in regs]} (Aff1)')
        elif regs:
            out.put('cpu.mpidr_core_shift', 0, f'/cpus/cpu@* reg = {[hex(r) for r in regs]} (Aff0)')
        caps = [(n.cell('capacity-dmips-mhz'), i) for i, n in enumerate(cpus)]
        if all(c is not None for c, _ in caps):
            low = min(c for c, _ in caps)
            mask = sum(1 << i for c, i in caps if c == low)
            out.put('cpu.small_cores', hx(mask), f'capacity-dmips-mhz {[c for c, _ in caps]}: lowest = small')
        out.put('cpu.enable_method', cpus[0].string('enable-method'), f'{cpus[0].path} enable-method')
    gic = next((n for n in tree.walk() if n.compatible_has('arm,gic-v3', 'arm,gic-400', 'arm,cortex-a15-gic')), None)
    if gic is not None:
        r = gic.reg()
        out.put('gic.dist', hx(r[0][0]), f'{gic.path} reg[0]')
        if len(r) > 1:
            out.put('gic.redist', hx(r[1][0]), f'{gic.path} reg[1]')
    timer = next((n for n in tree.walk() if n.compatible_has('arm,armv8-timer')), None)
    if timer is not None:
        ints = timer.cells('interrupts')
        if len(ints) >= 9:
            stride = 3
            ppis = [ints[i + 1] for i in range(0, len(ints), stride)]
            out.put('gic.vtimer_intid', ppis[2] + 16,
                    f'{timer.path} interrupts: PPIs {ppis} (secure, non-secure, VIRTUAL, hyp) -> 3rd + 16. '
                    'XNU defaults to 27: pass gic_vtimer= when it differs')
        if timer.cell('clock-frequency'):
            out.put('cpu.cntfrq', timer.cell('clock-frequency'), f'{timer.path} clock-frequency')

    # ---- TLMM ----
    tlmm = next((n for n in tree.walk() if re.search(r'qcom,(sm|sdm|sc|msm)\w*-pinctrl$|qcom,\w+-tlmm$',
                                                         ' '.join(n.strings('compatible'))) and n.reg()), None)
    if tlmm is None:
        tlmm = next((n for n in tree.walk() if n.base == 'pinctrl' and n.has('gpio-controller') and n.reg()
                     and n.reg()[0][0] < 0x20000000 and 'spmi' not in n.path), None)
    if tlmm is not None:
        out.put('tlmm.base', hx(tlmm.reg()[0][0]), f'{tlmm.path} reg[0] ({tlmm.string("compatible")})')
        bases = (socdata or {}).get('tlmm', {}).get('bases')
        if bases:
            out.put('tlmm.tiles', {k: v for k, v in bases.items() if not k.startswith('_')},
                    f'socs/{soc}.json ({bases.get("_source", "")})')

    # ---- touch ----
    touch = [n for n in tree.walk() if n.status_ok and (n.compatible_has(*TOUCH_COMPATIBLES)
                                                         or n.base in ('touchscreen', 'touch'))
             and n.parent is not None and n.parent.compatible_has('i2c', 'spi')]
    if touch:
        dev = touch[0]
        if len(touch) > 1:
            out.note(f'touch: several candidates {[t.path for t in touch]}; took the first')
        i2c_bus(tree, dev, socdata, out, 'touch')
        irq = None
        for key in ('irq_gpio', 'sec,irq_gpio', 'synaptics,irq-gpio', 'goodix,irq-gpio', 'interrupt-gpios', 'irq-gpios'):
            c = dev.cells(key)
            if len(c) >= 2:
                irq = (c[1], f'{dev.path} {key}')
                break
        if irq is None and dev.has('interrupts') and dev.cell('interrupt-parent') == (tlmm.cell('phandle') if tlmm else -1):
            irq = (dev.cell('interrupts'), f'{dev.path} interrupts (parent TLMM)')
        if irq:
            out.put('touch.irq_gpio', irq[0], irq[1])
            tile, _ = soc_pin(socdata, irq[0])
            out.put('touch.irq_tile', tile, f'socs pin {irq[0]} tile')
        supplies = []
        for key, val in dev.props.items():
            bare = key.split(',', 1)[-1]          # sec,regulator_avdd -> regulator_avdd
            names = dev.strings(key) if bare.startswith('regulator_') or bare.endswith('_regulator') else []
            for name in names:
                reg = next((n for n in tree.walk() if n.string('regulator-name') == name), None)
                if reg is None:
                    supplies.append({'name': name, 'kind': 'unknown'})
                    continue
                gpio = reg.cells('gpio') or reg.cells('gpios')
                ent = {'name': name, 'node': reg.path}
                if reg.compatible_has('regulator-fixed') and len(gpio) >= 2:
                    ent.update(kind='gpio', gpio=gpio[1], active_high=reg.has('enable-active-high'),
                               tile=soc_pin(socdata, gpio[1])[0],
                               microvolts=reg.cell('regulator-min-microvolt'))
                else:
                    ent.update(kind='pmic-regulator', compatible=reg.string('compatible'))
                supplies.append(ent)
            if key.endswith('-supply'):
                reg = tree.phandle(dev.cell(key))
                supplies.append({'name': key[:-7], 'kind': 'pmic-regulator',
                                 'node': reg.path if reg else None,
                                 'regulator_name': reg.string('regulator-name') if reg else None})
        out.put('touch.supplies', supplies, f'{dev.path} regulator_* names -> regulator nodes, *-supply phandles. '
                                            'A driver reads them first and raises only a low GPIO supply')
        mc = dev.cells('max_coords') or dev.cells('sec,max_coords')
        if mc:
            out.put('touch.max_coords', mc, f'{dev.path} max_coords')
        fw = dev.strings('firmware_name') or dev.strings('sec,firmware_name')
        if fw:
            out.put('touch.firmware', fw[-1], f'{dev.path} firmware_name')
    else:
        out.note('touch: no touchscreen node under an I2C/SPI controller found')

    # ---- battery ----
    def gauge_rank(n):
        text = (n.name + ' ' + ' '.join(n.strings('compatible'))).lower()
        for i, h in enumerate(GAUGE_HINTS):
            if h in text:
                return i
        return None
    gauges = sorted([n for n in tree.walk() if gauge_rank(n) is not None and n.parent is not None
                     and n.parent.compatible_has('i2c') and n.cells('reg')], key=gauge_rank)
    if gauges:
        g = gauges[0]
        out.put('battery.gauge', g.name.split('@')[0], f'{g.path}')
        i2c_bus(tree, g, socdata, out, 'battery')
        if g.cell('capacity_max'):
            out.put('battery.capacity_max', g.cell('capacity_max'), f'{g.path} capacity_max')
    else:
        out.note('battery: no fuel gauge on I2C found (it may sit inside a PMIC/charger: look for max77705, '
                 'pm8150b qg, smb1390 ...)')

    # ---- SPMI / PON / keys ----
    arb = next((n for n in tree.walk() if n.compatible_has('spmi-pmic-arb') and 'debug' not in n.name), None)
    pmics = {}
    if arb is not None:
        names = {k: hx(v[0]) for k, v in arb.reg_names().items()}
        ver = arb.cell('qcom,arb-version') or arb.cell('qcom,ee')
        out.put('buttons.spmi', dict(names, ee=arb.cell('qcom,ee')), f'{arb.path} reg-names')
        for p in arb.children:
            r = p.cells('reg')
            if r:
                pmics[p.path] = r[0]
    pons = [n for n in tree.walk() if n.compatible_has('power-on', 'pon', 'pwrkey') and n.parent is not None
            and n.parent.path in pmics]
    main_pon = None
    for n in pons:
        if any(c.cell('pon-type') in (0, 1) or c.cell('qcom,pon-type') in (0, 1)
               or c.compatible_has('pwrkey', 'resin') for c in n.children):
            main_pon = n
            break
    if main_pon is not None:
        sid = pmics[main_pon.parent.path]
        base = main_pon.cells('reg')[0]
        gen3 = main_pon.compatible_has('pmk8350', 'pmk8550', 'pon-gen3')
        types = {}
        for c in main_pon.children:
            t = c.cell('pon-type') if c.cell('pon-type') is not None else c.cell('qcom,pon-type')
            if t is None and c.compatible_has('pwrkey'):
                t = 0                       # upstream form: qcom,pmXXXX-pwrkey / -resin child nodes
            if t is None and c.compatible_has('resin'):
                t = 1
            if t is not None:
                types[t] = c.cell('code') or c.cell('linux,code')
        out.put('buttons.pon', {
            'pmic': short(main_pon.parent.name), 'sid': sid, 'base': hx(base),
            'peripheral_id': f'0x{(sid << 8) | (base >> 8):03x}',
            'generation': 3 if gen3 else 2,
            'kpdpwr_bit': '0x80' if gen3 else '0x01', 'resin_bit': '0x40' if gen3 else '0x02',
            'subtype_from': 9 if gen3 else 4, 'subtype_to': 9 if gen3 else 5,
            'pon_types': types}, f'{main_pon.path} ({main_pon.string("compatible")}); pon-type 0 KPDPWR, 1 RESIN. '
                                 'Subtype range/status bits are the PON generation\'s (redwood_buttons.c table)')
        out.put('buttons.power', 'kpdpwr' if 0 in types else None, 'pon-type 0 child')
        out.put('buttons.volume_down', 'resin' if types.get(1) == 114 else ('resin?' if 1 in types else None),
                'pon-type 1 child, code 114 = KEY_VOLUMEDOWN')
    keys = next((n for n in tree.walk() if n.compatible_has('gpio-keys') and n.status_ok), None)
    if keys is not None:
        for k in keys.children:
            g = k.cells('gpios')
            code = k.cell('linux,code') or k.cell('code')
            if len(g) < 2:
                continue
            ctrl = tree.phandle(g[0])
            ent = {'label': k.string('label'), 'code': code, 'gpio': g[1], 'controller': ctrl.path if ctrl else None}
            if ctrl is not None and ctrl.compatible_has('spmi-gpio') and ctrl.parent is not None \
                    and ctrl.parent.path in pmics:
                sid = pmics[ctrl.parent.path]
                gbase = ctrl.cells('reg')[0]
                addr = gbase + (g[1] - 1) * 0x100
                ent.update(pmic=short(ctrl.parent.name), sid=sid, peripheral_id=f'0x{(sid << 8) | (addr >> 8):03x}',
                           active_low=bool(g[2] & 1) if len(g) > 2 else None)
            name = {115: 'volume_up', 114: 'volume_down', 116: 'power'}.get(code, k.name)
            out.put(f'buttons.{name}', ent, f'{k.path} gpios -> {ent["controller"]}; peripheral id = '
                                            'sid << 8 | (gpio block + (n-1)*0x100) >> 8')

    # ---- cpufreq ----
    osm = next((n for n in tree.walk() if n.compatible_has('clk-cpu-osm')), None)
    hw = next((n for n in tree.walk() if n.compatible_has('cpufreq-hw', 'cpufreq-epss')), None)
    if osm is not None:
        rn = osm.reg_names()
        doms = [{'name': k, 'base': hx(rn[k][0])} for k in ('osm_pwrcl_base', 'osm_perfcl_base', 'osm_perfpcl_base')
                if k in rn]
        out.put('cpufreq.kind', 'osm', f'{osm.path} qcom,clk-cpu-osm')
        out.put('cpufreq.domains', doms, f'{osm.path} reg-names (pwrcl = small cores, perfcl = big, perfpcl = prime)')
        out.put('cpufreq.table_offset', '0x110', 'qcom-cpufreq-hw.c default layout (OSM)')
        out.put('cpufreq.row_bytes', 32, 'OSM LUT row')
        out.put('cpufreq.perf_state', '0x920', 'OSM perf state register')
    elif hw is not None:
        epss = hw.compatible_has('epss')
        rn = hw.reg_names() or {f'freq-domain{i}': r for i, r in enumerate(hw.reg())}
        out.put('cpufreq.kind', 'epss' if epss else 'cpufreq-hw', f'{hw.path} {hw.string("compatible")}')
        out.put('cpufreq.domains', [{'name': k, 'base': hx(v[0])} for k, v in rn.items()], f'{hw.path} reg')
        out.put('cpufreq.table_offset', '0x100' if epss else '0x110', 'qcom-cpufreq-hw.c layout')
        out.put('cpufreq.row_bytes', 4 if epss else 32, 'qcom-cpufreq-hw.c layout')
        out.put('cpufreq.perf_state', '0x320' if epss else '0x920', 'qcom-cpufreq-hw.c layout')

    # ---- display ----
    mdss = next((n for n in tree.walk() if n.base in ('qcom,mdss_mdp', 'mdss_mdp') and n.reg()), None) or \
        next((n for n in tree.walk() if n.compatible_has('qcom,sde-kms', 'qcom,mdss') and n.reg()), None) or \
        next((n for n in tree.walk() if n.base in ('display-subsystem', 'mdss') and n.reg()), None)
    if mdss is not None:
        out.put('display.mdss_base', hx(mdss.reg()[0][0]), f'{mdss.path} reg[0]')
    te = []
    for n in tree.walk():
        if n.string('function') == 'mdp_vsync':
            te += [int(p[4:]) for p in n.strings('pins') if p.startswith('gpio')]
    if te:
        pin = te[0]
        tile, idx = soc_pin(socdata, pin, 'mdp_vsync')
        out.put('display.te_gpio', pin, 'pinctrl node with function "mdp_vsync"')
        out.put('display.te_function', idx, f'socs pin {pin} mux list')
    # the panel(s) the board selects: nodes a display node points at (qcom,dsi-default-panel, ...panel phandles)
    selected = set()
    for n in tree.walk():
        for k in n.props:
            if 'panel' in k and not k.startswith('qcom,mdss-dsi-panel'):
                for c in n.cells(k):
                    t = tree.phandle(c) if isinstance(c, int) else None
                    if t is not None:
                        selected.add(t.path)
    panels = []
    for n in tree.walk():
        w, h = n.cell('qcom,mdss-dsi-panel-width'), n.cell('qcom,mdss-dsi-panel-height')
        if w and h and 'sim' not in n.path:
            chosen = any(n.path.startswith(s) for s in selected)
            panels.append({'node': n.path, 'width': w, 'height': h, 'fps': n.cell('qcom,mdss-dsi-panel-framerate'),
                           'selected_by_board': chosen})
    panels.sort(key=lambda x: not x['selected_by_board'])
    out.put('display.panel_candidates', panels,
            'DSI panel timing nodes, the ones a display node points at first; Qualcomm base trees carry many '
            'reference (MTP/QRD) panels that are not this phone\'s, and Samsung panels are often not in the DT: '
            'confirm with ABL\'s log or the MDSS dump')
    # display.width/height are deliberately NOT set from these: on r5q and y2q every reference panel is
    # pointed at by some display node, and none is the real one (1080x2400, 1440x3200). Measure it instead.

    # ---- wifi ----
    wifi = []
    for n in tree.walk():
        if (n.compatible_has('icnss', 'cnss', 'qca6390', 'qca6490', 'wcn39', 'wcn6750', 'wcn6855', 'wcn7850',
                             'ath11k', 'ath12k', 'brcm,bcm43', 'bcmdhd', 'mediatek,mt76', 'wlan')
                and not n.compatible_has('-bt', 'bluetooth')) or \
                (n.compatible_has('pci-msm', 'qcom,pcie') and n.base == 'pcie'):
            r = n.reg()
            wifi.append({'node': n.path, 'compatible': n.string('compatible'), 'status': n.string('status', 'okay'),
                         'reg': hx(r[0][0]) if r else None})
    out.put('wifi.nodes', wifi, 'icnss/cnss/PCIe/Broadcom nodes')
    if wifi:
        ok = [w for w in wifi if w['status'] in ('okay', 'ok')]
        kinds = ' '.join(w['compatible'] or '' for w in (ok or wifi))
        out.put('wifi.kind', 'icnss' if 'icnss' in kinds else
                'wpss' if ('wcn6750' in kinds or 'wpss' in kinds) else
                'pcie' if ('qca6' in kinds or 'pci' in kinds or 'wcn6855' in kinds or 'wcn7850' in kinds) else
                'brcm' if 'bcm' in kinds else 'mediatek' if 'mediatek' in kinds else 'unknown',
                'from the enabled wifi nodes (wpss: WCN6750 behind its own DSP, ath11k; icnss: WCN399x)')
    # ---- is this the merged tree? ----
    if re.search(r'Technologies, Inc\.|\bSoC$|\bMTP\b|\bQRD\b', model) and not out.evidence.get('touch.compatible'):
        out.note('this looks like the SoC BASE tree (model "%s"): board parts (touch, keys, power key, panel) come '
                 'from DTBO overlays at boot. Use the merged live tree: adb shell su -c "cat /sys/firmware/fdt"' % model)
    return out


def flatten(d, prefix=''):
    """Every scalar leaf of nested dicts (lists are compared whole elsewhere)."""
    for k, v in d.items():
        k = str(k)
        if k.startswith('_'):
            continue
        p = f'{prefix}.{k}' if prefix else k
        if isinstance(v, dict):
            yield from flatten(v, p)
        else:
            yield p, v


def norm(v):
    if isinstance(v, str) and re.fullmatch(r'0x[0-9a-fA-F]+', v):
        return int(v, 16)
    return v


def compare(cand, ref):
    """(path, extracted, profile, verdict): OK, DIFF, or CHECK for a '?' candidate the profile refines."""
    rows = []
    for path, value in flatten(cand):
        want = profile_spec.get(ref, path)
        if want is None or isinstance(value, (list, dict)) or isinstance(want, (list, dict)):
            continue
        if norm(value) == norm(want):
            verdict = 'OK'
        elif isinstance(value, str) and isinstance(want, str) and (
                value.endswith('?') and want.startswith(value[:-1]) or value.startswith(want) or want.startswith(value)):
            verdict = 'CHECK'
        else:
            verdict = 'DIFF'
        rows.append((path, value, want, verdict))
    return rows


# Qualcomm platform code names in vendor trees -> part numbers
CODENAMES = {'msmnile': 'sm8150', 'kona': 'sm8250', 'lahaina': 'sm8350', 'waipio': 'sm8450', 'kalama': 'sm8550',
             'pineapple': 'sm8650', 'sdm845': 'sdm845', 'lito': 'sm7250', 'atoll': 'sm7125', 'lagoon': 'sm6350',
             'bengal': 'sm6115', 'trinket': 'sm6125', 'yupik': 'sm7325', 'sdmmagpie': 'sm7150'}


def detect_soc(tree):
    """The SoC part number from the root compatible (qcom,sm8150 / qcom,kona / mediatek,mt6893 / google,gs101 ...)."""
    comp = ' '.join(tree.root.strings('compatible'))
    m = re.search(r'qcom,(sm\d{4}|sdm\d{3}|sc\d{4}\w*|msm\d{4}|qcm\d{4}|x1e\d+)', comp)
    if m:
        return m.group(1)
    for name, soc in CODENAMES.items():
        if f'qcom,{name}' in comp:
            return soc
    m = re.search(r'mediatek,(mt\d{4}\w?)|google,(gs\d{3}|zuma\w*)|samsung,(exynos\d+|s5e\d+)|sprd,(\w+)|unisoc,(\w+)', comp)
    if m:
        return next(g for g in m.groups() if g)
    return None


def soc_data(soc, socs_dir):
    """socs/<soc>.json, or the SoC it is an alias of (socs/aliases.json: sm7325 -> sc7280, ...)."""
    if not soc:
        return None, None
    path = socs_dir / f'{soc}.json'
    if path.exists():
        return soc, json.loads(path.read_text())
    aliases = socs_dir / 'aliases.json'
    if aliases.exists():
        target = json.loads(aliases.read_text()).get(soc)
        if target and (socs_dir / f'{target}.json').exists():
            return target, json.loads((socs_dir / f'{target}.json').read_text())
    return None, None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('dts', type=pathlib.Path)
    p.add_argument('--soc', help='sm8150, sm8250, ... (default: from / compatible)')
    p.add_argument('--socs-dir', type=pathlib.Path, default=HERE.parent / 'socs')
    p.add_argument('-o', '--out', type=pathlib.Path, help='write the candidate profile + evidence here')
    p.add_argument('--compare', type=pathlib.Path, help='a hand-made profile to check the candidates against')
    args = p.parse_args()
    tree = dtslib.load(args.dts)
    soc = args.soc or detect_soc(tree)
    data_soc, socdata = soc_data(soc, args.socs_dir)
    out = extract(tree, socdata, soc)
    if data_soc and data_soc != soc:
        out.note(f'SoC {soc}: using socs/{data_soc}.json (aliases.json: the same silicon/family)')
    if socdata is None:
        out.note(f'no socs/{soc}.json: tiles, mux indexes and clock registers are unresolved '
                 '(run tools/qcom_socdata.py on a Linux tree)')
    result = {'_generated_by': 'toolkit/tools/extract_profile.py', '_source_dts': args.dts.name,
              'candidate': out.profile, 'evidence': out.evidence, 'notes': out.notes}
    text = json.dumps(result, indent=1)
    if args.out:
        args.out.write_text(text + '\n', newline='\n')
        print(f'wrote {args.out}: {len(out.evidence)} values')
    else:
        print(text)
    for n in out.notes:
        print('note:', n, file=sys.stderr)
    if args.compare:
        ref = profile_spec.load(args.compare, local=False)
        rows = compare(out.profile, ref)
        count = {v: sum(1 for r in rows if r[3] == v) for v in ('OK', 'CHECK', 'DIFF')}
        print(f'\ncompare with {args.compare.name}: {count["OK"]} agree, {count["CHECK"]} refined by hand, '
              f'{count["DIFF"]} differ (of {len(rows)} scalar values both have)', file=sys.stderr)
        for path, got, want, verdict in rows:
            print(f'  {verdict:5} {path}: extracted {got!r}, profile {want!r}', file=sys.stderr)
        return 0 if count['DIFF'] == 0 else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
