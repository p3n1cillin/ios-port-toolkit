"""Check the toolkit against the real S10 Lite port. Host-only: reads work/ and outputs/, writes only a temp dir.

    python selftest.py [--work ../../work/ios-r5q] [--y2q-dts y2q.dts]

  1  SoC data     socs/sm8150.json gives the pins/functions/tiles/clocks the r5q drivers use by hand
  2  extractor    live.dts -> candidates agree with devices/r5q.json (no DIFF)
  3  validator    devices/r5q.json: every stage ready, no consistency errors
  4  generator    Tests 66, 67, 68 regenerate byte-identical to the work tree's files (7 files + runner block each)
  5  safety       the real Test 68 bundle passes check_bundle; forbidden commands are refused
  6  y2q (opt.)   the S20+ tree resolves touch clocks to the donor driver's registers
  7  donor .conf  r5q exports to a guarded, recovery-only donor profile; (opt.) y2q.json agrees with the donor's y2q.conf
  8  vendors      MediaTek (pinmux) and Tensor/Exynos (samsung,pins) fixtures resolve pins, modes and pin registers
  9  brands       fastboot-boot safety (RAM only), the redwood profile, feasibility tiers; (opt.) boot images v0-v4
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
TK = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TK / 'runner'))
import profile_spec  # noqa: E402

results = []


def case(name, ok, detail=''):
    results.append((name, ok, detail))
    print(f'{"PASS" if ok else "FAIL"}  {name}' + (f'  ({detail})' if detail else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', type=pathlib.Path, default=TK.parent / 'work' / 'ios-r5q')
    ap.add_argument('--bundles', type=pathlib.Path, default=TK.parent / 'outputs' / 'S10-Lite-native-iOS-port')
    ap.add_argument('--y2q-dts', type=pathlib.Path)
    ap.add_argument('--donor-conf-dir', type=pathlib.Path, help="a copy of the donor's common/devices/")
    ap.add_argument('--mkbootimg', type=pathlib.Path, help="AOSP mkbootimg.py (the donor's prebuilt/uefi/mkbootimg.py)")
    a = ap.parse_args()
    W = a.work

    # 1
    soc = json.loads((TK / 'socs' / 'sm8150.json').read_text())
    pins = soc['tlmm']['pins']
    want = {'55': ('south', 'qup17', 1), '57': ('south', 'qup19', 2), '87': ('east', None, None), '8': ('north', 'mdp_vsync', 1)}
    ok = all(pins[k]['tile'] == t and (f is None or pins[k]['functions'].index(f) == i) for k, (t, f, i) in want.items())
    clk = soc['gcc']['clocks']['109']
    ok = ok and clk['halt_reg'] == '0x1e144' and clk['enable_reg'] == '0x52014' and clk['enable_bit'] == 4
    case('SoC data: sm8150 touch/gauge pins, TE pin, wrap2 s0 clock', ok)

    # 2
    r = subprocess.run([sys.executable, str(HERE / 'extract_profile.py'), str(W / 'live.dts'), '--compare',
                        str(TK / 'devices' / 'r5q.json'), '-o', str(pathlib.Path(tempfile.gettempdir()) / 'r5q-cand.json')],
                       capture_output=True, text=True)
    line = next((l for l in r.stderr.splitlines() if l.startswith('compare with')), '')
    case('extractor: live.dts vs r5q profile', r.returncode == 0, line.replace('compare with r5q.json: ', ''))

    # 3
    r = subprocess.run([sys.executable, str(HERE / 'validate_profile.py'), str(TK / 'devices' / 'r5q.json')],
                       capture_output=True, text=True)
    case('validator: r5q profile complete and consistent', r.returncode == 0 and 'ERROR' not in r.stdout)

    # 4
    with tempfile.TemporaryDirectory() as tmp:
        for N in (66, 67, 68):
            out = pathlib.Path(tmp) / f'gen{N}'
            r = subprocess.run([sys.executable, str(TK / 'generator' / 'gen_test.py'), '--device', str(TK / 'devices' / 'r5q.json'),
                                '--spec', str(TK / 'tests' / 'r5q' / f'test{N}.json'), '--out', str(out)],
                               capture_output=True, text=True)
            if r.returncode:
                case(f'generator: Test {N}', False, r.stderr.strip()[:200])
                continue
            files = [f'build_test{N}_shim.py', f'package_test{N}.py', f'test_test{N}_shim.py', f'run_test{N}_checks.py',
                     f'check_test{N}.sh', f'bundle_test{N}.sh', f'port/xnuloader/build-test{N}.sh']
            diff = [f for f in files if (out / f).read_bytes() != (W / f).read_bytes()]
            runner = (W / 'run_stage_test_auto.ps1').read_bytes().decode()
            i = runner.index(f'    {N} {{')
            j = runner.index(f'\n    {N - 1} {{', i)
            block_ok = (out / 'runner' / f'markers-test{N}.ps1.txt').read_bytes().decode().rstrip('\n') == runner[i:j]
            case(f'generator: Test {N} byte-identical ({len(files) - len(diff)}/{len(files)} files, runner block '
                 f'{"same" if block_ok else "DIFFERS"})', not diff and block_ok, ', '.join(diff))

    # 5
    import safety
    prof = profile_spec.load(TK / 'devices' / 'r5q.json')
    b = a.bundles / 'test68'
    if b.exists():
        try:
            passed = safety.check_bundle(b, prof)
            case('safety: Test 68 bundle', True, f'{len(passed)} checks')
        except safety.SafetyError as e:
            case('safety: Test 68 bundle', False, str(e))
    refused = 0
    bad = ['dd if=x of=/dev/block/by-name/boot', 'dd if=x of=/dev/block/sda24', 'cat x > /dev/block/by-name/vbmeta',
           'fastboot flash boot x', 'echo 1 > /sys/kernel/debug/regmap/spmi0-00/data', 'reboot download']
    for c in bad:
        try:
            safety.assert_shell_safe(c, prof)
        except safety.SafetyError:
            refused += 1
    try:
        safety.assert_shell_safe('dd if=/data/local/tmp/r5q-test68/recovery-original.img of=/dev/block/by-name/recovery bs=4096', prof)
        allowed = True
    except safety.SafetyError:
        allowed = False
    case('safety: forbidden writes refused, the recovery restore allowed', refused == len(bad) and allowed,
         f'{refused}/{len(bad)} refused')

    # 6
    if a.y2q_dts:
        r = subprocess.run([sys.executable, str(HERE / 'extract_profile.py'), str(a.y2q_dts), '--compare',
                            str(TK / 'devices' / 'y2q.json')], capture_output=True, text=True)
        cand = json.loads(r.stdout)['candidate']
        g = cand['touch']['gcc']
        ok = (g['vote_reg'], g['se_cbcr'], g['m_ahb_cbcr'], g['s_ahb_cbcr'], g.get('core_cbcr'), g.get('core2x_cbcr')) == \
            ('0x52008', '0x175fc', '0x17004', '0x17008', '0x23000', '0x23008')
        case('y2q: touch clocks match the donor driver (vendor clock numbers ignored)', ok)

    # 8  other vendors (synthetic fixtures built from upstream SoC data)
    fx = TK / 'tests' / 'fixtures'
    for name, check in [('mt6797-synthetic.dts', lambda t: (t.get('sda_gpio'), t.get('scl_gpio'), t.get('pin_function_name'),
                                                             t.get('bus_tile')) == (38, 37, 'SDA0_0', 'pio')),
                        ('gs101-synthetic.dts', lambda t: (t.get('sda_gpio'), t.get('pin_function'),
                                                           (t.get('bus_tile') or '').endswith('0x10840200'))
                         == ('gpp16-0', 3, True))]:
        r = subprocess.run([sys.executable, str(HERE / 'extract_profile.py'), str(fx / name)], capture_output=True, text=True)
        t = json.loads(r.stdout)['candidate'].get('touch', {})
        case(f'other vendors: {name} -> I2C pins, mux and pin registers', check(t), str({k: t.get(k) for k in
             ('sda_gpio', 'scl_gpio', 'pin_function', 'bus_tile')}))

    # 9  brands and boot methods
    rw = profile_spec.load(TK / 'devices' / 'redwood.json')
    refused = 0
    for argv in (['fastboot', 'flash', 'boot', 'x'], ['fastboot', 'erase', 'userdata'], ['fastboot', 'oem', 'unlock'],
                 ['fastboot', 'set_active', 'b'], ['fastboot', 'reboot', 'bootloader'], ['fastboot', 'flashing', 'unlock']):
        try:
            safety.assert_fastboot_safe(argv, rw)
        except safety.SafetyError:
            refused += 1
    try:
        safety.assert_fastboot_safe(['fastboot', 'boot', 'test.img'], rw)
        allowed = True
    except safety.SafetyError:
        allowed = False
    case('fastboot-boot phones: only `fastboot boot` (RAM) allowed, writes refused', allowed and refused == 6,
         f'{refused}/6 refused')
    r = subprocess.run([sys.executable, str(HERE / 'validate_profile.py'), str(TK / 'devices' / 'redwood.json')],
                       capture_output=True, text=True)
    case('validator: redwood (fastboot-boot, SPI touch) has no consistency errors', 'ERROR' not in r.stdout)
    import assess_device
    tiers = {}
    for dev in ('r5q', 'y2q', 'redwood'):
        tiers[dev] = assess_device.assess(None, profile_spec.load(TK / 'devices' / f'{dev}.json', local=False))['tier']
    for name in ('mt6797-synthetic.dts', 'gs101-synthetic.dts'):
        import dtslib
        tiers[name] = assess_device.assess(dtslib.load(fx / name), None, None)['tier']
    case('feasibility tiers: the three ported SoCs A, SoCs without a UEFI D', tiers == {
        'r5q': 'A', 'y2q': 'A', 'redwood': 'A', 'mt6797-synthetic.dts': 'D', 'gs101-synthetic.dts': 'D'}, str(tiers))
    if a.mkbootimg:
        r = subprocess.run([sys.executable, str(HERE / 'test_bootimg.py'), '--mkbootimg', str(a.mkbootimg)],
                           capture_output=True, text=True)
        case('boot images v0-v4 + vendor_boot: repack == AOSP mkbootimg', r.returncode == 0,
             f'{r.stdout.count("PASS")} checks')

    # 7  donor profile format
    import conf_bridge
    text = conf_bridge.export(prof, TK / 'devices' / 'r5q.json')
    v = conf_bridge.parse_conf(text)
    ok = (v.get('KIND') == 'phone-recovery' and v.get('BOOT_VIA') == 'recovery-test' and 'xl|run.sh)' in text
          and v.get('FD_BASE') == '0x9FC00000' and v.get('PAYLOAD_BASE') == '0xF0000000' and v.get('FB_BASE') == '0x9C400000'
          and v.get('IOSPORT_RECOVERY_SHA256') == prof['baseline']['recovery_sha256'])
    case('donor .conf: r5q export is recovery-only, guarded, and carries the profile values', ok)
    if a.donor_conf_dir:
        y = profile_spec.load(TK / 'devices' / 'y2q.json', local=False)
        rows = conf_bridge.check(y, (a.donor_conf_dir / 'y2q.conf').read_text())
        case('donor .conf: y2q profile agrees with the donor\'s y2q.conf', all(r[3] for r in rows),
             f'{sum(r[3] for r in rows)}/{len(rows)} variables')

    bad = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(bad)}/{len(results)} passed')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
