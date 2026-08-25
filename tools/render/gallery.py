#!/usr/bin/env python3
"""Compose the rendered round screens into a device-framed contact sheet.

Each screen is dropped into a subtle brushed-metal bezel with a soft shadow and
captioned, then laid out on a grid over the brand background. This is purely a
presentation artefact for the README — it takes no screen content of its own,
only the PNGs the C renderer + rasteriser produced.

    python3 gallery.py ../../assets/screens ../../assets/branding/gallery.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter

# Curated hero set: what best shows the product, in reading order.
SCREENS = [
    ("home", "Home \u2014 is anything wrong?"),
    ("watch_camped", "Watch \u2014 FLOOD LIKELY (camped)"),
    ("watch_hopping", "Watch \u2014 SUSPICIOUS (hopping)"),
    ("browse", "Browse \u2014 what a tool is for"),
    ("detail", "Detail \u2014 the evidence, in cards"),
    ("quiet", "Watch \u2014 QUIET (nothing to do)"),
    ("karma", "Karma \u2014 KARMA/MANA rogue AP"),
    ("mirage", "Mirage \u2014 beacon-flood detection"),
    ("locate", "Locate \u2014 walk to the transmitter"),
    ("footprint", "Footprint \u2014 OPSEC (a drill)"),
    ("spectrum", "Spectrum \u2014 what the room is doing"),
    ("splash", "Boot \u2014 receive-only, up front"),
]

COLS = 3
TILE = 300          # screen diameter inside a cell
BEZEL = 16          # metal ring thickness
PAD_X = 46
PAD_Y = 40
CAP_H = 40
BG_TOP = (11, 29, 43)
BG_BOT = (4, 9, 15)
CAP_COL = (127, 166, 181)

FONT_CANDIDATES = [
    "/System/Library/Fonts/Menlo.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]
_fp = next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)


def font(px):
    try:
        return ImageFont.truetype(_fp, px) if _fp else ImageFont.load_default()
    except Exception:
        return ImageFont.load_default()


def vgradient(w, h, top, bot):
    base = Image.new("RGB", (w, h), top)
    top_i = Image.new("RGB", (w, 1), top)
    grad = Image.new("RGB", (w, h))
    for y in range(h):
        t = y / max(1, h - 1)
        col = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        grad.paste(Image.new("RGB", (w, 1), col), (0, y))
    base.paste(grad)
    return base


def framed(screen_png):
    """Wrap a round screen PNG in a metal bezel with a soft drop shadow."""
    d = TILE
    ss = 2
    D = (d + 2 * BEZEL) * ss
    plate = Image.new("RGBA", (D, D), (0, 0, 0, 0))
    dr = ImageDraw.Draw(plate)
    # bezel: two rings, a light top edge and a darker body, for a machined look
    dr.ellipse([0, 0, D - 1, D - 1], fill=(38, 46, 56, 255))
    dr.ellipse([2 * ss, 2 * ss, D - 1 - 2 * ss, D - 1 - 2 * ss], outline=(90, 104, 118, 255),
               width=2 * ss)
    inner = BEZEL * ss
    dr.ellipse([inner, inner, D - 1 - inner, D - 1 - inner], fill=(3, 8, 13, 255))

    scr = Image.open(screen_png).convert("RGBA").resize((d * ss, d * ss), Image.LANCZOS)
    plate.alpha_composite(scr, (inner, inner))
    plate = plate.resize((d + 2 * BEZEL, d + 2 * BEZEL), Image.LANCZOS)

    # drop shadow
    pad = 18
    canvas = Image.new("RGBA", (plate.width + 2 * pad, plate.height + 2 * pad), (0, 0, 0, 0))
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.ellipse([pad + 3, pad + 8, pad + plate.width + 3, pad + plate.height + 8],
               fill=(0, 0, 0, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    canvas.alpha_composite(shadow)
    canvas.alpha_composite(plate, (pad, pad))
    return canvas


def main():
    srcdir = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else "gallery.png"

    tiles = []
    for key, cap in SCREENS:
        p = os.path.join(srcdir, key + ".png")
        if os.path.exists(p):
            tiles.append((framed(p), cap))
    if not tiles:
        print("no screens found in", srcdir)
        return 1

    rows = (len(tiles) + COLS - 1) // COLS
    cell_w = tiles[0][0].width + PAD_X
    cell_h = tiles[0][0].height + CAP_H + PAD_Y
    W = COLS * cell_w + PAD_X
    H = rows * cell_h + PAD_Y

    img = vgradient(W, H, BG_TOP, BG_BOT).convert("RGBA")
    dr = ImageDraw.Draw(img)
    cap_font = font(19)

    for i, (tile, cap) in enumerate(tiles):
        r, c = divmod(i, COLS)
        x = PAD_X + c * cell_w
        y = PAD_Y + r * cell_h
        img.alpha_composite(tile, (x, y))
        dr.text((x + tile.width // 2, y + tile.height + CAP_H // 2 - 4), cap,
                font=cap_font, fill=CAP_COL, anchor="mm")

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    img.convert("RGB").save(out)
    print("  wrote", out, f"({W}x{H}, {len(tiles)} screens)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
