"""Generate one test's build/bundle scripts from a device profile and a test spec.

This replaces the copy-and-edit genNN.py scripts: instead of copying test N-1's files and patching
strings, every per-test file is rendered from a template (templates/<template>/, made from a real
hardware-tested test by templatize.py) with the device's values from its profile and the test's from
its spec.

    python gen_test.py --device ../devices/r5q.json --spec ../tests/r5q/test69.json --out <dir>

Writes into <dir> (mirroring the work tree, nothing outside it):
  build_testN_shim.py   package_testN.py   test_testN_shim.py   run_testN_checks.py
  check_testN.sh        bundle_testN.sh    port/xnuloader/build-testN.sh
  runner/markers-testN.ps1.txt   the run_stage_test_auto.ps1 case block (paste it before the previous one)
  runner/testN.json              markers/timeout/anchor/USB files for toolkit/runner/run_test.py
  stage_testN.py                 copies the artefacts (FD, kernel, root, port dir) into the work tree and
                                 registers --testN with build_loader_linux.py; run it yourself, once
  NEXT-testN.md                  the commands to build, check, bundle and (only with the owner's OK) run

A spec may say "extends": "test68.json": it inherits everything and overrides what it lists, which is
how the next S10 test is written (tests/r5q/test69.example.json).

Nothing here touches a phone. Writing to the work tree happens only when you run stage_testN.py.
"""
import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import profile_spec as spec  # noqa: E402
from profile_spec import get, num  # noqa: E402

BS = chr(92)   # a backslash, kept out of literals: the rendered Python/sh sources carry them as text


def load_spec(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text(encoding='utf-8'))
    if 'extends' in data:
        base = load_spec(path.parent / data['extends'])
        over = {k: v for k, v in data.items() if k != 'extends'}
        data = spec.merge(base, over)
    return data


def hexu(v, width=0):
    return '0x' + format(v, 'X').rjust(width, '0')


def hexl(v, width=0):
    return '0x' + format(v, 'x').rjust(width, '0')


