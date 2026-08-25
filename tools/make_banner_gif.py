#!/usr/bin/env python3
"""Pharos - the animated README banner.

Built by rendering the REAL banner.svg once per frame with a beam injected into
it, rather than by redrawing the artwork in code. The lighthouse is the brand,
it already exists as vector, and a hand-copied version of it in PIL would drift
from the original the first time either changed.

The motion is not decoration. Pharos is the Lighthouse of Alexandria, and the
device it names really does sweep: one radio, sixteen watches, each taking the
antenna in turn (see pharos_tower.h). Seen side-on, a rotating lighthouse beam
sweeps across the view and flares as it comes round to face you - which is what
this does, and it is the same rotation the firmware performs.
"""
import math
import os
import re
import subprocess
import tempfile
from PIL import Image

SRC = "assets/branding/banner.svg"
OUT = "assets/branding/banner.gif"
FRAMES = 30
MS = 70

# The lighthouse group is translate(721.38,61.38) scale(4.3103) over a 120-unit
# square, so the lamp at local (60, 42.5) lands here. The beam has to leave the
# LAMP - a beam from the middle of the circle would pass through the tower.
SCALE = 4.3103
OX, OY = 721.38, 61.38
LAMP = (OX + 60.0 * SCALE, OY + 42.5 * SCALE)
DIAL = (OX + 60.0 * SCALE, OY + 60.0 * SCALE)
DIAL_R = 58.0 * SCALE

ACCENT = (0x21, 0xB6, 0xC6)
TRACK  = (0x18, 0x38, 0x4A)
GREEN  = (0x39, 0xDB, 0x84)
AMBER  = (0xFF, 0xC3, 0x4A)
TEXT   = (0xE7, 0xF7, 0xF7)
DIMMER = (0x63, 0x92, 0xA5)


def beam_svg(phase):
    """One frame's worth of beam, in the lighthouse group's LOCAL coordinates.

    It has to be local, and it has to go INSIDE the group: the group's first
    element is the filled field circle, so a beam injected before it was
    painted straight over and never appeared at all. Inside, after the field
    and before the tower, the light sits behind the glass and the tower
    occludes it - which is what makes it read as coming from the lamp rather
    than being painted on the front.

    `phase` is 0..1 around a full turn. A lighthouse seen from the side shows
    its beam sweeping across and flaring as it comes round to face you; half a
    turn later it is pointing away and the sky is nearly dark. That asymmetry
    is what makes it read as rotating rather than as a windscreen wiper.
    """
    ang = phase * 2.0 * math.pi
    facing = math.cos(ang)                 # +1 straight at us, -1 away
    lx, ly = 60.0, 42.5                    # the lamp, in group units
    r = 58.0                               # the field circle's radius

    # Pointing at the viewer the beam is short and broad; edge-on it is long
    # and narrow. That is foreshortening, and it is most of the illusion.
    spread = 6.0 + 17.0 * max(0.0, facing)
    reach = r * (1.05 - 0.18 * max(0.0, facing))
    tilt = math.degrees(math.asin(max(-1.0, min(1.0, math.sin(ang))))) * 1.2

    # A floor, so the beam never vanishes completely - a lighthouse that goes
    # fully dark for half of a two-second loop looks broken rather than
    # rotating.
    lit = 0.20 + 0.80 * max(0.0, facing) ** 1.5

    parts = [f'<clipPath id="beamclip"><circle cx="60" cy="60" r="{r}"/>'
             '</clipPath>', '<g clip-path="url(#beamclip)">']
    for w, o in ((1.0, 0.42), (0.60, 0.62), (0.28, 0.90)):
        half = spread * w
        a0, a1 = math.radians(tilt - half), math.radians(tilt + half)
        p0 = (lx + math.sin(a0) * reach, ly - math.cos(a0) * reach)
        p1 = (lx + math.sin(a1) * reach, ly - math.cos(a1) * reach)
        parts.append(
            f'<path d="M{lx:.2f} {ly:.2f} L{p0[0]:.2f} {p0[1]:.2f} '
            f'L{p1[0]:.2f} {p1[1]:.2f} Z" fill="#1fb6c9" '
            f'opacity="{lit * o:.3f}"/>')

    # The lamp's own halo brightens as it comes round.
    parts.append(f'<circle cx="{lx:.2f}" cy="{ly:.2f}" r="{5 + 5 * lit:.2f}" '
                 f'fill="#ffc24b" opacity="{0.12 + 0.38 * lit:.3f}"/>')
    parts.append("</g>")
    return "".join(parts)


