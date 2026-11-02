"""
Turns the ride logs pulled from the board (logs/) into a static website
(site/): an overview of every ride and fill-up, and one page per ride with
its charts. Plain HTML, CSS and JS with the data embedded in each page, so
it opens straight from disk (file://), no server and no libraries.

    python3 tools/build_site.py [logs_dir] [site_dir]

Reads each ride folder's ride.json, trip.csv and events.csv. logs/notes.json,
optional and hand-written, adds a note to a ride, or a date to one logged
before the board's clock was set:
    {"ride_0001": {"date": "2026-05-01", "note": "first ride"}}

The page code lives in tools/site/ and is copied to site/assets/. Python
standard library only. pull_logs.py runs this after each download.
"""
import csv
import html
import json
import math
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = Path(__file__).resolve().parent / "site"

# trip.csv columns the charts know: label, unit, decimals in the page data.
COLUMNS = {
    "kmh": ("Speed", "km/h", 0),
    "rpm": ("RPM", "rpm", 0),
    "gear": ("Gear", "", 0),
    "throttle_pct": ("Throttle", "%", 1),
    "load_pct": ("Load", "%", 1),
    "map_kpa": ("MAP", "kPa", 0),
    "coolant_c": ("Coolant", "°C", 0),
    "iat_c": ("Intake air", "°C", 0),
    "advance_deg": ("Ignition advance", "°", 1),
    "stft_pct": ("Short-term fuel trim", "%", 1),
    "fuel_sys": ("Fuel loop", "", 0),
    "fuel_lph_raw": ("Fuel rate", "L/h", 2),
    "fuel_ml_raw": ("Fuel used", "mL", 0),
    "volts": ("Battery", "V", 2),
    "lean_deg": ("Lean", "°", 1),
    "lat_g": ("Lateral G", "g", 2),
    "lon_g": ("Longitudinal G", "g", 2),
    "trip_km": ("Distance", "km", 2),
    "board_soc": ("Board battery", "%", 0),
    "board_mv": ("Board battery", "mV", 0),
    "front_kpa": ("Front tyre", "kPa", 0),
    "front_c": ("Front tyre", "°C", 0),
    "rear_kpa": ("Rear tyre", "kPa", 0),
    "rear_c": ("Rear tyre", "°C", 0),
    "chip_c": ("ESP32-S3 chip", "°C", 1),
    "cell_c": ("Battery area", "°C", 1),
    "board_ma": ("Board current", "mA", 0),
}
# Within a 1-second bucket these keep the sample furthest from zero, so a
# peak survives; gear and fuel_sys keep the last; the rest are averaged.
PEAK = {"lean_deg", "lat_g", "lon_g"}
LAST = {"gear", "fuel_sys", "trip_km", "fuel_ml_raw"}
FUEL_SYS = {1: "Open loop, cold", 2: "Closed loop", 4: "Open loop, load or decel", 8: "Open loop, fault",
            16: "Open loop, fault"}
HELD_S = 1.0          # max lean and G: the largest value held this long
RUNNING_RPM = 800
VOLTS_AFTER_S = 10    # min volts: not while the starter turns
WAKE_KPA = 20         # a tyre sensor's first packets after waking can read ~1 kPa


def num(text):
    try:
        v = float(text)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def read_csv(path):
    if not path.exists():
        return [], []
    with open(path, newline="") as f:
        lines = [line for line in f if line.strip() and not line.startswith("#")]
    reader = csv.DictReader(lines)
    return list(reader), reader.fieldnames or []


def load_ride(folder, notes):
    meta = None
    for name in ("ride.json", "ride.json.tmp"):
        p = folder / name
        if p.exists():
            try:
                meta = json.loads(p.read_text())
                break
            except ValueError:
                pass
    if meta is None:
        meta = {"format": 0, "in_progress": True}
    meta["ride"] = int(folder.name.split("_")[1])
    note = notes.get(folder.name, {})
    meta["note"] = note.get("note", "")
    meta["date"] = note.get("date", "")   # a local date, for rides before the clock

    rows, fields = read_csv(folder / "trip.csv")
    events = []
    for e in read_csv(folder / "events.csv")[0]:
        t = num(e.get("t_s"))
        if t is not None:
            events.append({"t": t, "type": e.get("type", ""), "detail": e.get("detail", "")})
    samples = []
    for r in rows:
        t = num(r.get("t_s"))
        if t is None:
            continue
        samples.append({k: num(r.get(k)) for k in COLUMNS if k in fields} | {"t": t})
    events.sort(key=lambda e: e["t"])
    return {"meta": meta, "samples": samples, "events": events, "columns": [k for k in COLUMNS if k in fields],
            "files": sorted(p.name for p in folder.iterdir() if p.is_file())}


