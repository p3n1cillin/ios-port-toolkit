"""Turn a twin iOS run's frames (screen-<N>s.png) into boot.gif and timeline.png. EMULATION ONLY.

usage: python make_timeline.py port/build/twin-ios/<run dir>       (needs Pillow)
"""
import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

d = pathlib.Path(sys.argv[1])
frames = sorted(((int(m.group(1)), p) for p in d.glob('screen-*s.png')
                 if (m := re.fullmatch(r'screen-(\d+)s\.png', p.name))), key=lambda t: t[0])
assert frames, 'no screen-<N>s.png frames'
try:
    font = ImageFont.truetype('arial.ttf', 22)
except OSError:
    font = ImageFont.load_default()


def labelled(sec, path):
    im = Image.open(path).convert('RGB')
    bar = Image.new('RGB', (im.width, 34), (24, 24, 24))
    ImageDraw.Draw(bar).text((10, 5), f'{sec // 60}:{sec % 60:02d} into the boot', fill=(235, 235, 235), font=font)
    out = Image.new('RGB', (im.width, im.height + bar.height))
    out.paste(bar, (0, 0))
    out.paste(im, (0, bar.height))
    return out


images = [labelled(s, p) for s, p in frames]
images[0].save(d / 'boot.gif', save_all=True, append_images=images[1:] + [images[-1]] * 4, duration=450, loop=0)

# timeline: up to 8 frames chosen where the picture changes
picked, last = [], None
for (s, p), im in zip(frames, images):
    sig = im.resize((24, 48)).tobytes()
    if last is None or sum(abs(a - b) for a, b in zip(sig, last)) > 4000:
        picked.append(im)
        last = sig
picked = picked[:7] + ([images[-1]] if images[-1] is not picked[-1] else [])
w, h = picked[0].width // 2, picked[0].height // 2
sheet = Image.new('RGB', (w * len(picked) + 8 * (len(picked) + 1), h + 16), (12, 12, 12))
for i, im in enumerate(picked):
    sheet.paste(im.resize((w, h)), (8 + i * (w + 8), 8))
sheet.save(d / 'timeline.png')
print(f'{len(frames)} frames -> {d / "boot.gif"}; {len(picked)} distinct -> {d / "timeline.png"}')
