"""
Captures the page screenshots a DASH_SCREENSHOTS build prints at boot and
saves them as PNGs, transparent outside the round panel. Needs pyserial and Pillow.
The firmware keeps cycling through the pages, so a page whose data got cut short
is caught on the next lap.

    python tools/grab_screens.py /dev/cu.usbmodem* out_dir
"""
import base64
import math
import re
import sys
import time
from pathlib import Path

import serial
from PIL import Image

port, out = sys.argv[1], Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False   # reset into the app
saved, cur, lines = set(), None, []
PAGES = 10   # N_PAGES + N_VIEWS in main/ui/ui.c (8 pages, then Rides and Diagnostics)
deadline = time.time() + 300   # pushed back while shots arrive (DASH_SCREENSHOT_DELAY_S)
while time.time() < deadline and len(saved) < PAGES:
    line = s.readline().decode("ascii", "replace").strip()
    if line.startswith("SHOT "):
        cur = [int(x) for x in line.split()[1:]]
        lines = []
        deadline = time.time() + 120
    elif line == "ENDSHOT" and cur:
        page, w, h, stride = cur
        raw = base64.b64decode("".join(lines))
        if len(raw) < stride * h:
            print(f"page {page}: short ({len(raw)} of {stride * h} bytes), skipped")
            cur = None
            continue
        if page in saved:
            cur = None
            continue
        img = Image.new("RGBA", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                v = raw[y * stride + 2 * x] | raw[y * stride + 2 * x + 1] << 8
                # Coverage of the round glass, anti-aliased over one pixel.
                edge = w / 2 - math.hypot(x + 0.5 - w / 2, y + 0.5 - h / 2)
                a = int(255 * min(1.0, max(0.0, edge + 0.5)))
                px[x, y] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3, a)
        img.save(out / f"page{page}.png")
        print(f"page {page} saved")
        saved.add(page)
        cur = None
    elif cur and re.fullmatch(r"[A-Za-z0-9+/=]{4,76}", line) and len(line) % 4 == 0:
        lines.append(line)
