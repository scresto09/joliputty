#!/usr/bin/env python3
# mkassets.py: make the logos of the MSIX package (Assets/*.png) from
# windows/putty.ico, so that they look just like the program's icon:
# each from the icon's own image of the nearest size, enlarged by a
# whole number of times (it's pixel art, which doesn't take any other
# scaling well) and centred.
#
# Run it from this directory after changing the icon, and commit the
# result (it needs Pillow, which the build doesn't, hence not done at
# build time):
#
#   python3 mkassets.py

import os
import struct
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ICON = os.path.join(HERE, "..", "putty.ico")

# Each logo the manifest names, at each size Windows asks for:
# (file, size in pixels)
LOGOS = [
    ("StoreLogo.scale-100.png", 50),
    ("StoreLogo.scale-200.png", 100),
    ("JoliPuTTY150x150Logo.scale-100.png", 150),
    ("JoliPuTTY150x150Logo.scale-200.png", 300),
    ("JoliPuTTY44x44Logo.scale-100.png", 44),
    ("JoliPuTTY44x44Logo.scale-200.png", 88),
]
# (the taskbar's and the Start menu's, at their actual sizes, with and
# without the plate behind them)
for n in (16, 24, 32, 48, 256):
    for alt in ("", "_altform-unplated"):
        LOGOS.append(("JoliPuTTY44x44Logo.targetsize-%d%s.png" % (n, alt), n))

def icon_images(path):
    """The icon's 16-colour images, by size"""
    d = open(path, "rb").read()
    images = {}
    for i in range(struct.unpack("<H", d[4:6])[0]):
        w, h, _, _, _, bpp, _, off = struct.unpack(
            "<BBBBHHII", d[6 + 16 * i:22 + 16 * i])
        if bpp != 4:
            continue
        pal = [tuple(d[off + 40 + 4 * k:off + 43 + 4 * k][::-1])
               for k in range(16)]
        stride = ((w * 4 + 31) // 32) * 4
        px = off + 40 + 64
        mstride = ((w + 31) // 32) * 4
        mo = px + stride * h
        im = Image.new("RGBA", (w, h))
        for y in range(h):
            row = px + (h - 1 - y) * stride
            mrow = mo + (h - 1 - y) * mstride
            for x in range(w):
                if (d[mrow + x // 8] >> (7 - x % 8)) & 1:
                    continue
                b = d[row + x // 2]
                k = b >> 4 if x % 2 == 0 else b & 15
                im.putpixel((x, y), pal[k] + (255,))
        images[w] = im
    return images

def logo(images, n):
    # (the biggest image that fills at least 70% of it, enlarged as much
    # as fits; or failing that, whichever comes out biggest)
    fits = [(s * (n // s), s) for s in images if s <= n]
    good = [f for f in fits if f[0] >= 0.7 * n]
    size, s = max(good, key=lambda f: f[1]) if good else max(fits)
    im = images[s].resize((size, size), Image.NEAREST)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.alpha_composite(im, ((n - size) // 2, (n - size) // 2))
    return out

images = icon_images(ICON)
for name, n in LOGOS:
    logo(images, n).save(os.path.join(HERE, "Assets", name))