def ring_svg(phase):
    """The sixteen watches, around the rim, lighting as the beam reaches them.

    This is the firmware's rotation drawn literally: one radio, sixteen
    watches, each taking its turn. The dot that is lit is the watch that
    currently holds the antenna.
    """
    names = ["WATCH", "KARMA", "MIRAGE", "HARVEST", "TWIN", "WARD", "RIVAL",
             "ROSTER", "CENSUS", "PROBE", "SQUALL", "VIGIL", "WHISPER",
             "SENTINEL", "SPECTRUM", "FOOTPRINT"]
    n = len(names)
    cx, cy = DIAL
    # Inside the rim, not outside it. Placed beyond the circle the dots
    # scattered across the corners of the banner and the topmost label was cut
    # off by the canvas edge - a ring of watches around the lighthouse is the
    # picture, not a halo drifting off the page.
    rr = DIAL_R - 16.0
    sweep = phase * 360.0 - 90.0
    out = []
    lead = min(range(n),
               key=lambda i: abs((((-90.0 + 360.0 * i / n) - sweep + 180.0)
                                  % 360.0) - 180.0))
    for i, nm in enumerate(names):
        a = -90.0 + 360.0 * i / n
        rad = math.radians(a)
        x, y = cx + math.cos(rad) * rr, cy + math.sin(rad) * rr
        gap = abs(((a - sweep + 180.0) % 360.0) - 180.0)
        lit = max(0.0, 1.0 - gap / 34.0)
        r = 3.0 + 4.0 * lit
        col = "#1fb6c9" if lit > 0.12 else "#1d4d63"
        out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" '
                   f'fill="{col}" opacity="{0.45 + 0.55 * lit:.2f}"/>')
        if i == lead and lit > 0.3:
            # Only the watch holding the antenna is named. Naming every dot
            # the beam brushes crowds them, and is untrue besides.
            # Inboard of its dot, towards the middle - the only direction
            # with room, and it clears the tower at every angle.
            lx = cx + math.cos(rad) * (rr - 34.0)
            ly = cy + math.sin(rad) * (rr - 34.0) + 4.0
            out.append(
                f'<text x="{lx:.1f}" y="{ly:.1f}" text-anchor="middle" '
                f'font-family="Menlo, monospace" font-size="13" fill="#e7f7f7" '
                f'opacity="{lit:.2f}">{nm}</text>')
    return "".join(out)


def brand_palette():
    """A fixed palette with the verdict colours reserved.

    An adaptive palette is chosen from what covers the most pixels, and this
    frame is overwhelmingly dark navy - so the three coloured bullets, thirty
    pixels each, earned no slot and quantised to identical grey. The claims
    lost the colour that distinguishes them. One shared palette across every
    frame also makes the file substantially smaller.
    """
    pal = []

    def ramp(a, b, n):
        for i in range(n):
            t = i / max(1, n - 1)
            pal.append(tuple(int(a[j] + (b[j] - a[j]) * t) for j in range(3)))

    ramp((0x10, 0x30, 0x46), (0x04, 0x09, 0x0F), 30)   # background gradient
    ramp((0x08, 0x15, 0x21), (0x12, 0x34, 0x49), 12)   # the dial's field
    ramp(TRACK, ACCENT, 22)                            # dots, rim, beam
    ramp(ACCENT, (0xDF, 0xFA, 0xFF), 8)
    ramp(DIMMER, TEXT, 18)                             # type
    ramp((0x9F, 0xC2, 0xD0), (0xEE, 0xF7, 0xFB), 12)   # the tower
    # THE TOWER'S RED BANDS, which quantised to amber.
    #
    # The upper band is drawn at 55% opacity over a pale tower, so on the glass
    # it is a light dusty red - a colour that sat between this palette's red
    # ramp and its lamp-amber ramp and got rounded to the wrong one. A
    # lighthouse with one red band and one orange band looks like a rendering
    # fault. The ramp now runs all the way up to that blended tint.
    ramp((0x6A, 0x1E, 0x18), (0xC6, 0x39, 0x2E), 10)
    ramp((0xC6, 0x39, 0x2E), (0xE0, 0x9A, 0x92), 10)
    ramp((0x7F, 0x60, 0x20), AMBER, 12)                # the lamp
    ramp((0xFF, 0xC2, 0x4B), (0xFF, 0xF0, 0xC6), 8)
    ramp((0x1A, 0x60, 0x3C), GREEN, 8)
    flat = []
    for c in pal[:256]:
        flat.extend(c)
    flat.extend([0, 0, 0] * (256 - len(pal[:256])))
    p = Image.new("P", (1, 1))
    p.putpalette(flat)
    return p


def build():
    svg = open(SRC, encoding="utf-8").read()

    # The beam goes UNDER the lighthouse group so the tower occludes it, which
    # is what makes the light look like it is behind the glass rather than
    # painted on the front.
    # After the field circle, before the tower - see beam_svg().
    anchor = ('<circle cx="60" cy="60" r="58" fill="url(#field)" '
              'stroke="#14384c" stroke-width="1.5"/>')
    if anchor not in svg:
        raise SystemExit("banner.svg has moved; the field circle is gone")

    # Keep the wordmark honest about the count the firmware actually registers.
    svg = svg.replace("thirteen watches", "sixteen watches")

    pal = brand_palette()
    frames = []
    with tempfile.TemporaryDirectory() as td:
        for i in range(FRAMES):
            phase = i / FRAMES
            frame_svg = svg.replace(anchor, anchor + beam_svg(phase))
            frame_svg = frame_svg.replace("</svg>", ring_svg(phase) + "</svg>")
            sp = os.path.join(td, f"f{i}.svg")
            pp = os.path.join(td, f"f{i}.png")
            open(sp, "w", encoding="utf-8").write(frame_svg)
            subprocess.run(["rsvg-convert", "-w", "1280", "-h", "640",
                            "-o", pp, sp], check=True)
            im = Image.open(pp).convert("RGB")
            frames.append(im.quantize(palette=pal, dither=Image.Dither.NONE))

    # DISPOSAL 1, NOT 2.
    #
    # Disposal 2 restores the background before each frame, which forces every
    # frame to be stored in full - and five sixths of this banner never changes.
    # Leaving the previous frame in place lets the encoder store only the dial,
    # which is the only part that moves. Same animation, a third of the bytes.
    frames[0].save(OUT, save_all=True, append_images=frames[1:], loop=0,
                   duration=MS, optimize=True, disposal=1)
    return OUT


if __name__ == "__main__":
    print(build())
