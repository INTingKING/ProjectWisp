#!/usr/bin/env python3
"""contact_sheet.py DIR: tile every NN-*.png in DIR into DIR/contact.png with captions."""
import glob, os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
files = sorted(f for f in glob.glob(os.path.join(d, "[0-9]*.png")))
tw, th, cols, cap = 400, 250, 4, 18
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * tw, rows * (th + cap)), (15, 17, 21))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB"); im.thumbnail((tw - 8, th - 8))
    x, y = (i % cols) * tw, (i // cols) * (th + cap)
    sheet.paste(im, (x + 4, y + 4)); dr.text((x + 6, y + th - 2), os.path.basename(f), fill=(200, 205, 215))
sheet.save(os.path.join(d, "contact.png")); print("contact sheet:", len(files), "shots")