class Ctx:
    def __init__(self, profile, test):
        self.p, self.t = profile, test
        self.N = test['number']
        self.problems = []

    def need(self, path):
        v = get(self.p, path)
        if v is None:
            self.problems.append(f'profile is missing {path}')
        return v

    def fmt_values(self):
        p, t = self.p, self.t
        sysbytes = get(p, 'userland.system_image_bytes') or 0
        res = num(get(p, 'memory.resident_image.base')) or 0
        return {
            'N': self.N, 'previous': t.get('previous', self.N - 1), 'timed': t.get('timed', 0),
            'panel_last_row': (get(p, 'display.height') or 1) - 1,
            'system_bytes': sysbytes, 'system_hex': format(sysbytes, 'x'), 'resident_hex': format(res, 'x'),
            'vtimer': get(p, 'gic.vtimer_intid'), 'prefix': get(p, 'device.prefix'),
        }

    def placeholders(self):
        p, t = self.p, self.t
        need = self.need
        fd_base, fd_size = num(need('memory.uefi_fd.base')), num(need('memory.uefi_fd.size'))
        pay_base, pay_max = num(need('memory.payload.base')), num(need('memory.payload.max'))
        ring, ring_size = num(need('memory.log_ring.base')), num(need('memory.log_ring.size'))
        fb = num(need('memory.display_reserved.base'))
        w, h, stride = need('display.width'), need('display.height'), need('display.stride')
        wc = need('memory.loader_window_check') or {}
        magic = need('log_slot.magic') or ''
        local = p.get('local', {})
        if not local.get('work_wsl'):
            self.problems.append('the local overlay (<codename>.local.json) is missing local.work_wsl')
        if not local.get('backups_dir'):
            self.problems.append('the local overlay is missing local.backups_dir')
        if get(p, 'bootimg.header_version') not in (None, 2):
            self.problems.append('package template handles Android boot image header v2 only (recovery.img); '
                                 'v3/v4 needs a package variant')
        if None in (fd_base, fd_size, pay_base, pay_max, ring, ring_size, fb, w, h, stride):
            return None
        frame = stride * h
        return {
            'N': str(self.N), 'PREFIX': need('device.prefix') or '',
            'FD_BASE': hexu(fd_base, 8), 'FD_SIZE': hexl(fd_size), 'fd_base': hexl(fd_base, 8), 'fd_size': hexl(fd_size),
            'PAYLOAD_BASE': hexu(pay_base, 8), 'PAYLOAD_MAX': hexu(pay_max, 8), 'payload_base': hexl(pay_base, 8),
            'payload_max_short': hexl(pay_max), 'payload_outside': hexl(pay_max - 0x200000),
            'PAYLOAD_LIMIT': hexu(pay_max - 0x100000, 8),
            'kernel_load_pa': hexl(num(need('bootimg.kernel_load_pa')) or 0, 8),
            'mdss_base': hexl(num(need('display.mdss_base')) or 0, 8),
            'bootshim_info': hexl(num(need('memory.bootshim_info')) or 0, 8),
            'log_ring': hexl(ring, 8), 'ring_size': hexl(ring_size), 'ring_map': hexl(ring_size + 0x1000),
            'ring_last': hexl(ring_size + 0xfff), 'ring_magic': hexl(num(need('memory.log_ring.magic')) or 0, 8),
            'fb_base': hexl(fb, 8), 'frame_map': hexl((frame + 0xFFFFF) & ~0xFFFFF),
            'STRIDE': str(stride), 'WIDTH': str(w), 'HEIGHT': str(h),
            'WINDOW_CHECK': f'{hexl(num(wc.get("base")) or 0)}, {hexl(num(wc.get("size")) or 0)}',
            'BACKUPS_DIR': local.get('backups_dir', ''), 'WORK_WSL': local.get('work_wsl', ''),
            'RECOVERY_SHA256': need('baseline.recovery_sha256') or '',
            'PAGE_SIZE': str(need('bootimg.page_size') or 4096),
            'SLOT_OFFSET': hexl(num(need('log_slot.offset')) or 0), 'SLOT_BLOCKS': str(need('log_slot.blocks') or 0),
            'SLOT_MAGIC': magic, 'SLOT_PAD': str(16 - len(magic)),
            'loader_pe_base': hexl(num(need('memory.loader_pe_base')) or 0, 8),
            'FIRMWARE_NUMBER': str(t.get('firmware_number', self.N)), 'MODE': t.get('mode', 'diagnostic'),
            'NOTES': t.get('notes', ''), 'START_BLOCKED_REASON': t.get('start_blocked_reason', ''),
            'PAYLOAD_ITEMS': self.payload_items(), 'LOADER_COMMENT': '\n'.join(t['loader']['comment']),
            'LOADER_FLAGS': self.loader_flags(),
        }

    def payload_items(self):
        pl, N, fv = self.t['payload'], self.N, self.fmt_values()
        lines = [f"body = item('loader.efi', loader) + item('kernel', (build / 'test{N}-xnu.macho').read_bytes())"]
        rd = pl.get('ramdisk')
        if rd:
            line = f"body += item('ramdisk', (build / '{rd['file'].format(**fv)}').read_bytes())"
            if rd.get('comment'):
                line += '   # ' + rd['comment']
            lines.append(line)
        if pl.get('resident_system_image'):
            sysbytes = get(self.p, 'userland.system_image_bytes')
            res = num(get(self.p, 'memory.resident_image.base'))
            if not sysbytes or res is None:
                self.problems.append('resident_system_image needs userland.system_image_bytes and memory.resident_image.base')
                sysbytes, res = sysbytes or 0, res or 0
            lines.append(f"body += item('rdres', b'{sysbytes:x}{BS}0')   # system.apfs ({sysbytes} bytes), "
                         f"put at {hexu(res, 8)} by the firmware's USB RAM drive")
        cmd = pl['cmdline'].replace('%d', '\0').format(**fv).replace('\0', '%d')
        tail = " % int(time.time())" if '%d' in cmd else ''
        lines.append(f"body += item('checkpoint', b'{pl.get('checkpoint', 0)}{BS}0') + item('cmdline', "
                     f"b'{cmd}{BS}0'{tail}) + bytes({pl.get('trailing_zero_bytes', 24)})")
        return '\n'.join(lines)

    def loader_flags(self):
        prefix = get(self.p, 'loader.flag_prefix') or ''
        fv = self.fmt_values()
        out = []
        for f in self.t['loader']['flags']:
            f = f.format(**fv)
            if '"' in f:
                out.append('"-D' + prefix + f.replace('"', BS + '"') + '"')
            else:
                out.append('-D' + prefix + f)
        return ' '.join(out) + ' '

    def markers(self):
        fv = self.fmt_values()
        return [[m.format(**fv) for m in line] for line in self.t['runner']['markers']]

    def markers_block(self):
        lines = self.markers()
        q = lambda m: "'" + m.replace("'", "''") + "'"
        out = [f"    {self.N} {{   # {self.t['runner'].get('comment', '')}"]
        for i, line in enumerate(lines):
            text = ', '.join(q(m) for m in line)
            lead = '        $markers = @(' if i == 0 else ' ' * 21
            out.append(lead + text + (') }' if i == len(lines) - 1 else ','))
        return '\n'.join(out)


