"""Grab what the board shows as a PNG: the screen plus the overlay layer (face,
pet props, history screens), composited. Optional commands run first.
usage: screenshot.py PORT OUT.png [--no-top] [--wait SECONDS] [cmd ...]
Needs firmware with `screenshot top` (clawdmeter2)."""
import struct, sys, time, zlib
import serial

def read_frame(s, cmd, tag):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    end = time.time() + 20
    line = b""
    while time.time() < end:
        line = s.readline()
        if line.startswith(tag.encode()):
            break
    else:
        raise SystemExit(f"no {tag} (last line: {line!r})")
    w, h, n = map(int, line.split()[1:4])
    data = b""
    while len(data) < n and time.time() < end + 20:
        data += s.read(n - len(data))
    if len(data) != n:
        raise SystemExit(f"short read {len(data)}/{n}")
    return w, h, data

def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def main():
    port, out = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    wait = 1.0
    no_top = args[:1] == ["--no-top"]   # the screen only (the overlay layer read is flaky under animation)
    if no_top:
        args = args[1:]
    if args[:1] == ["--wait"]:
        wait, args = float(args[1]), args[2:]
    s = serial.Serial(None, 115200, timeout=0.5)
    s.port = port; s.dtr = False; s.rts = False  # never pulse the USB reset lines
    s.open()
    for c in args:
        s.write((c + "\n").encode())
        time.sleep(0.3)
    time.sleep(wait)
    w, h, scr = read_frame(s, "screenshot", "SCREENSHOT_START")
    top = bytes(w * h * 4) if no_top else read_frame(s, "screenshot top", "SCREENSHOT_TOP_START")[2]
    s.close()
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        p = scr[2 * i] | (scr[2 * i + 1] << 8)   # RGB565 little-endian
        r, g, b = (p >> 11) << 3, ((p >> 5) & 0x3F) << 2, (p & 0x1F) << 3
        tb, tg, tr, ta = top[4 * i], top[4 * i + 1], top[4 * i + 2], top[4 * i + 3]   # ARGB8888 = B,G,R,A bytes
        rgb[3 * i] = (tr * ta + r * (255 - ta)) // 255
        rgb[3 * i + 1] = (tg * ta + g * (255 - ta)) // 255
        rgb[3 * i + 2] = (tb * ta + b * (255 - ta)) // 255
    png(out, w, h, bytes(rgb))
    print(out)

main()