def held_peaks(samples, value):
    """The largest |value| held for HELD_S, each side: (positive, negative) as
    positive numbers, None without data. A window counts when it has rows all
    through (no gap over 0.5 s) and one sign; its smallest |value| was held.
    A jolt or the board shifting in a bag doesn't last; a corner does."""
    pts = [(s["t"], value(s)) for s in samples]
    pts = [(t, v) for t, v in pts if v is not None]
    best = {1: None, -1: None}
    j = 0
    for i, (t0, v0) in enumerate(pts):
        if j < i:
            j = i
        # the rows i..j starting within HELD_S of t0 (five 5 Hz rows: each stands for 0.2 s)
        while j + 1 < len(pts) and pts[j + 1][0] - t0 < HELD_S - 1e-6 and pts[j + 1][0] - pts[j][0] <= 0.5:
            j += 1
        if pts[j][0] - t0 < HELD_S - 0.25:   # 5 Hz rows: 0.8 s between the first and last of a second
            continue
        window = [v for _, v in pts[i:j + 1]]
        sign = 1 if window[0] > 0 else -1 if window[0] < 0 else 0
        if not sign or any((v > 0) - (v < 0) != sign for v in window):
            continue
        held = min(abs(v) for v in window)
        if best[sign] is None or held > best[sign]:
            best[sign] = held
    return best[1], best[-1]


def min_volts_running(samples):
    """The lowest battery voltage once the engine has run VOLTS_AFTER_S."""
    low, since = None, None
    for s in samples:
        rpm, v = s.get("rpm"), s.get("volts")
        if rpm is None or rpm < RUNNING_RPM:
            since = None
            continue
        if since is None:
            since = s["t"]
        if v is not None and s["t"] - since >= VOLTS_AFTER_S and (low is None or v < low):
            low = v
    return low


def derived(samples):
    """Tiles worked out from the rows (held peaks, voltage once running)."""
    d = {}
    # Riding only, as the board measures it.
    samples_riding = [s for s in samples if (s.get("kmh") or 0) >= 15]
    right, left = held_peaks(samples_riding, lambda s: s.get("lean_deg"))
    if right is not None or left is not None:
        d["max_lean_right_deg"], d["max_lean_left_deg"] = round(right or 0, 1), round(left or 0, 1)
    g, _ = held_peaks(samples_riding, lambda s: math.hypot(s["lat_g"], s["lon_g"])
                      if s.get("lat_g") is not None and s.get("lon_g") is not None else None)
    if g is not None:
        d["max_g"] = round(g, 2)
    if (v := min_volts_running(samples)) is not None:
        d["min_volts"] = round(v, 2)
    # Each tyre's range from the rows, after drop_wake_readings().
    for tyre in ("front", "rear"):
        kpa = [s[f"{tyre}_kpa"] for s in samples if s.get(f"{tyre}_kpa") is not None]
        if kpa:
            d.setdefault("tyres", {})[tyre] = {"min_kpa": min(kpa), "max_kpa": max(kpa)}
    return d


def drop_wake_readings(samples):
    """Without tyre events, trip.csv may hold a sensor's wake-up packets (1-2
    kPa for a few seconds). Pressures under WAKE_KPA are dropped until that
    tyre's first real reading."""
    for key in ("front_kpa", "rear_kpa"):
        for s in samples:
            v = s.get(key)
            if v is None:
                continue
            if v >= WAKE_KPA:
                break
            s[key] = None


def tyre_readings(events):
    """tyre events ("front 194 21 -86": kPa, °C, dBm) per tyre, as [t, kpa, c, dbm]."""
    out = {}
    for e in events:
        if e["type"] != "tyre":
            continue
        parts = e["detail"].split(" ")
        if len(parts) < 2 or num(parts[1]) is None:
            continue
        vals = [num(p) if p else None for p in parts[1:4]] + [None] * 3
        out.setdefault(parts[0], []).append([e["t"], vals[0], vals[1], vals[2]])
    return out