def render(template: str, values: dict) -> str:
    for k, v in values.items():
        template = template.replace(f'@@{k}@@', v)
    if '@@' in template:
        left = sorted(set(template.split('@@')[1::2]))
        raise SystemExit(f'unfilled placeholders: {left}')
    return template


STAGE = '''"""Stage Test {N}'s artefacts in the work tree (generated by toolkit/generator/gen_test.py).

Copies what the spec names; never overwrites a file of another test; touches no phone.
Run from anywhere:  python stage_test{N}.py
"""
import pathlib
import shutil

W = pathlib.Path({work!r})
N, P = {N}, {previous}
copies = {copies!r}
for src, dst in copies:
    s, d = W / src, W / dst
    if d.exists() and d.read_bytes() == s.read_bytes():
        continue
    if d.exists() and not dst.startswith('port/test{N}/'):
        raise SystemExit(f'refusing to overwrite {{d}}')
    d.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(s, d)
    print('staged', dst)
port_from = W / {port_from!r}
port_to = W / 'port/test{N}'
if not port_to.exists():
    shutil.copytree(port_from, port_to)
for src in {refresh!r}:
    shutil.copyfile(W / src, port_to / pathlib.Path(src).name)
# the BootShim defines must be this device's (a new device: they are patched here, r5q: unchanged)
shim = port_to / 'BootShim.S'
text = shim.read_bytes().decode()
for name, value in {defines!r}:
    import re
    new = re.sub(r'(#define ' + name + r' )0x[0-9A-Fa-f]+', lambda m: m.group(1) + value, text)
    text = new
shim.write_bytes(text.encode())
# the loader build profile, written by gen_test.py, and its build_loader_linux.py option
for name in ['build-test{N}.sh']:
    shutil.copyfile(pathlib.Path(__file__).with_name('port') / 'xnuloader' / name, W / 'port/xnuloader' / name)
b = W / 'build_loader_linux.py'
t = b.read_text()
if "('--test{N}',)" not in t:
    anchor = "('--test{previous}',): 'build-test{previous}.sh', "
    assert anchor in t, 'build_loader_linux.py has no --test{previous} entry to add --test{N} after'
    b.write_text(t.replace(anchor, anchor + "('--test{N}',): 'build-test{N}.sh', "), newline='\\n')
    print('build_loader_linux.py: --test{N} added')
# the per-test scripts next to the work tree's own
for name in {scripts!r}:
    src = pathlib.Path(__file__).with_name(name)
    dst = W / name
    if dst.exists() and dst.read_bytes() != src.read_bytes():
        raise SystemExit(f'refusing to overwrite a different {{dst}}')
    shutil.copyfile(src, dst)
print('Test {N} staged in', W)
'''

