"""Render a bundle's guarded operator (Manage-TestN.ps1) from the device profile.

    python make_operator.py --device ../devices/<codename>.json --number N -o <bundle>/Manage-TestN.ps1

The r5q bundles get theirs from bundle_native_test.py (copied from Test 9's); this is the same operator
with the device values taken from the profile, for any other phone. It refuses a profile that breaks
the safety rules (safety.assert_profile_safe).
"""
import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import profile_spec  # noqa: E402
import safety  # noqa: E402


def render(profile, number):
    safety.assert_profile_safe(profile)
    values = {'SERIAL': profile.get('local', {}).get('serial'), 'PREFIX': profile['device']['prefix'],
              'N': str(number), 'MODEL': profile['device']['model'],
              'RECOVERY_NODE': profile['flash']['recovery_block_device'],
              'MIN_BATTERY': str(profile['flash'].get('min_battery_percent', 30))}
    missing = [k for k, v in values.items() if not v]
    if missing:
        raise SystemExit(f'profile/local overlay lacks: {missing}')
    text = (HERE / 'operator.ps1.tmpl').read_text()
    for k, v in values.items():
        text = text.replace(f'@@{k}@@', v)
    assert '@@' not in text
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--device', type=pathlib.Path, required=True)
    ap.add_argument('--number', type=int, required=True)
    ap.add_argument('-o', '--out', type=pathlib.Path, required=True)
    a = ap.parse_args()
    a.out.write_text(render(profile_spec.load(a.device), a.number), newline='\r\n')
    print('wrote', a.out)


if __name__ == '__main__':
    main()
