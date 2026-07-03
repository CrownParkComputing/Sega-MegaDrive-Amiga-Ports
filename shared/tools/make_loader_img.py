#!/usr/bin/env python3
"""make_loader_img.py <in.png> <out.img> [w h]

Convert artwork into the mdloader background format:
  uint16be w, uint16be h, 256*3 palette bytes (RGB), then w*h pen bytes.
Pens 0-251 hold the quantized art; pens 252-255 are reserved for the menu:
  252 black panel, 253 white (selected), 254 grey, 255 orange accent.
"""
import sys
from PIL import Image

src = sys.argv[1]
out = sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 320
h = int(sys.argv[4]) if len(sys.argv) > 4 else 256

im = Image.open(src).convert("RGB").resize((w, h), Image.LANCZOS)
im = im.quantize(colors=252, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)

pal = im.getpalette()[: 252 * 3]
pal += [0] * (252 * 3 - len(pal))
pal += [0, 0, 0,  255, 255, 255,  150, 150, 150,  255, 150, 32]

pix = bytes(im.getdata())

with open(out, "wb") as f:
    f.write(bytes([w >> 8, w & 255, h >> 8, h & 255]))
    f.write(bytes(pal))
    f.write(pix)

print(f"wrote {out}: {w}x{h}, {len(pix)} pixels")
