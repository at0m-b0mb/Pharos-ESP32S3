#!/usr/bin/env python3
"""Rasterise a Pharos display list into round-panel PNGs.

The geometry and every number in the display list come from the firmware's own
C code (tools/render/pharos_render.c linking the real engines). This script
turns primitives into pixels and adds the finish an AMOLED panel actually has —
antialiasing (draw at 4x, downsample), a soft bloom on the bright elements, and
gradient-filled gauge arcs. No layout decision is taken here; all of that is in
the C, which is why it can be bounds-checked.

    ./pharos_render | python3 rasterize.py ../../assets/screens
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

SS = 4  # supersample factor — draw large, shrink for clean edges

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
        try:
            _font_cache[key] = ImageFont.truetype(_font_path, key) if _font_path \
                else ImageFont.load_default()
        except Exception:
            _font_cache[key] = ImageFont.load_default()
    return _font_cache[key]


def hex_rgb(c):
    c = c.lstrip("#")
    return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))


def lerp_rgb(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def to_pil_angles(start_deg, sweep):
    """Pharos measures degrees clockwise from 12 o'clock; PIL from 3 o'clock
    counter-clockwise. One conversion, in one place."""
    a0 = start_deg - 90.0
    return a0, a0 + sweep


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        # A second layer collects the elements that should bloom; it is blurred
        # and screened back over the base at finish time.
        self.glow = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        self.gd = ImageDraw.Draw(self.glow)

    def disc(self, cx, cy, r, col):
        s = SS
        self.d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=col)

    def ring(self, cx, cy, r, w, col):
        s = SS
        self.d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s],
                       outline=col, width=max(1, int(w * s)))

    def arc(self, cx, cy, r, w, start, sweep, col, glow=False):
        s = SS
        a0, a1 = to_pil_angles(start, sweep)
        box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
        self.d.arc(box, a0, a1, fill=col, width=max(1, int(w * s)))
        if glow:
            self.gd.arc(box, a0, a1, fill=col, width=max(1, int(w * s)))

    def garc(self, cx, cy, r, w, start, sweep, c0, c1, glow=True):
        """Gradient arc: interpolate colour along the sweep, drawn as short
        sub-arcs. This is what makes a gauge read as premium rather than flat."""
        if sweep <= 0.1:
            return
        s = SS
        box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
        a, b = hex_rgb(c0), hex_rgb(c1)
        steps = max(2, int(sweep / 2))
        for i in range(steps):
            t0 = i / steps
            t1 = (i + 1) / steps
            seg0 = start + sweep * t0
            seg1 = start + sweep * t1
            col = lerp_rgb(a, b, (t0 + t1) / 2)
            p0, p1 = to_pil_angles(seg0, seg1 - seg0)
            # small overlap so segments join seamlessly
            self.d.arc(box, p0 - 0.6, p1 + 0.6, fill=col, width=max(1, int(w * s)))
            if glow:
                self.gd.arc(box, p0 - 0.6, p1 + 0.6, fill=col, width=max(1, int(w * s)))

    def line(self, x1, y1, x2, y2, w, col, glow=False):
        s = SS
        pts = [x1 * s, y1 * s, x2 * s, y2 * s]
        self.d.line(pts, fill=col, width=max(1, int(w * s)))
        if glow:
            self.gd.line(pts, fill=col, width=max(1, int(w * s)))

    def dot(self, cx, cy, r, col, glow=False):
        s = SS
        box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
        self.d.ellipse(box, fill=col)
        if glow:
            self.gd.ellipse(box, fill=col)

    def roundrect(self, x, y, w, h, r, col):
        s = SS
        self.d.rounded_rectangle([x * s, y * s, (x + w) * s, (y + h) * s],
                                 radius=r * s, fill=col)

    def text(self, x, y, size, anchor, col, msg, glow=False):
        s = SS
        f = font(size * s)
        anchors = {"c": "mm", "l": "lm", "r": "rm"}
        self.d.text((x * s, y * s), msg, font=f, fill=col, anchor=anchors.get(anchor, "mm"))
        if glow:
            self.gd.text((x * s, y * s), msg, font=f, fill=col, anchor=anchors.get(anchor, "mm"))

    def finish(self):
        # Bloom: blur the glow layer and screen it over the base. Two radii,
        # a tight core and a wide halo, is what gives OLED text its lift.
        base = self.img
        if self.glow.getbbox():
            tight = self.glow.filter(ImageFilter.GaussianBlur(radius=3 * SS))
            wide = self.glow.filter(ImageFilter.GaussianBlur(radius=9 * SS))
            bloom = ImageChops.screen(tight, wide)
            base = ImageChops.screen(base, bloom)

        out = base.resize((self.w, self.h), Image.LANCZOS)
        # Clip to the circle: the panel has no corners, and neither should the
        # PNG — it reads as a device, not a rectangle.
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

    def flush():
        if canvas is not None:
            path = os.path.join(outdir, name + ".png")
            canvas.finish().save(path)
            written.append(path)

    for raw in sys.stdin:
        parts = raw.rstrip("\n").split(" ")
        if not parts or not parts[0]:
            continue
        op = parts[0]

        if op == "SCREEN":
            flush()
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
        elif op == "GARC":  # gradient arc: cx cy r w start sweep col0 col1
            canvas.garc(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                        float(parts[5]), float(parts[6]), parts[7], parts[8])
        elif op == "GLOWARC":  # arc that also blooms
            canvas.arc(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                       float(parts[5]), float(parts[6]), parts[7], glow=True)
        elif op == "WEDGE":
            # filled annulus sector — draw via pieslice then punch the middle
            cx, cy, r0, r1 = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            a0, a1 = to_pil_angles(float(parts[5]), float(parts[6]))
            s = SS
            canvas.d.pieslice([(cx - r1) * s, (cy - r1) * s, (cx + r1) * s, (cy + r1) * s],
                              a0, a1, fill=parts[7])
            if r0 > 0:
                canvas.d.pieslice([(cx - r0) * s, (cy - r0) * s, (cx + r0) * s, (cy + r0) * s],
                                  a0, a1, fill=(0, 0, 0, 0))
        elif op == "LINE":
            canvas.line(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                        int(parts[5]), parts[6])
        elif op == "GLOWLINE":
            canvas.line(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                        int(parts[5]), parts[6], glow=True)
        elif op == "DOT":
            canvas.dot(int(parts[1]), int(parts[2]), int(parts[3]), parts[4])
        elif op == "GLOWDOT":
            canvas.dot(int(parts[1]), int(parts[2]), int(parts[3]), parts[4], glow=True)
        elif op == "ROUNDRECT":
            canvas.roundrect(int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]),
                             int(parts[5]), parts[6])
        elif op == "TEXT":
            canvas.text(int(parts[1]), int(parts[2]), int(parts[3]), parts[4],
                        parts[5], " ".join(parts[6:]))
        elif op == "GLOWTEXT":
            canvas.text(int(parts[1]), int(parts[2]), int(parts[3]), parts[4],
                        parts[5], " ".join(parts[6:]), glow=True)

    flush()

    for p in written:
        print("  wrote", p)
    print(f"  {len(written)} screens")


if __name__ == "__main__":
    main()
