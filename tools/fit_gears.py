"""
Fits the gear estimate to your bike from a logged ride. Each gear shows up in
trip.csv as a cluster of rpm / km/h; with the gear ratios known, one constant
K (rpm per km/h per unit of gear ratio: primary and final drive, the rear
tyre and the speed PID's own error) lines them all up.

    python3 tools/fit_gears.py logs/ride_0003/trip.csv [more trip.csv ...]
        lists the rpm per km/h clusters it finds (no ratios needed)
    python3 tools/fit_gears.py logs/ride_*/trip.csv --ratios 3.285,2.105,1.6,1.3,1.15,1.043
        fits K for those ratios and prints the lines for your bike profile

Ride a few minutes in every gear first; demo-mode rides aren't logged.
Plain Python, no dependencies.
"""
import argparse
import csv
from collections import Counter

MIN_KMH, MIN_RPM = 15, 1500    # slower than this the speed PID is too coarse
TOLERANCE = 0.045              # as BIKE_GEAR_TOLERANCE


def samples(paths):
    out = []
    for path in paths:
        with open(path, newline="") as f:
            for r in csv.DictReader(line for line in f if not line.startswith("#")):
                try:
                    rpm, kmh = float(r["rpm"]), float(r["kmh"])
                except (KeyError, TypeError, ValueError):
                    continue
                if kmh >= MIN_KMH and rpm >= MIN_RPM:
                    out.append(rpm / kmh)
    return out


def clusters(ratios, width=1.0, top=8):
    """The most common rpm per km/h values, in bins `width` wide, merged when adjacent."""
    bins = Counter(round(r / width) for r in ratios)
    floor = len(ratios) * 0.005   # rarer than this is a shift, not a gear
    peaks = [b for b, n in bins.items() if n >= floor and n >= bins.get(b - 1, 0) and n >= bins.get(b + 1, 0)]
    peaks.sort(key=lambda b: -bins[b])
    return sorted((b * width, bins[b]) for b in peaks[:top])


def matched(ratios, gears, k):
    return sum(1 for r in ratios if any(abs(r / k - g) <= g * TOLERANCE for g in gears))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trip_csv", nargs="+")
    ap.add_argument("--ratios", help="the gearbox ratios, 1st to top, comma-separated")
    args = ap.parse_args()
    ratios = samples(args.trip_csv)
    if len(ratios) < 200:
        raise SystemExit(f"too few moving samples (>= {MIN_KMH} km/h, >= {MIN_RPM} rpm) in those files")
    print(f"{len(ratios)} moving samples")
    if not args.ratios:
        print("rpm per km/h clusters (each gear should be one; shifts and the clutch scatter the rest):")
        for v, n in clusters(ratios):
            print(f"  {v:6.1f}  {n} samples")
        print("Divide each by its gear ratio: the results should agree, and that is BIKE_GEAR_K.")
        return
    gears = [float(g) for g in args.ratios.split(",")]
    best = max((matched(ratios, gears, k / 100), k / 100) for k in range(1000, 20000, 5))
    n, k = best
    print(f"BIKE_GEAR_K {k:.2f}: {n} of {len(ratios)} samples ({100 * n / len(ratios):.0f} %) fall in a gear's band")
    for i, g in enumerate(gears, 1):
        print(f"  gear {i}: {g * k:6.1f} rpm per km/h")
    print("\nFor your profile:")
    print(f"#define BIKE_GEAR_RATIOS     {{{', '.join(f'{g}f' for g in gears)}}}")
    print(f"#define BIKE_GEAR_K          {k:.2f}f")


if __name__ == "__main__":
    main()