def tyre_series(readings, ts, hold_s=900):
    """Each reading held on the one-second grid until the next, or hold_s."""
    series = {}
    for tyre, rows in readings.items():
        for i, name in ((1, f"{tyre}_kpa"), (2, f"{tyre}_c")):
            vals, k, cur, cur_t = [], 0, None, None
            for t in ts:
                while k < len(rows) and rows[k][0] <= t + 0.5:
                    if rows[k][i] is not None:
                        cur, cur_t = rows[k][i], rows[k][0]
                    k += 1
                vals.append(cur if cur is not None and t - cur_t <= hold_s else None)
            if any(v is not None for v in vals):
                series[name] = vals
    return series


def bucket_series(samples, columns):
    """One point a second: t, then each column's values (None in gaps)."""
    buckets = {}
    for s in samples:
        buckets.setdefault(int(s["t"]), []).append(s)
    ts = sorted(buckets)
    out = {"t": ts}
    for c in columns:
        vals = []
        for t in ts:
            xs = [s[c] for s in buckets[t] if s.get(c) is not None]
            if not xs:
                v = None
            elif c in PEAK:
                v = max(xs, key=abs)
            elif c in LAST:
                v = xs[-1]
            else:
                v = sum(xs) / len(xs)
            vals.append(None if v is None else round(v, COLUMNS[c][2]))
        if any(v is not None for v in vals):
            out[c] = vals
    return out


def histogram(values, width, lo=None, hi=None):
    values = [v for v in values if v is not None]
    if not values:
        return None
    lo = math.floor(min(values) / width) * width if lo is None else lo
    hi = math.floor(max(values) / width) * width if hi is None else hi
    n = int(round((hi - lo) / width)) + 1
    counts = [0] * n
    for v in values:
        i = int(math.floor((v - lo) / width))
        if 0 <= i < n:
            counts[i] += 1
    return {"lo": lo, "width": width, "seconds": [round(c * 0.2, 1) for c in counts]}


