"""How much of XNU's boot line a port has left, and what would be cut.

    python bootargs_budget.py phone-args.txt                       # the stored set alone
    python bootargs_budget.py phone-args.txt --reserve 140 +cam_af=0 -gps
    python bootargs_budget.py --line "debug=0x8 cpus=8 ..."        # a whole line, e.g. from the boot log

XNU keeps the boot-args in a fixed buffer (BOOT_LINE_LENGTH: 608 bytes on the iOS 16 era donor kernels, one for the
NUL), and a loader that copies into it usually stops at the end without a word: the args at the end of the line --
normally the newest ones -- are dropped or cut mid-token (`cpufreq=2` becomes `cpufre`), and the feature they turn on
silently is not there. On the S10 Lite the loader's own args (debug, the timer, UFS and DMA windows, rmtfs ...) and
the payload's command line take 230 bytes besides phone-args.txt (`--reserve 230`; Test 449's whole line, read from
its boot log, was 590 of 607 bytes: 17 left). Measure yours: `--line "<the boot-args line from the boot log>"`.
Since Test 451 the r5q loader also says it in the log: `[LAB] boot-args: N of 607 bytes used`, or
`[LAB] boot-args: FULL (...): the kernel will NOT see "..."` -- do the same in your loader.

`+key=value` adds or replaces an arg and `-key` removes one, as `phone.py cycle` takes them. Also reported: keys
given twice (which copy XNU's PE_parse_boot_argn sees depends on the kernel; do not rely on it).
"""
from __future__ import annotations

import argparse
import pathlib
import sys

BOOT_LINE_LENGTH = 608


def apply(args, changes):
    out = list(args)
    for c in changes:
        if c.startswith('+'):
            key = c[1:].split('=', 1)[0]
            out = [a for a in out if a.split('=', 1)[0] != key] + [c[1:]]
        elif c.startswith('-'):
            out = [a for a in out if a.split('=', 1)[0] != c[1:]]
        else:
            out.append(c)
    return out


def check(args, reserve, limit):
    usable = limit - 1 - reserve
    line = ' '.join(args)
    used = len(line.encode())
    report = {'used': used, 'usable': usable, 'left': usable - used, 'cut': [], 'duplicates': []}
    seen = {}
    for a in args:
        k = a.split('=', 1)[0]
        if k in seen:
            report['duplicates'].append(k)
        seen[k] = a
    if used > usable:
        at = 0
        for a in args:
            end = at + len(a.encode())
            if end > usable:
                report['cut'].append(a if at >= usable else f'{a} (becomes "{a[:usable - at]}")')
            at = end + 1
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', nargs='?', type=pathlib.Path, help='a stored boot-args file (one line)')
    ap.add_argument('changes', nargs='*', help='+key=value / -key, as phone.py cycle takes them')
    ap.add_argument('--line', help='a whole boot line instead of a file')
    ap.add_argument('--reserve', type=int, default=0, help="bytes the loader's own args take in front (r5q: ~140)")
    ap.add_argument('--limit', type=int, default=BOOT_LINE_LENGTH)
    a = ap.parse_args(argv)
    if a.line is None and a.file is None:
        ap.error('give a file or --line')
    text = a.line if a.line is not None else a.file.read_text(encoding='utf-8')
    args = apply(text.split(), a.changes)
    r = check(args, a.reserve, a.limit)
    print(f'{len(args)} args, {r["used"]} bytes of {r["usable"]} usable ({a.limit} - 1 NUL - {a.reserve} reserved): '
          f'{r["left"]} left')
    for k in r['duplicates']:
        print(f'WARNING: {k} is given more than once')
    if r['cut']:
        print('OVER: the kernel would not see:')
        for c in r['cut']:
            print(f'  {c}')
        return 1
    if r['left'] < 40:
        print('WARNING: under 40 bytes left: one more feature arg or a longer value and the end of the line is cut')
    return 0


if __name__ == '__main__':
    sys.exit(main())
