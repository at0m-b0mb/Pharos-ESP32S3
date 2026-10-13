#!/usr/bin/env python3
"""Photograph every screen the device can show, and contact-sheet the lot.

This is the hardware regression test for the face. The host suite proves the
geometry maths; tools/render proves the layout against the safe radius using
the real engines. Neither of them can tell you that the panel actually drew
what was asked, with real radio data, in the real font. Only the glass can,
and this is how the glass answers.

  python3 tools/screen/sweep.py                 # every lens, home view
  python3 tools/screen/sweep.py --detail        # home AND detail page each
  python3 tools/screen/sweep.py --lens wifi.watch wifi.census
  python3 tools/screen/sweep.py --settle 6      # give each lens longer to see

Writes PNGs plus sheet.png into the output directory, and exits non-zero if
any screen could not be captured - a lens that cannot draw itself is a
regression, not a warning.
"""
import argparse, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial
from grab import (FrameError, StaleFirmware, default_port, capture, decode,
                  to_rgb)


def lens_ids(dev, timeout=6.0):
    """Ask the board what it actually has, rather than trusting a list here
    that would rot the moment a lens is added."""
    s = serial.Serial(dev, 115200, timeout=0.2)
    s.setDTR(True)
    s.setRTS(False)
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"lens\r\n")
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = s.read(65536)
        if d:
            buf += d
    s.close()
    ids = []
    for line in buf.decode('utf-8', 'replace').splitlines():
        # One space is enough. Requiring two tied this parser to the width of
        # a printf column on the device, and the longest lens id overflowed it.
        m = re.match(r'\s*\*?\s+([a-z]+\.[a-z]+)\s+\S', line)
        if m and m.group(1) not in ids:
            ids.append(m.group(1))
    return ids


def shoot(dev, out, cmds, settle, timeout, tries):
    last = None
    for attempt in range(1, tries + 1):
        try:
            buf = capture(dev, list(cmds), settle, timeout)
            w, h, raw, skipped = decode(buf)
            rgb = to_rgb(w, h, raw)
        except StaleFirmware:
            raise
        except FrameError as e:
            last = e
            cmds = ()          # the setup already happened; only redo the dump
            time.sleep(0.5)
            continue
        from PIL import Image
        img = Image.frombytes('RGB', (w, h), rgb)
        img.save(out)
        return img, skipped
    raise FrameError(str(last))


SAFE_R = 224          # pharos_round.h: PR_SAFE_R, where the glass stops
CENTRE = 233


def escapes(img):
    """Count lit pixels outside the safe radius.

    tools/render bounds-checks the SIMULATED face against this radius. This is
    the same check against what the panel actually drew - which catches the
    cases the simulation cannot know about, like a real font being wider than
    the 0.6em estimate the layout was solved with."""
    px = img.load()
    w, h = img.size
    n, far = 0, 0
    r2max = SAFE_R * SAFE_R
    for y in range(h):
        dy = y - CENTRE
        dy2 = dy * dy
        if dy2 > r2max and abs(dy) - SAFE_R > w:
            continue
        for x in range(w):
            dx = x - CENTRE
            r2 = dx * dx + dy2
            if r2 <= r2max:
                continue
            r, g, b = px[x, y]
            if r + g + b > 60:
                n += 1
                if r2 > far:
                    far = r2
    return n, int(far ** 0.5)


def sheet(shots, path, cols=5, cell=200, pad=10):
    """A contact sheet you can take in at one glance - the point is to spot
    the one screen that looks wrong among twenty that look right."""
    from PIL import Image, ImageDraw
    if not shots:
        return
    rows = (len(shots) + cols - 1) // cols
    label_h = 18
    W = cols * (cell + pad) + pad
    H = rows * (cell + pad + label_h) + pad
    out = Image.new('RGB', (W, H), (16, 16, 18))
    d = ImageDraw.Draw(out)
    for i, (name, img) in enumerate(shots):
        x = pad + (i % cols) * (cell + pad)
        y = pad + (i // cols) * (cell + pad + label_h)
        out.paste(img.resize((cell, cell), Image.LANCZOS), (x, y))
        d.text((x + 2, y + cell + 3), name[:28], fill=(200, 200, 205))
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='assets/screens/sweep')
    ap.add_argument('--port')
    ap.add_argument('--lens', nargs='*', help="only these lens ids")
    ap.add_argument('--detail', action='store_true',
                    help="also capture each lens' advice page and detail rows")
    ap.add_argument('--settle', type=float, default=3.0)
    ap.add_argument('--timeout', type=float, default=40.0)
    ap.add_argument('--tries', type=int, default=3)
    a = ap.parse_args()

    dev = a.port or default_port()
    os.makedirs(a.out, exist_ok=True)

    ids = a.lens or lens_ids(dev)
    if not ids:
        sys.exit("the board listed no lenses - is it running Pharos?")
    print(f"sweeping {len(ids)} lenses on {dev}")

    shots, failed = [], []
    for lid in ids:
        views = [('', [f"lens {lid}"])]
        if a.detail:
            # The bottom strip has three stops: the face, then the full advice,
            # then the rows. One tap reaches the advice, two reach the detail.
            views.append(('.advice', [f"lens {lid}", "nav detail"]))
            views.append(('.detail',
                          [f"lens {lid}", "nav detail", "nav detail"]))
        for suffix, cmds in views:
            name = lid + suffix
            path = os.path.join(a.out, name.replace('.', '_') + '.png')
            try:
                img, skipped = shoot(dev, path, cmds, a.settle, a.timeout,
                                     a.tries)
                shots.append((name, img))
                esc, far = escapes(img)
                if esc:
                    failed.append((name, f"{esc} lit pixels past the "
                                         f"{SAFE_R}px safe radius (to r={far})"))
                    print(f"  ESC  {name}: {esc} px outside the glass "
                          f"(furthest r={far})")
                else:
                    note = (f"  ({skipped} log lines stepped over)"
                            if skipped else "")
                    print(f"  ok   {name}{note}")
            except (FrameError, StaleFirmware) as e:
                failed.append((name, str(e)))
                print(f"  FAIL {name}: {e}")

    sheet(shots, os.path.join(a.out, 'sheet.png'))
    print(f"\n{len(shots)} captured, {len(failed)} failed -> {a.out}/sheet.png")
    if failed:
        for n, e in failed:
            print(f"  {n}: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