def distributions(samples):
    """From the full 5 Hz rows: each sample is 0.2 s of riding."""
    col = lambda k: [s.get(k) for s in samples]
    moving = [s for s in samples if (s.get("kmh") or 0) > 3]
    d = {}
    gears = {}
    for s in moving:
        g = s.get("gear")
        if g:
            gears[int(g)] = gears.get(int(g), 0) + 0.2
    if gears:
        d["gear"] = {str(g): round(gears[g], 1) for g in sorted(gears)}
    running = [v for v in col("rpm") if v and v >= 300]
    if h := histogram(running, 500, 0):
        d["rpm"] = h
    if h := histogram([s.get("kmh") for s in moving], 10, 0):
        d["kmh"] = h
    if h := histogram([s.get("lean_deg") for s in moving], 5, -60, 55):
        d["lean"] = h
    loops = {}
    for v in col("fuel_sys"):
        if v:
            name = FUEL_SYS.get(int(v), f"Other ({int(v)})")
            loops[name] = loops.get(name, 0) + 0.2
    if loops:
        d["fuel_sys"] = {k: round(v, 1) for k, v in sorted(loops.items(), key=lambda kv: -kv[1])}
    gg = [(s["lat_g"], s["lon_g"]) for s in samples if s.get("lat_g") is not None and s.get("lon_g") is not None]
    if gg:
        step = max(1, len(gg) // 3000)
        d["gg"] = [[round(x, 2), round(y, 2)] for x, y in gg[::step]]
    return d


def summary_row(ride):
    m, d = ride["meta"], ride.get("derived", {})
    k = m.get("fuel_k") or 1
    fuel_l = m["fuel_raw_l"] * k if m.get("fuel_raw_l") is not None else None
    l100 = m["fuel_raw_l100"] * k if m.get("fuel_raw_l100") is not None else None
    return {
        "ride": m["ride"], "start": m.get("start"), "date": m.get("date", ""), "note": m.get("note", ""),
        "in_progress": bool(m.get("in_progress")), "format": m.get("format", 0),
        "duration_s": m.get("duration_s"), "engine_s": m.get("engine_s"), "distance_km": m.get("distance_km"),
        "avg_kmh": m.get("avg_kmh"), "max_kmh": m.get("max_kmh"), "max_rpm": m.get("max_rpm"),
        "max_coolant_c": m.get("max_coolant_c"), "min_volts": d.get("min_volts", m.get("min_volts")),
        "max_lean_deg": max((v for v in (d.get("max_lean_left_deg", m.get("max_lean_left_deg")),
                                         d.get("max_lean_right_deg", m.get("max_lean_right_deg"))) if v is not None),
                            default=None),
        "max_g": d.get("max_g", m.get("max_g")), "fuel_l": fuel_l, "l100": l100, "alerts": m.get("alerts"),
        "codes": m.get("fault_codes") or [],
        "max_chip_c": m.get("max_chip_c"), "max_cell_c": m.get("max_cell_c"),
    }


def load_fillups(path):
    rows, _ = read_csv(path)
    out = []
    for i, r in enumerate(rows):
        out.append({
            "fill": num(r.get("fill")), "ride": r.get("ride", ""), "time": r.get("time") or "",
            "km": num(r.get("km_since_last")), "raw_l": num(r.get("raw_l_since_last")),
            "litres": num(r.get("litres_pumped")), "k": num(r.get("k")), "coverage_pct": num(r.get("coverage_pct")),
            # The first row only marks the starting point; demo rows aren't real.
            "usable": r.get("ride") != "demo" and num(r.get("fill")) != 1,
        })
    return out


def embed(data):
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")


def page(title, body_class, data_name, data, script):
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<link rel="stylesheet" href="assets/style.css">
</head>
<body class="{body_class}">
<main id="app"></main>
<script>window.{data_name} = {embed(data)};</script>
<script src="assets/charts.js"></script>
<script src="assets/{script}"></script>
</body>
</html>
"""


def build(logs, site):
    notes = {}
    if (logs / "notes.json").exists():
        notes = json.loads((logs / "notes.json").read_text())
    folders = sorted((p for p in logs.glob("ride_*") if p.is_dir() and re.fullmatch(r"ride_\d+", p.name)),
                     key=lambda p: int(p.name.split("_")[1]))
    site.mkdir(parents=True, exist_ok=True)
    (site / "assets").mkdir(exist_ok=True)
    for f in ASSETS.iterdir():
        if f.suffix in (".css", ".js"):
            shutil.copyfile(f, site / "assets" / f.name)
    rides, loaded = [], []
    for folder in folders:
        ride = load_ride(folder, notes)
        loaded.append(ride)
        m = ride["meta"]
        tyres = tyre_readings(ride["events"])
        if not tyres:
            drop_wake_readings(ride["samples"])
        ride["derived"] = derived(ride["samples"])
        data = {
            "derived": ride["derived"],
            "tyres": tyres,
            "meta": m,
            "series": bucket_series(ride["samples"], ride["columns"]),
            "columns": {c: COLUMNS[c][:2] for c in ride["columns"]},
            "events": ride["events"],
            "dist": distributions(ride["samples"]),
            "files": [{"name": n, "href": Path(os.path.relpath(folder / n, site)).as_posix()} for n in ride["files"]],
            "samples": len(ride["samples"]),
        }
        if tyres:
            data["series"].update(tyre_series(tyres, data["series"]["t"]))
            for c in ("front_kpa", "front_c", "rear_kpa", "rear_c"):
                if c in data["series"]:
                    data["columns"][c] = COLUMNS[c][:2]
        name = f"ride_{m['ride']:04d}.html"
        (site / name).write_text(page(f"Ride {m['ride']}", "ride", "RIDE", data, "ride.js"))
        rides.append(summary_row(ride) | {"href": name})
    bike = next((r["meta"]["bike"] for r in reversed(loaded) if r["meta"].get("bike")), "")
    index = {"rides": rides, "fillups": load_fillups(logs / "fillups.csv"), "bike": bike}
    (site / "index.html").write_text(page(f"{bike} rides" if bike else "Rides", "index", "SITE", index, "index.js"))
    # Pages of rides that are gone from logs/.
    keep = {r["href"] for r in rides}
    for old in site.glob("ride_*.html"):
        if old.name not in keep:
            old.unlink()
    return rides


def main_with(logs: Path, site: Path) -> None:
    rides = build(logs, site)
    print(f"{len(rides)} ride{'s' if len(rides) != 1 else ''} -> {site / 'index.html'}")


def main() -> None:
    if len(sys.argv) > 3:
        sys.exit(__doc__)
    main_with(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "logs",
              Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "site")


if __name__ == "__main__":
    main()
