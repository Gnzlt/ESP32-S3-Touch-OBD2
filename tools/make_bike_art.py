"""
Draws the generic motorcycle the dashboard shows: a side view facing right,
white lines on black, 2400x1600. It's drawn from code (no artwork to license)
and stands for no particular make or model.

    python tools/make_bike_art.py

Writes tools/art/bike.png and prints each tyre's centre and outer radius,
which tools/make_tyre_bike.py needs. Then build the two images from it:

    python tools/make_tyre_bike.py tools/art/bike.png
    python tools/make_splash.py tools/art/bike.png

To show your own bike instead, give those two scripts your own drawing (see
docs/configure-your-bike.md, "Artwork"). Needs Pillow.
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
W, H = 2400, 1600
LINE = 9                       # stroke width, px
REAR = (520, 1150, 300)        # tyre centre x, y and outer radius
FRONT = (1880, 1150, 300)

img = Image.new("L", (W, H), 0)
d = ImageDraw.Draw(img)


def bez(*pts, n=48):
    """Points along a Bezier curve through the control points."""
    out = []
    for i in range(n + 1):
        t = i / n
        p = list(pts)
        while len(p) > 1:
            p = [(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t) for a, b in zip(p, p[1:])]
        out.append(p[0])
    return out


def line(pts, w=LINE):
    d.line(pts, fill=255, width=w, joint="curve")
    for x, y in (pts[0], pts[-1]):   # round caps
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)


def circle(cx, cy, r, w=LINE):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=255, width=w)


def arc(cx, cy, r, a0, a1, w=LINE):
    pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
           for a in range(int(a0), int(a1) + 1, 2)]
    line(pts, w)


def along(p, q, t):
    return (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)


def offset(p, q, o):
    """The segment p-q moved o px to its left."""
    dx, dy = q[0] - p[0], q[1] - p[1]
    n = math.hypot(dx, dy)
    nx, ny = -dy / n * o, dx / n * o
    return (p[0] + nx, p[1] + ny), (q[0] + nx, q[1] + ny)


def wheel(cx, cy, r, front):
    circle(cx, cy, r)              # tread
    circle(cx, cy, r - 62)         # tyre wall / rim edge
    circle(cx, cy, r - 84)         # rim
    circle(cx, cy, 34)             # hub
    for k in range(5):             # cast wheel: five split spokes
        a = math.radians(k * 72 - 90)
        for da in (-0.07, 0.07):
            p = (cx + 46 * math.cos(a + da * 3), cy + 46 * math.sin(a + da * 3))
            q = (cx + (r - 86) * math.cos(a + da), cy + (r - 86) * math.sin(a + da))
            line([p, q], LINE - 2)
    if front:
        circle(cx, cy, 150, LINE - 1)   # brake disc
        circle(cx, cy, 120, LINE - 3)
    else:
        circle(cx, cy, 112, LINE - 2)   # sprocket


# ---- Wheels and fenders ----------------------------------------------------------
wheel(*REAR, front=False)
wheel(*FRONT, front=True)
arc(FRONT[0], FRONT[1], FRONT[2] + 34, 205, 315)            # front fender
line(bez((FRONT[0] - 120, FRONT[1] - 320), (FRONT[0] - 40, FRONT[1] - 300), (FRONT[0] + 60, FRONT[1] - 310)))

# ---- Front end: fork, headlight, screen, bars -----------------------------------
rake = math.radians(25)
axle = (FRONT[0], FRONT[1])
top = (axle[0] - 690 * math.sin(rake), axle[1] - 690 * math.cos(rake))       # top yoke
mid = along(axle, top, 0.46)                                                 # lower/upper tube joint
for o in (-26, 26):
    a, b = offset(axle, mid, o)
    line([a, b])
for o in (-17, 17):
    a, b = offset(mid, top, o)
    line([a, b])
line([offset(axle, mid, -26)[1], offset(axle, mid, 26)[1]])                  # the slider's top edge
line([offset(mid, top, -40)[1], offset(mid, top, 40)[1]], LINE + 2)          # top yoke

head = (top[0] + 70, top[1] + 120)                                           # headlight
circle(head[0], head[1], 78)
circle(head[0], head[1], 54, LINE - 3)
line(bez((top[0] - 10, top[1] - 30), (top[0] + 60, top[1] - 120), (top[0] + 10, top[1] - 260),
         (top[0] - 50, top[1] - 300)))                                       # screen
line(bez((top[0] + 60, top[1] + 30), (top[0] + 120, top[1] - 60), (top[0] + 30, top[1] - 200)))
line([(top[0] - 20, top[1] - 20), (top[0] - 40, top[1] - 120)], LINE + 2)    # bar riser
line(bez((top[0] - 40, top[1] - 120), (top[0] - 120, top[1] - 130), (top[0] - 210, top[1] - 110)), LINE + 4)
line([(top[0] - 150, top[1] - 120), (top[0] - 190, top[1] - 250)])           # mirror stalk
d.ellipse((top[0] - 255, top[1] - 300, top[0] - 140, top[1] - 238), outline=255, width=LINE)

# ---- Tank, seat, tail ------------------------------------------------------------
tank_front = (top[0] - 60, top[1] + 70)
line(bez(tank_front, (tank_front[0] - 60, tank_front[1] - 120), (1380, 470), (1150, 600)))      # tank top
line(bez((tank_front[0] + 10, tank_front[1] + 150), (1450, 760), (1250, 720), (1150, 720)))     # tank bottom
line([tank_front, (tank_front[0] + 10, tank_front[1] + 150)])
line(bez((1330, 560), (1310, 620), (1270, 680), (1230, 715)), LINE - 3)                         # knee recess
line(bez((1150, 600), (1050, 650), (900, 650), (820, 630)))                                     # rider seat
line(bez((820, 630), (790, 590), (740, 580), (600, 590)))                                       # pillion
line(bez((600, 590), (540, 590), (500, 610), (470, 640)))                                       # tail tip
line(bez((470, 640), (600, 660), (850, 720), (1150, 720)))                                      # tail underside
line(bez((1150, 600), (1150, 650), (1150, 690), (1150, 720)), LINE - 2)
line([(540, 655), (460, 760)], LINE - 1)                                                        # plate hanger
d.rectangle((380, 760, 480, 815), outline=255, width=LINE - 2)
arc(REAR[0], REAR[1], REAR[2] + 30, 235, 300)                                                   # hugger

# ---- Frame, swingarm, shock ------------------------------------------------------
pivot = (1100, 1010)
line([(top[0] - 10, top[1] + 160), (1545, 805)], LINE + 2)                                       # down tube
line([pivot, (1150, 720)], LINE + 2)                                                            # frame to seat rail
for o in (-30, 30):                                                                             # swingarm
    a, b = offset(pivot, (REAR[0], REAR[1]), o)
    line([a, b])
line([offset(pivot, (REAR[0], REAR[1]), -30)[1], offset(pivot, (REAR[0], REAR[1]), 30)[1]])
shock_lo, shock_hi = (900, 1040), (1010, 740)                                                   # rear shock
for o in (-12, 12):
    a, b = offset(shock_lo, shock_hi, o)
    line([a, b], LINE - 3)
for k in range(1, 9):                                                                           # spring
    p = along(shock_lo, shock_hi, 0.15 + k * 0.08)
    a, b = offset(p, (p[0] + 1, p[1] - 3), 28)
    c, e = offset(p, (p[0] + 1, p[1] - 3), -28)
    line([a, c], LINE - 4)
circle(1010, 1040, 22)                                                                          # footpeg
line([(1010, 1040), (930, 1110)], LINE - 2)                                                     # side stand

# ---- Engine and exhaust ---------------------------------------------------------
d.rounded_rectangle((1150, 900, 1520, 1150), radius=70, outline=255, width=LINE)                # crankcase
cyl = [(1270, 790), (1560, 800), (1530, 960), (1250, 935)]                                      # cylinder
line(cyl + [cyl[0]])
for k in range(1, 4):                                                                           # fins
    a, b = along(cyl[0], cyl[3], k / 4), along(cyl[1], cyl[2], k / 4)
    line([a, b], LINE - 4)
circle(1330, 1030, 62, LINE - 2)                                                                # clutch cover
circle(1330, 1030, 30, LINE - 4)
line(bez((1540, 900), (1640, 960), (1640, 1180), (1450, 1200), (1180, 1200)))                   # header pipe
line(bez((1540, 960), (1600, 1010), (1590, 1150), (1450, 1165), (1180, 1165)))
can = [(1180, 1150), (800, 1040), (770, 1110), (1160, 1215)]                                    # silencer
line(can + [can[0]])
d.ellipse((740, 1028, 812, 1124), outline=255, width=LINE)

out = ROOT / "tools" / "art" / "bike.png"
out.parent.mkdir(exist_ok=True)
img.save(out, optimize=True)
print(f"wrote {out.relative_to(ROOT)}; rear tyre {REAR[0]},{REAR[1]},{REAR[2]}, front tyre {FRONT[0]},{FRONT[1]},{FRONT[2]}")
