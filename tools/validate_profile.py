"""Check a device profile: what is missing for each bring-up stage, and what does not add up.

    python validate_profile.py ../devices/r5q.json                 # full checklist
    python validate_profile.py ../devices/y2q.json --stage boot    # exit 1 if 'boot' is not ready
    python validate_profile.py ../devices/r5q.json --dts live.dts  # also cross-check against the live tree

Checks beyond "is it filled in":
  safety   only recovery is a test partition; boot/vbmeta are in never_write; baseline hashes look like sha256
  memory   FD, payload, ring, BootShim record, loader window, kernel window, resident image, USB RAM disk:
           no overlap with each other where they must not, none inside a secure no-map region, all in DRAM
  display  stride = width * 4; each flip buffer holds a frame inside Display Reserved below reserved_end;
           the BootShim record lies outside every frame; DCS 2A/2B end at width-1 / height-1
  slot     the log slot is 4 KiB aligned and leaves the footer block of the recovery image alone
  kernel   the virtual timer INTID is a PPI (16..31)
  --dts    every no-map region of the live tree is in memory.secure_no_map (the r5q Test 40 hang)
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import profile_spec as spec  # noqa: E402
from profile_spec import get, num  # noqa: E402


def region(p, path, size_path=None, size=None):
    base = num(get(p, path))
    sz = num(get(p, size_path)) if size_path else size
    if base is None or sz is None:
        return None
    return (base, base + sz, path.rsplit('.', 1)[0] if size_path else path)


def overlap(a, b):
    return a and b and a[0] < b[1] and b[0] < a[1]


def check(p, dts=None):
    errors, warnings = [], []
    E, W = errors.append, warnings.append

    # ---- safety ----------------------------------------------------------------------------------------
    method = method_of(p)
    if method not in ('recovery', 'fastboot-boot'):
        E(f'flash.method {method!r} is not one the toolkit knows (recovery | fastboot-boot): see BRANDS.md')
    if method == 'recovery' and get(p, 'flash.test_partition') not in (None, 'recovery'):
        E('flash.test_partition must be "recovery": tests never write any other partition')
    if method == 'fastboot-boot' and get(p, 'flash.test_partition'):
        E('flash.method fastboot-boot writes no partition: flash.test_partition must be null')
    nw = get(p, 'flash.never_write') or []
    for must in ('boot', 'vbmeta'):
        if must not in nw:
            E(f'flash.never_write must list {must}')
    for k in ('recovery_sha256', 'boot_sha256', 'vbmeta_sha256', 'reboot_helper_sha256'):
        v = get(p, f'baseline.{k}')
        if v is not None and not re.fullmatch(r'[0-9a-f]{64}', str(v)):
            E(f'baseline.{k} is not a lowercase sha256')
    dev = get(p, 'flash.recovery_block_device')
    if dev and not dev.startswith('/dev/block/'):
        E('flash.recovery_block_device should be the resolved /dev/block/... node')

    # ---- memory ----------------------------------------------------------------------------------------
    ram_base = num(get(p, 'memory.dram_base')) or 0x80000000
    ram_bytes = get(p, 'device.ram_bytes')
    regions = {
        'uefi_fd': region(p, 'memory.uefi_fd.base', 'memory.uefi_fd.size'),
        'payload': region(p, 'memory.payload.base', 'memory.payload.max'),
        'display_reserved': region(p, 'memory.display_reserved.base', 'memory.display_reserved.size'),
        'log_ring': region(p, 'memory.log_ring.base', 'memory.log_ring.size'),
        'bootshim_info': region(p, 'memory.bootshim_info', size=0x1000),
        'loader_pe': region(p, 'memory.loader_pe_base', size=0x100000),
        'xnu_window': region(p, 'memory.xnu_window.base', 'memory.xnu_window.size'),
        'usb_ramdisk': region(p, 'memory.usb_ramdisk.base', 'memory.usb_ramdisk.size'),
    }
    lwc = get(p, 'memory.loader_window_check')
    if isinstance(lwc, dict) and lwc.get('base'):
        regions['loader_window_check'] = (num(lwc['base']), num(lwc['base']) + num(lwc['size']), 'loader_window_check')
    sysbytes = get(p, 'userland.system_image_bytes')
    res = num(get(p, 'memory.resident_image.base'))
    if res is not None and sysbytes:
        regions['resident_image'] = (res, res + sysbytes, 'resident_image')
    must_not_overlap = [('uefi_fd', 'payload'), ('uefi_fd', 'display_reserved'), ('uefi_fd', 'log_ring'),
                        ('payload', 'display_reserved'), ('payload', 'log_ring'), ('log_ring', 'display_reserved'),
                        ('resident_image', 'usb_ramdisk'), ('payload', 'resident_image'), ('loader_pe', 'payload'),
                        ('loader_pe', 'uefi_fd')]
    for a, b in must_not_overlap:
        if overlap(regions.get(a), regions.get(b)):
            E(f'memory: {a} {fmt(regions[a])} overlaps {b} {fmt(regions[b])}')
    for name, r in regions.items():
        if r is None:
            continue
        if name not in ('bootshim_info',) and r[0] < ram_base and name not in ('log_ring',):
            E(f'memory: {name} {fmt(r)} starts below DRAM ({ram_base:#x})')
        if ram_bytes and r[1] > ram_base + ram_bytes + 0x200000000 and name != 'xnu_window':
            W(f'memory: {name} {fmt(r)} ends past DRAM base + RAM size (DRAM may have holes: check /memory)')
    if regions.get('resident_image') and regions.get('xnu_window'):
        r, w = regions['resident_image'], regions['xnu_window']
        if not (w[0] <= r[0] and r[1] <= w[1]):
            E(f'memory: resident image {fmt(r)} is not inside the kernel window {fmt(w)}')
    if regions.get('bootshim_info') and regions.get('display_reserved'):
        if not overlap(regions['bootshim_info'], regions['display_reserved']):
            W('memory.bootshim_info is outside Display Reserved (BootShim expects its record there)')
    for nm in get(p, 'memory.secure_no_map') or []:
        s = (num(nm['base']), num(nm['base']) + num(nm['size']), nm.get('name'))
        for name in ('uefi_fd', 'payload', 'loader_pe', 'loader_window_check', 'resident_image',
                     'usb_ramdisk', 'bootshim_info'):   # (the log ring lives inside sec_debug on purpose)
            if overlap(regions.get(name), s):
                E(f'memory: {name} {fmt(regions[name])} lies in secure no-map region {nm.get("name")} {fmt(s)}')

    # ---- display ----------------------------------------------------------------------------------------
    w, h, stride = get(p, 'display.width'), get(p, 'display.height'), get(p, 'display.stride')
    if w and h and stride and stride != w * 4:
        W(f'display.stride {stride} != width*4 ({w * 4}): fine only for a padded framebuffer')
    if w and h and stride:
        frame = stride * h
        dr = regions.get('display_reserved')
        end = num(get(p, 'display.reserved_end')) or (dr[1] if dr else None)
        frames = []
        for i, b in enumerate(get(p, 'display.flip_buffers') or []):
            fb = (num(b), num(b) + frame, f'flip buffer {i}')
            frames.append(fb)
            if dr and not (dr[0] <= fb[0] and fb[1] <= dr[1]):
                E(f'display: {fb[2]} {fmt(fb)} not inside Display Reserved {fmt(dr)}')
            if end and fb[1] > end:
                E(f'display: {fb[2]} {fmt(fb)} runs past reserved_end {end:#x}')
        for a in range(len(frames)):
            for b in range(a + 1, len(frames)):
                if overlap(frames[a], frames[b]):
                    E(f'display: {frames[a][2]} overlaps {frames[b][2]}')
        if dr and not frames:
            frames = [(dr[0], dr[0] + frame, 'frame')]
            if frame > dr[1] - dr[0]:
                E(f'display: one {w}x{h} frame ({frame:#x}) does not fit Display Reserved')
        bi = regions.get('bootshim_info')
        for fb in frames:
            if overlap(bi, fb):
                E(f'memory.bootshim_info {fmt(bi)} is inside {fb[2]} {fmt(fb)}: the picture would overwrite it')
        dcs = get(p, 'display.panel_window_dcs') or {}
        for key, last in (('2A', w - 1), ('2B', h - 1)):
            v = dcs.get(key)
            if v:
                bs = [int(x, 16) for x in v.split()]
                if len(bs) == 4 and (bs[2] << 8 | bs[3]) != last:
                    E(f'display.panel_window_dcs {key} ends at {(bs[2] << 8 | bs[3])}, expected {last}')

    # ---- slot -------------------------------------------------------------------------------------------
    off, blocks, rb = num(get(p, 'log_slot.offset')), get(p, 'log_slot.blocks'), get(p, 'flash.recovery_bytes')
    if off is not None:
        if off % 4096:
            E('log_slot.offset must be 4 KiB aligned')
        if rb and blocks and off + blocks * 4096 > rb - 4096:
            E('log_slot runs into the AVB footer block of the recovery partition')

    # ---- kernel -----------------------------------------------------------------------------------------
    vt = get(p, 'gic.vtimer_intid')
    if vt is not None and not 16 <= vt <= 31:
        E('gic.vtimer_intid must be a PPI (16..31)')
    if vt is not None and vt != 27 and 'gic_vtimer=' not in (get(p, 'kernel.base_boot_args') or ''):
        W(f'gic.vtimer_intid is {vt} (not XNU\'s 27): kernel.base_boot_args needs gic_vtimer={vt} (r5q Tests 34-36)')

    # ---- live tree ---------------------------------------------------------------------------------------
    if dts:
        import dtslib
        tree = dtslib.load(dts)
        listed = [(num(n['base']), num(n['base']) + num(n['size'])) for n in get(p, 'memory.secure_no_map') or []]
        listed += uefiplat_reserved(p)
        rm = tree.find('/reserved-memory')
        for n in (rm.children if rm else []):
            if not n.has('no-map'):
                continue
            for a, s in n.reg():
                covered = covers(listed, a, a + s)
                fd = regions.get('uefi_fd')
                if not covered:
                    # regions the stock UEFI map already reserves are fine; say which to look at
                    W(f'live DT no-map {n.name} {a:#x}+{s:#x} is not in memory.secure_no_map: make sure the UEFI '
                      'memory map reserves it (RamManagerDxe adds what the map omits as free RAM)')
                if overlap(fd, (a, a + s)) or overlap(regions.get('payload'), (a, a + s)):
                    E(f'live DT no-map {n.name} overlaps the FD or the payload')
    return errors, warnings


def uefiplat_reserved(p):
    """Regions the phone's own uefiplat.cfg (firmware.uefiplat_cfg) keeps from the OS: anything not Conv."""
    rel = get(p, 'firmware.uefiplat_cfg')
    if not rel:
        return []
    path = (HERE.parents[1] / rel)
    if not path.exists():
        return []
    out = []
    for line in path.read_text(errors='replace').splitlines():
        m = re.match(r'\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*"([^"]*)",(.*)', line)
        if not m:
            continue
        # columns after the name: build HOB, resource type, attributes, memory type, cache attributes
        fields = [f.strip() for f in m.group(4).split(',')]
        if len(fields) >= 4 and fields[3] != 'Conv':
            base = int(m.group(1), 16)
            out.append((base, base + int(m.group(2), 16)))
    return out


