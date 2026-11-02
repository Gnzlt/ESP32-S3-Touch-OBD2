# Configure your bike

The dashboard keeps everything that depends on the motorcycle in one header, a **bike profile**, in
[`main/bike/profiles/`](../main/bike/profiles/). Two come with it:

- [`generic.h`](../main/bike/profiles/generic.h): safe defaults for any bike, and the template for yours.
  It has no gear ratios, so the Ride page shows no gear.
- [`honda_cb500x.h`](../main/bike/profiles/honda_cb500x.h): a complete profile for a 2022 Honda CB500X, the
  bike the dashboard was developed and field-tested on.

## Make your profile

1. Copy `main/bike/profiles/generic.h` to `main/bike/profiles/<your_bike>.h`, for example `yamaha_mt07.h`.
2. Fill in the values below.
3. `idf.py menuconfig` → **OBD2 dashboard** → **Bike profile**: enter `<your_bike>` (no `.h`).
4. `idf.py build flash`.

A misspelled name stops the build with the path it looked for. A profile missing a required value stops
it with the value's name.

## The values

**Required:**

| Value | What it does | Where to find it |
|---|---|---|
| `BIKE_NAME` | Shown on the boot splash and the Status page. About 12 characters fit | Anything you like |
| `BIKE_RPM_MAX` | The end of the rev counter on the Ride page, in whole thousands (`12000.0f`) | Your bike's tacho |
| `BIKE_RPM_RED` | Where the red zone starts. The needle turns amber 1,000 rpm before it | The tacho's red line |
| `BIKE_DISPLACEMENT_L` | Engine displacement in litres (689 cc = `0.689f`). Drives the fuel estimate | Spec sheet |
| `BIKE_TANK_L` | Tank capacity in litres. The Fuel page's range is the tank minus what's been used since the fill-up | Spec sheet; take the lower figure when sources differ |
| `BIKE_TYRE_FRONT_KPA`, `BIKE_TYRE_REAR_KPA` | Cold tyre pressures in gauge kPa (bar × 100, psi × 6.895). The low-pressure alerts default to 10 % under them | The label on the swingarm or chain guard, or the owner's manual |

**Optional** (defaults in [`main/bike/bike.h`](../main/bike/bike.h)):

| Value | Default | What it does |
|---|---|---|
| `BIKE_RPM_IDLE` | 1300 | Warm idle. The demo ride idles here |
| `BIKE_COOLANT_COLD` | 60 °C | Below this the coolant gauge is blue (warming up) |
| `BIKE_COOLANT_WARN` | 100 °C | Amber |
| `BIKE_COOLANT_ALERT` | 105 °C | Red, plus the "ENGINE HOT" alert |
| `BIKE_VOLT_LOW` | 11.8 V | With the engine running for 10 s: "NOT CHARGING" |
| `BIKE_VOLT_HIGH` | 15.2 V | For 5 s: "VOLTAGE HIGH" (a failing regulator) |
| `BIKE_VOLT_CHARGING` | 13.2 V | Above this the Engine page says "charging" |
| `BIKE_GEAR_RATIOS` | none | Gearbox ratios, 1st to top: `{2.846f, 1.947f, …}`. Without it there's no gear estimate |
| `BIKE_GEAR_K` | from the three below | rpm per km/h per unit of gear ratio (see below) |
| `BIKE_PRIMARY_RATIO`, `BIKE_FINAL_RATIO`, `BIKE_REAR_TYRE_CIRC_M` | | Primary reduction, final reduction (rear sprocket teeth ÷ front), rear tyre rolling circumference in metres |
| `BIKE_GEAR_TOLERANCE` | 0.045 | A measured ratio within 4.5 % of a gear's counts as that gear |
| `BIKE_GEAR_MIN_KMH` | 8 | No gear below this speed (the speed PID moves in whole km/h) |
| `BIKE_VE_PEAK` | 0.90 | The engine's volumetric efficiency at full throttle, for the fuel estimate. Fill-up calibration absorbs its error |
| `BIKE_TYRE_LOW_FRONT_KPA`, `BIKE_TYRE_LOW_REAR_KPA` | 90 % of the cold pressures | The default low-pressure alerts. You can change them on the Tyres page too |

The coolant, voltage and tyre values are only starting points: alerts, the Engine page's colours and the
tyre limits all follow them.

## The gear estimate

OBD2 has no gear PID, so the dashboard divides RPM by road speed. In each gear that ratio is constant:

    rpm per km/h = K × gear ratio
    K = primary ratio × final ratio × 1000 / 60 / rear tyre circumference (m)

A gear is shown once the measured ratio has stayed within 4.5 % of that gear's for a few samples. With the
clutch in, mid-shift or in neutral, it shows `-`.

**From the spec sheet.** The gearbox ratios, primary reduction and sprockets are in the owner's or
service manual, and on most spec sites. For the rear tyre's circumference, take the size (180/55 ZR17):
diameter = 17 × 25.4 + 2 × 180 × 0.55 = 630 mm, and circumference ≈ π × 0.630 × 0.97 = 1.92 m. The
0.97 allows for the tyre squashing under load. That's usually within a couple of percent, close enough
to start.

**From a ride (more accurate).** K also absorbs the speed PID's own error, which can be a few percent. Log
a ride that uses every gear, pull it with `tools/pull_logs.py`, then:

```bash
python3 tools/fit_gears.py logs/ride_0001/trip.csv
#   lists the rpm per km/h clusters: one per gear
python3 tools/fit_gears.py logs/ride_0001/trip.csv --ratios 2.846,1.947,1.556,1.333,1.190,1.083
#   fits K and prints the two #define lines for your profile
```

If you don't know the ratios at all, the clusters alone are enough. Divide each cluster by the top-gear
cluster to get ratios relative to top gear, then use those with `BIKE_GEAR_K` equal to the top-gear
cluster. On the CB500X, 93 % of moving samples land in a gear band. The rest are shifts and clutch work.

## Artwork

The boot splash, the parked screen and the Tyres page show a motorcycle drawn in code by
[`tools/make_bike_art.py`](../tools/make_bike_art.py), a generic bike that isn't any particular make. To
show your own bike, you need:

- **A line drawing:** white lines on black, side view, **facing right** (rear wheel on the left), around
  2000 px wide. Trace a photo in any drawing app.
- **Each tyre's centre and outer radius** in that drawing, in pixels.

Then, with Pillow and numpy (`pip install pillow numpy`):

```bash
python3 tools/make_tyre_bike.py my_bike.png --rear 433,1361,258 --front 1603,1365,271
python3 tools/make_splash.py my_bike.png     # or a colour illustration drawn for a black background
```

They write `main/ui/assets/tyre_bike.c`/`.h` and `splash_bike.c`, with previews in `tools/`. The Tyres
page puts its pressure rings over the tyres using the positions you gave. Don't commit artwork you don't
have the rights to.

## Things that don't need a profile

- **Which values the ECU reports.** The dashboard asks the ECU (PIDs 00, 20, 40) and only polls what it
  supports. Anything it doesn't support shows `--`.
- **The protocol.** The adapter detects it on the first connect, and the dashboard remembers it.
- **The lean angle.** No calibration and no mounting angle: the board learns which way is up while
  riding. If it reads left for right, flip `MOTION_FLIP` in `main/sensor_task.c`.
- **Units.** Tyre pressures show in bar, psi or kPa (Status page). Speed and distance are km/h and km,
  as the speed PID reports them.

## Share it

If your profile works on your bike, a pull request adding it to `main/bike/profiles/` helps the next rider.
Please give the model years, and say which values you measured and which came from a spec sheet.