NEXT = '''# Test {N} on {name} ({codename}): what to do next

Generated by `toolkit/generator/gen_test.py` from `{spec}` and the {codename} profile.

1. Stage the artefacts and scripts into the work tree (copies only; refuses to overwrite another test's files):
   `python stage_test{N}.py`
2. Build and check in WSL (the loader, the shim, the Unicorn shim test):
   `bash {work_wsl}/check_test{N}.sh`
3. Package and bundle (the guarded bundle refuses to overwrite an existing one):
   `bash {work_wsl}/bundle_test{N}.sh`
4. Runner: paste `runner/markers-test{N}.ps1.txt` into `run_stage_test_auto.ps1` before the case for Test {previous},
   raise its `ValidateRange` upper bound to {N}{ios_note}.
5. Hardware run: ONLY with the owner's explicit OK for this one run (or a standing authorization recorded in
   PHONE-STATE.json), one run at a time, recovery partition only, automatic restore. See toolkit/SAFETY.md.
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', type=pathlib.Path, required=True)
    ap.add_argument('--spec', type=pathlib.Path, required=True)
    ap.add_argument('--out', type=pathlib.Path, required=True)
    ap.add_argument('--no-local', action='store_true', help='ignore <codename>.local.json')
    args = ap.parse_args()
    profile = spec.load(args.device, local=not args.no_local)
    test = load_spec(args.spec)
    ctx = Ctx(profile, test)
    values = ctx.placeholders()
    if ctx.problems or values is None:
        for pr in ctx.problems:
            print('cannot generate:', pr, file=sys.stderr)
        return 2
    tdir = HERE / 'templates' / test.get('template', 'qcom-userland')
    N = ctx.N
    out = args.out
    written = []
    for tmpl in sorted(tdir.glob('*.tmpl')):
        name = tmpl.name[:-5].replace('@@N@@', str(N))
        dest = out / ('port/xnuloader/' + name if name.startswith('build-test') else name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(render(tmpl.read_bytes().decode(), values).encode())
        written.append(dest)
    (out / 'runner').mkdir(parents=True, exist_ok=True)
    block = ctx.markers_block()
    (out / 'runner' / f'markers-test{N}.ps1.txt').write_bytes((block + '\n').encode())
    flat = [m for line in ctx.markers() for m in line]
    r = test['runner']
    fv = ctx.fmt_values()
    runner = {'number': N, 'device': get(profile, 'device.codename'), 'markers': flat,
              'anchor': flat[r.get('anchor_index', 2)], 'timeout': r.get('timeout', 300),
              'log_slot': hexl(num(get(profile, 'log_slot.offset'))) if r.get('log_slot') else None,
              'usb_files': ([f"{get(profile, 'userland.system_image')}=SYSTEM.APF"] if r.get('usb_system_image') else []),
              'usb_label': get(profile, 'firmware.usb_msd_label')}
    (out / 'runner' / f'test{N}.json').write_bytes((json.dumps(runner, indent=1) + '\n').encode())
    a = test.get('artefacts', {})
    prefix = get(profile, 'device.prefix')
    copies = []
    if a.get('firmware_fd'):
        copies.append((a['firmware_fd'].format(**fv), f'port/build/{prefix}-test{N}-uefi.fd'))
    if a.get('kernel'):
        copies.append((a['kernel'].format(**fv), f'port/build/test{N}-xnu.macho'))
    if a.get('root'):
        copies.append((a['root'].format(**fv), f'port/build/test{N}-root.apfs'))
    defines = [('MDSS', hexu(num(get(profile, 'display.mdss_base')), 8)),
               ('INFO', hexu(num(get(profile, 'memory.bootshim_info')), 8)),
               ('SEC_LOG_BASE', hexu(num(get(profile, 'memory.log_ring.base')), 8))]
    scripts = [w.name for w in written if w.parent == out]
    stage = STAGE.format(N=N, previous=fv['previous'], work=profile.get('local', {}).get('work_win', ''),
                         copies=copies, port_from=a.get('port_dir_from', 'port/test{previous}').format(**fv),
                         refresh=a.get('port_dir_refresh', []), defines=defines, scripts=scripts)
    (out / f'stage_test{N}.py').write_bytes(stage.encode())
    ios_note = ', and add it to `$iosRuns` (it takes the system image over USB)' if r.get('usb_system_image') else ''
    (out / f'NEXT-test{N}.md').write_bytes(NEXT.format(
        N=N, previous=fv['previous'], name=get(profile, 'device.name'), codename=get(profile, 'device.codename'),
        spec=args.spec.name, work_wsl=values['WORK_WSL'], ios_note=ios_note).encode())
    for w in written:
        print('wrote', w.relative_to(out))
    print(f'wrote runner/markers-test{N}.ps1.txt, runner/test{N}.json, stage_test{N}.py, NEXT-test{N}.md')
    return 0


if __name__ == '__main__':
    sys.exit(main())