def covers(intervals, lo, hi):
    """Is [lo, hi) inside the union of the intervals?"""
    at = lo
    for a, b in sorted(intervals):
        if a <= at < b:
            at = b
            if at >= hi:
                return True
    return at >= hi


def fmt(r):
    return f'{r[0]:#x}-{r[1]:#x}' if r else '?'


def method_of(p):
    """How a test reaches the phone: recovery (the default, Samsung) or fastboot-boot (RAM only)."""
    return get(p, 'flash.method') or 'recovery'


# fields that only a recovery-flashing method needs (a fastboot-boot test writes and restores nothing)
RECOVERY_ONLY = {'flash.test_partition', 'flash.recovery_block_device', 'flash.recovery_bytes',
                 'flash.requires_root', 'baseline.recovery_sha256', 'baseline.boot_sha256', 'baseline.vbmeta_sha256',
                 'baseline.reboot_helper_sha256', 'log_slot.offset', 'log_slot.blocks', 'log_slot.magic',
                 'bootimg.avb_footer', 'bootimg.seandroid_trailer'}


def not_applicable(p):
    """Fields this device does not need, given its entry method, touch bus and how its system image arrives."""
    skip = set(RECOVERY_ONLY) if method_of(p) == 'fastboot-boot' else set()
    if method_of(p) == 'fastboot-boot':
        skip |= {'memory.log_ring.base', 'memory.log_ring.size', 'memory.log_ring.magic'}   # the loader's USB link logs
    if get(p, 'touch.bus') == 'spi':
        skip |= {'touch.i2c_address', 'touch.sda_gpio', 'touch.scl_gpio'}
    sources = get(p, 'userland.system_sources') or ['usb-ramdisk']
    if 'usb-ramdisk' not in sources:
        skip |= {'memory.usb_ramdisk.base', 'memory.usb_ramdisk.size', 'firmware.usb_msd_label',
                 'firmware.usb_controller_init_event'}
    return skip


