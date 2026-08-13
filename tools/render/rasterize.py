#!/usr/bin/env python3
"""Rasterise a Pharos display list into round-panel PNGs.

The geometry and every number in the display list come from the firmware's own
C code (tools/render/pharos_render.c linking the real engines). This script
does nothing but turn primitives into pixels, which is why it is allowed to be
in Python: no layout decision is taken here.

Antialiasing is done by drawing at 3x and downsampling, which is both simpler
and better looking than fighting PIL's arc endpoints at 1x.

    ./pharos_render | python3 rasterize.py ../../assets/screens
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SS = 3  # supersample factor

# A monospace face whose advance is ~0.6 em, matching PD_ADVANCE_NUM/DEN in
# pharos_dial.c. If the C side says a label fits, it fits here too.
FONT_CANDIDATES = [
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
]
_font_path = next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)
_font_cache = {}


def font(px):
    key = int(px)
    if key not in _font_cache:
        if _font_path:
            try:
                _font_cache[key] = ImageFont.truetype(_font_path, key)
            except Exception:
                _font_cache[key] = ImageFont.load_default()
        else:
            _font_cache[key] = ImageFont.load_default()
    return _font_cache[key]


def to_pil_angles(start_deg, sweep):
    """Pharos measures degrees clockwise from 12 o'clock; PIL measures them
    counter-clockwise-ish from 3 o'clock. One conversion, in one place."""
    a0 = start_deg - 90.0
    return a0, a0 + sweep


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    def disc(self, cx, cy, r, col):
        s = SS
        self.d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=col)

    def ring(self, cx, cy, r, w, col):
        s = SS
        self.d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s],
                       outline=col, width=max(1, int(w * s)))

    def arc(self, cx, cy, r, w, start, sweep, col):
        s = SS
        a0, a1 = to_pil_angles(start, sweep)
        self.d.arc([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s],
                   a0, a1, fill=col, width=max(1, int(w * s)))

    def wedge(self, cx, cy, r0, r1, start, sweep, col):
        s = SS
        a0, a1 = to_pil_angles(start, sweep)
        self.d.pieslice([(cx - r1) * s, (cy - r1) * s, (cx + r1) * s, (cy + r1) * s],
                        a0, a1, fill=col)
        if r0 > 0:
            # punch the middle back out to leave an annulus sector
            self.d.pieslice([(cx - r0) * s, (cy - r0) * s, (cx + r0) * s, (cy + r0) * s],
                            a0, a1, fill=(0, 0, 0, 0))

    def line(self, x1, y1, x2, y2, w, col):
        s = SS
        self.d.line([x1 * s, y1 * s, x2 * s, y2 * s], fill=col,
                    width=max(1, int(w * s)))

    def roundrect(self, x, y, w, h, r, col):
        s = SS
        self.d.rounded_rectangle([x * s, y * s, (x + w) * s, (y + h) * s],
                                 radius=r * s, fill=col)

    def text(self, x, y, size, anchor, col, msg):
        s = SS
        f = font(size * s)
        anchors = {"c": "mm", "l": "lm", "r": "rm"}
        self.d.text((x * s, y * s), msg, font=f, fill=col,
                    anchor=anchors.get(anchor, "mm"))

    def finish(self):
        out = self.img.resize((self.w, self.h), Image.LANCZOS)
        # Clip to the circle: the panel has no corners, and neither should the
        # PNG - it makes the screenshots read as a device, not a rectangle.
        mask = Image.new("L", (self.w * SS, self.h * SS), 0)
        ImageDraw.Draw(mask).ellipse([0, 0, self.w * SS - 1, self.h * SS - 1], fill=255)
        out.putalpha(mask.resize((self.w, self.h), Image.LANCZOS))
        return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)

    canvas = None
    name = None
    written = []

    for raw in sys.stdin:
        parts = raw.rstrip("\n").split(" ")
        if not parts or not parts[0]:
            continue
        op = parts[0]

        if op == "SCREEN":
            if canvas is not None:
                path = os.path.join(outdir, name + ".png")
                canvas.finish().save(path)
                written.append(path)
            name = parts[1]
            canvas = Canvas(int(parts[2]), int(parts[3]))
        elif canvas is None:
            continue
        elif op == "DISC":
            canvas.disc(int(parts[1]), int(parts[2]), int(parts[3]), parts[4])
        elif op == "RING":
            canvas.ring(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]), parts[5])
        elif op == "ARC":
            canvas.arc(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                       float(parts[5]), float(parts[6]), parts[7])
        elif op == "WEDGE":
            canvas.wedge(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                         float(parts[5]), float(parts[6]), parts[7])
        elif op == "LINE":
            canvas.line(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                        int(parts[5]), parts[6])
        elif op == "DOT":
            canvas.disc(int(parts[1]), int(parts[2]), int(parts[3]), parts[4])
        elif op == "ROUNDRECT":
            canvas.roundrect(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                             int(parts[5]), parts[6])
        elif op == "TEXT":
            canvas.text(int(parts[1]), int(parts[2]), int(parts[3]), parts[4],
                        parts[5], " ".join(parts[6:]))

    if canvas is not None:
        path = os.path.join(outdir, name + ".png")
        canvas.finish().save(path)
        written.append(path)

    for p in written:
        print("  wrote", p)
    print(f"  {len(written)} screens")


if __name__ == "__main__":
    main()
