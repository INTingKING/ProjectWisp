#!/usr/bin/env python3
"""Contact sheet for portrait phone shots: phone_sheet.py DIR -> DIR/contact.png (6 per row, 1/4 scale, labelled)."""
import os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
files = sorted(f for f in os.listdir(d) if f.endswith(".png") and f != "contact.png")
tw, th, cols, lab = 270, 585, 6, 22
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (tw + 8) + 8, max(1, rows) * (th + lab + 8) + 8), (8, 9, 12))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(os.path.join(d, f)).convert("RGB").resize((tw, th), Image.LANCZOS)
    x, y = 8 + (i % cols) * (tw + 8), 8 + (i // cols) * (th + lab + 8)
    sheet.paste(im, (x, y + lab)); dr.text((x, y + 4), f[:-4], fill=(200, 200, 200))
sheet.save(os.path.join(d, "contact.png")); print("contact sheet:", len(files), "shots")
