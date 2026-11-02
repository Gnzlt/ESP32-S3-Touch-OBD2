"""
Builds the site from test/fixtures/site/ (one ride and the fill-ups) into a
temporary folder and checks what the pages carry.

    python3 -m unittest tools/test_build_site.py
"""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_site  # noqa: E402

FIXTURES = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "site"


def page_data(path: Path, name: str):
    m = re.search(rf"window\.{name} = (.*?);</script>", path.read_text(), re.S)
    return json.loads(m.group(1).replace("<\\/", "</"))


class BuildSite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.site = Path(cls.tmp.name) / "site"
        build_site.build(FIXTURES, cls.site)
        cls.index = page_data(cls.site / "index.html", "SITE")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_files(self):
        for name in ("index.html", "ride_0002.html", "assets/style.css", "assets/charts.js",
                     "assets/ride.js", "assets/index.js"):
            self.assertTrue((self.site / name).exists(), name)

    def test_index(self):
        rides = {r["ride"]: r for r in self.index["rides"]}
        self.assertEqual(sorted(rides), [2])
        new = rides[2]
        self.assertEqual(self.index["bike"], "CB500X")
        self.assertEqual((new["format"], new["start"], new["note"]), (2, "2026-10-17T09:12:03Z", "Test ride"))
        self.assertAlmostEqual(new["fuel_l"], 2.61 * 1.08, places=4)
        self.assertAlmostEqual(new["l100"], 4.11 * 1.08, places=4)
        self.assertEqual(new["codes"], ["P0118", "P0562"])
        fills = self.index["fillups"]
        self.assertEqual([f["usable"] for f in fills], [False, True, False])

    def test_new_ride(self):
        d = page_data(self.site / "ride_0002.html", "RIDE")
        s = d["series"]
        self.assertEqual(s["t"], list(range(7)))
        self.assertEqual(len(s["kmh"]), 7)
        self.assertNotIn("rear_kpa", s)          # never heard: no series at all
        # Lean keeps each second's peak, not the mean.
        self.assertEqual(s["lean_deg"][1], -14.0)   # samples -14.0 to -8.0: the mean would be -11
        self.assertEqual([e["type"] for e in d["events"]], ["obd", "code", "alert", "fillup"])
        self.assertEqual(d["dist"]["gear"], {"1": 2.0, "2": 2.0, "3": 2.0})
        self.assertEqual(d["meta"]["ecu"]["vin"], "TESTVIN0000000000")
        self.assertEqual(s["cell_c"][0], 40.0)
        self.assertEqual(d["meta"]["max_chip_c"], 71.5)
        self.assertEqual(d["files"][0]["href"].split("/")[-2:], ["ride_0002", "events.csv"])


def rows(values, key, dt=0.2, **extra):
    return [{"t": round(i * dt, 3), key: v, **extra} for i, v in enumerate(values)]


class Helpers(unittest.TestCase):
    def test_held_peaks(self):
        # 2 s at 30 with a 0.4 s spike to 70: 30 was held, 70 wasn't.
        self.assertEqual(build_site.held_peaks(rows([30] * 4 + [70] * 2 + [30] * 4, "v"), lambda s: s["v"]), (30, None))
        # A tank-bag tumble, 0.2 s a value: -70..-37 is the second held.
        tumble = [41, -24, -70, -55, -35, -54, -37, -29, -28, -9]
        self.assertEqual(build_site.held_peaks(rows(tumble, "v"), lambda s: s["v"]), (None, 35))
        # A gap over 0.5 s breaks the second.
        broken = rows([20] * 3, "v") + [{"t": 1.5, "v": 20}, {"t": 1.7, "v": 20}]
        self.assertEqual(build_site.held_peaks(broken, lambda s: s["v"]), (None, None))

    def test_min_volts_running(self):
        crank = [{"t": i * 0.2, "rpm": 1678 if i < 10 else 1200, "volts": 10.6 if i < 10 else 14.0} for i in range(100)]
        self.assertEqual(build_site.min_volts_running(crank), 14.0)

    def test_wake_readings(self):
        s = rows([None, 1, 1, 250, 2, 249], "rear_kpa")
        build_site.drop_wake_readings(s)
        self.assertEqual([x["rear_kpa"] for x in s], [None, None, None, 250, 2, 249])   # only before the first real one

    def test_tyre_readings(self):
        events = [{"t": 3.2, "type": "tyre", "detail": "front 194 21 -86"},
                  {"t": 5.0, "type": "alert", "detail": "tyre_low rear"},
                  {"t": 6.0, "type": "tyre", "detail": "front 196  -80"}]
        r = build_site.tyre_readings(events)
        self.assertEqual(r, {"front": [[3.2, 194, 21, -86], [6.0, 196, None, -80]]})
        s = build_site.tyre_series(r, list(range(9)), hold_s=4)
        self.assertEqual(s["front_kpa"], [None, None, None, 194, 194, 194, 196, 196, 196])
        self.assertEqual(s["front_c"], [None, None, None, 21, 21, 21, 21, 21, None])   # held 4 s, then gone


if __name__ == "__main__":
    unittest.main()
