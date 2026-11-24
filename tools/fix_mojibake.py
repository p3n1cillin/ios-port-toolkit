"""Repair notes whose UTF-8 was read as Windows-1252 and saved again ("â€¦" for "…", "Ã¢â‚¬Â¦" after two rounds).

    python fix_mojibake.py FILE [FILE ...]          # report what would change
    python fix_mojibake.py --write FILE [FILE ...]  # repair in place (line endings kept)

Windows tools (PowerShell 5's Set-Content, some editors) do this to Markdown notes; each round makes it worse.
A run of suspicious characters is re-encoded as cp1252 and decoded as UTF-8, up to three rounds, and only kept when
that decodes cleanly. Text that is legitimately non-ASCII is left alone.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SUSPECT = re.compile(r'[Â-ÃâÅËŒœŠšŸŽžƒˆ˜'
                     r'–—‘-„†-•…‰‹›€™\u0080-¿]{2,}')


def _cp1252_bytes(s):
    out = bytearray()
    for ch in s:
        try:
            out += ch.encode('cp1252')
        except UnicodeEncodeError:
            if ord(ch) < 256:           # the five bytes cp1252 leaves undefined come back as U+0081 etc.
                out.append(ord(ch))
            else:
                raise
    return bytes(out)


def fix_run(run):
    cur = run
    for _ in range(3):
        try:
            nxt = _cp1252_bytes(cur).decode('utf-8')
        except (UnicodeEncodeError, UnicodeDecodeError):
            break
        if nxt == cur:
            break
        cur = nxt
    return cur


def fix_text(text):
    return SUSPECT.sub(lambda m: fix_run(m.group(0)), text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='+', type=pathlib.Path)
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(errors='replace')        # a Windows console's cp1252 cannot show every repair
    except AttributeError:
        pass
    changed = 0
    for f in a.files:
        raw = f.read_bytes()
        text = raw.decode('utf-8')
        new = fix_text(text)
        if new == text:
            continue
        changed += 1
        old_lines, new_lines = text.splitlines(), new.splitlines()
        for i, (o, n) in enumerate(zip(old_lines, new_lines)):
            if o != n:
                print(f'{f}:{i + 1}: {o.strip()[:90]}\n{" " * len(str(f))}   -> {n.strip()[:90]}')
        if a.write:
            f.write_bytes(new.encode('utf-8'))
    print(f'{changed} file(s) {"repaired" if a.write else "to repair (run with --write)"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
