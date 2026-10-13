#!/usr/bin/env python3
"""Grab the live Pharos screen over the console and write a PNG.

The device renders its real widget tree with lv_snapshot and streams it
run-length encoded and base64'd; this reverses that. See the note on
pharos_bsp_screen_dump() for why RLE is cheap here: the face is a circle on
true black, so most rows are one colour end to end.

  python3 tools/screen/grab.py out.png
  python3 tools/screen/grab.py out.png --cmd "nav next" --settle 1.5
  python3 tools/screen/grab.py out.png --lens wifi.watch

THIS TOOL REFUSES TO GUESS. Every payload line is marked, the stream is
checksummed, and the pixel count must come out exact. If any of those fail it
retries, and if it still fails you get an error and no file - because the
version that quietly painted whatever arrived cost an afternoon hunting a
rendering fault that was never on the glass.
"""
import argparse, base64, glob, re, sys, time
import serial

MARK = b'FB:'
NAV_WORDS = ("detail", "next", "prev", "select", "home")


class FrameError(Exception):
    """The frame arrived damaged. Worth another try."""


class StaleFirmware(Exception):
    """The board predates the checksummed format. Retrying will not help."""


def default_port():
    c = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not c:
        sys.exit("no board on /dev/cu.usbmodem*")
    return c[-1]


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def capture(dev, cmds, settle, timeout):
    s = serial.Serial(dev, 115200, timeout=0.2)
    # DTR must be asserted or the board stays silent - see the console notes.
    s.setDTR(True)
    s.setRTS(False)
    time.sleep(0.3)
    s.reset_input_buffer()
    for c in cmds:
        s.write(c.encode() + b"\r\n")
        time.sleep(settle)
    if cmds:
        s.reset_input_buffer()
    s.write(b"screen dump\r\n")

    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = s.read(65536)
        if d:
            buf += d
            if b"PHAROSFB END" in buf:
                break
    s.close()
    return buf


def decode(buf):
    m = re.search(rb'PHAROSFB2 (\d+) (\d+) rgb565rle (\d+) ([0-9a-f]{8})', buf)
    if not m:
        if b'PHAROSFB ' in buf:
            raise StaleFirmware(
                "this board is running the old unmarked dump format, whose "
                "captures cannot be trusted - reflash before believing a "
                "screenshot (idf.py flash)")
        raise FrameError("no frame header - is the board running Pharos, and "
                         "is the display up? (try 'diag')")
    w, h, n, want = (int(m.group(1)), int(m.group(2)), int(m.group(3)),
                     int(m.group(4), 16))

    end = buf.index(b"PHAROSFB END")
    # ONLY MARKED LINES. The logger shares this console and does not wait;
    # an unmarked line is its business, not ours.
    body, skipped = [], 0
    for line in buf[m.end():end].split(b'\n'):
        line = line.strip()
        if not line:
            continue
        if line.startswith(MARK):
            body.append(line[len(MARK):])
        else:
            skipped += 1
    b64 = b''.join(body)
    raw = base64.b64decode(b64 + b'=' * (-len(b64) % 4))[:n]

    if len(raw) != n:
        raise FrameError(f"stream truncated: {len(raw)} of {n} bytes arrived")
    got = fnv1a(raw)
    if got != want:
        raise FrameError(f"checksum mismatch: {got:08x} != {want:08x} - the "
                         f"frame was mangled on the wire, not rendered wrong")
    return w, h, raw, skipped


def to_rgb(w, h, raw):
    """RLE: count(LE16), pixel(LE16). Runs never cross a row."""
    px = bytearray()
    npx = 0
    i = 0
    while i + 4 <= len(raw):
        run = raw[i] | (raw[i + 1] << 8)
        v = raw[i + 2] | (raw[i + 3] << 8)
        i += 4
        # RGB565 -> RGB888, replicating the high bits so white stays white
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4),
                     (b << 3) | (b >> 2))) * run
        npx += run
    if npx != w * h:
        raise FrameError(f"frame is {npx} pixels, the panel is {w*h} - "
                         f"refusing to pad or crop a screenshot")
    return bytes(px)


def grab(out, cmds=(), settle=1.0, timeout=40.0, port=None, tries=3,
         quiet=False):
    """Capture one frame and save it. Returns (w, h, wire_bytes, skipped)."""
    dev = port or default_port()
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
            if not quiet:
                print(f"  attempt {attempt}/{tries}: {e}", file=sys.stderr)
            # Only the dump itself is worth repeating; the setup commands have
            # already been applied and replaying them would move the UI on.
            cmds = ()
            time.sleep(0.5)
            continue

        try:
            from PIL import Image
        except ImportError:
            sys.exit("pip install pillow")
        Image.frombytes('RGB', (w, h), rgb).save(out)
        return w, h, len(raw), skipped

    raise FrameError(f"gave up after {tries} attempts: {last}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', default='screen.png')
    ap.add_argument('--port')
    ap.add_argument('--cmd', action='append', default=[],
                    help="console command to run before capturing, repeatable")
    ap.add_argument('--lens', help="switch to this lens id first")
    ap.add_argument('--settle', type=float, default=1.0,
                    help="seconds to wait after each command (default 1.0)")
    ap.add_argument('--timeout', type=float, default=40.0)
    ap.add_argument('--tries', type=int, default=3)
    a = ap.parse_args()

    cmds = ([f"lens {a.lens}"] if a.lens else [])
    cmds += [f"nav {c}" if c in NAV_WORDS else c for c in a.cmd]

    try:
        w, h, n, skipped = grab(a.out, cmds, a.settle, a.timeout, a.port,
                                a.tries)
    except (FrameError, StaleFirmware) as e:
        sys.exit(str(e))

    note = f", {skipped} log line(s) stepped over" if skipped else ""
    print(f"{a.out}  {w}x{h}  {n} bytes on the wire "
          f"({100*n/(w*h*2):.1f}% of raw){note}")


if __name__ == '__main__':
    main()
