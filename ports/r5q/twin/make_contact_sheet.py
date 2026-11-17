"""Contact sheet of chosen twin_ctl.py screenshots: python make_contact_sheet.py OUT.png FRAME.png[=label] ... (Pillow)"""
import sys
from PIL import Image, ImageDraw, ImageFont

out, items = sys.argv[1], sys.argv[2:]
try:
    font = ImageFont.truetype('arial.ttf', 20)
except OSError:
    font = ImageFont.load_default()
cols, w, h, pad, lab = 5, 270, 600, 10, 30
rows = (len(items) + cols - 1) // cols
sheet = Image.new('RGB', (cols * (w + pad) + pad, rows * (h + lab + pad) + pad), (16, 16, 16))
for i, item in enumerate(items):
    path, _, label = item.partition('=')
    im = Image.open(path).convert('RGB').resize((w, h))
    x, y = pad + (i % cols) * (w + pad), pad + (i // cols) * (h + lab + pad)
    ImageDraw.Draw(sheet).text((x + 4, y + 4), label, fill=(235, 235, 235), font=font)
    sheet.paste(im, (x, y + lab))
sheet.save(out)
print(out)
