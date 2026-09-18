#!/usr/bin/env python3
"""Grab the live Pharos screen over the console and write a PNG.

The device renders its real widget tree with lv_snapshot and streams it
run-length encoded and base64'd; this reverses that. See the note on
pharos_bsp_screen_dump() for why RLE is cheap here: the face is a circle on
true black, so most rows are one colour end to end.

  python3 tools/screen/grab.py out.png
"""
import base64, glob, re, sys, time
import serial

def port():
    c = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not c:
        sys.exit("no board on /dev/cu.usbmodem*")
    return c[0]

def grab(path, timeout=40.0):
    s = serial.Serial(port(), 115200, timeout=0.2)
    # DTR must be asserted or the board stays silent - see the console notes.
    s.setDTR(True); s.setRTS(False); time.sleep(0.3)
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

    text = buf.decode('utf-8', 'replace')
    m = re.search(r'PHAROSFB (\d+) (\d+) rgb565rle (\d+)', text)
    if not m:
        sys.exit("no frame header - is this build newer than the flash?")
    w, h, n = int(m.group(1)), int(m.group(2)), int(m.group(3))

    body = text[m.end():text.index("PHAROSFB END")]
    b64 = ''.join(l.strip() for l in body.splitlines() if l.strip())
    raw = base64.b64decode(b64 + '=' * (-len(b64) % 4))[:n]

    # RLE: count(LE16), pixel(LE16). Runs never cross a row.
    px = bytearray(w * h * 3)
    i = o = 0
    while i + 4 <= len(raw) and o < w * h:
        run = raw[i] | (raw[i + 1] << 8)
        v = raw[i + 2] | (raw[i + 3] << 8)
        i += 4
        # RGB565 -> RGB888, replicating the high bits so white stays white
        r = ((v >> 11) & 0x1F); g = ((v >> 5) & 0x3F); b = v & 0x1F
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2)
        for _ in range(min(run, w * h - o)):
            px[o*3], px[o*3+1], px[o*3+2] = r, g, b
            o += 1

    if o != w * h:
        print(f"warning: {o} of {w*h} pixels decoded", file=sys.stderr)

    try:
        from PIL import Image
        Image.frombytes('RGB', (w, h), bytes(px)).save(path)
    except ImportError:
        sys.exit("pip install pillow")
    print(f"{path}  {w}x{h}  {len(raw)} bytes on the wire "
          f"({100*len(raw)/(w*h*2):.1f}% of raw)")

if __name__ == '__main__':
    grab(sys.argv[1] if len(sys.argv) > 1 else 'screen.png')