def checklist(p):
    rows = {s: [] for s in spec.STAGES}
    skip = not_applicable(p)
    for path, typ, stages, where in spec.FIELDS:
        if path in skip:
            continue
        v = get(p, path)
        missing = v is None or v == [] or v == {}
        for s in stages:
            rows[s].append((path, not missing, where))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile', type=pathlib.Path)
    ap.add_argument('--stage', choices=spec.STAGES, action='append', help='only these stages decide the exit code')
    ap.add_argument('--dts', type=pathlib.Path, help='the live device tree to cross-check reserved memory against')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()
    p = spec.load(args.profile)
    errors, warnings = check(p, args.dts)
    rows = checklist(p)
    stages = args.stage or spec.STAGES
    if args.json:
        print(json.dumps({'errors': errors, 'warnings': warnings,
                          'stages': {s: {'ready': all(ok for _, ok, _ in r), 'missing': [x for x, ok, _ in r if not ok]}
                                     for s, r in rows.items()}}, indent=1))
    else:
        print(f'{p.get("device", {}).get("name")} ({args.profile.name})')
        for s in spec.STAGES:
            r = rows[s]
            have = sum(1 for _, ok, _ in r if ok)
            mark = 'READY ' if have == len(r) else 'MISSING'
            print(f'\n[{mark}] {s}: {have}/{len(r)}')
            for path, ok, where in r:
                if not ok:
                    print(f'   - {path}: {where}')
        print()
        for e in errors:
            print('ERROR  ', e)
        for w in warnings:
            print('warning', w)
        if not errors and not warnings:
            print('no consistency problems found')
    bad = errors or any(not ok for s in stages for _, ok, _ in rows[s])
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
