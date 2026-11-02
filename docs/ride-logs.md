# Ride logs

With a microSD card in the board, every ride is logged: every value, every event and every byte of the
OBD2 traffic. Demo mode isn't logged (unless `DASH_LOG_DEMO` is on).

## On the card

```
/obd2/ride_0007/trip.csv       samples, 5 rows a second, every column a number or empty
/obd2/ride_0007/events.csv     what happened when: alerts, new fault codes, OBD phase changes, fill-ups
/obd2/ride_0007/ride.json      the ride's totals and details (rewritten every 30 s)
/obd2/ride_0007/obd_raw.log    every ELM327 command and reply, exactly as on the wire
/obd2/fillups.csv              one row per fill-up marked on the Fuel page (the fuel calibration points)
/obd2/tpms.cfg                 the tyre sensors and their alert limits (docs/tyre-sensors.md)
```

- **When a ride starts and ends:** a ride opens when the ECU first answers. It closes after 5 minutes without
  the ECU, so a stall or a fuel stop stays one ride. Rows stop 10 s after the ECU goes quiet and resume when it
  comes back. Ride numbers continue from the highest folder on the card and never go below the number the board
  keeps in NVS, so a formatted card carries on from the last ride.
- **Power cuts:** files are flushed and synced every 5 s, so cutting the power (ignition off) loses at most
  that. If the card is removed or fills up, logging stops cleanly, the board retries the mount every 30 s, and a
  new ride starts once a card is back.
- **Size:** roughly 1 KB/s of raw traffic plus 0.4 KB/s of CSV, about 5 MB an hour.
- **Formatting:** Status → Format SD card (with a confirm) makes FAT32 with 32 KB clusters. The firmware never
  formats on its own. After a format the board writes its `tpms.cfg` back.
- **Date and time:** the board has no network. Its PCF85063 clock (UTC) is set by `tools/pull_logs.py` at each
  download, and keeps running from the board's LiPo while the bike is off. Without a cell the clock stops
  when the power goes, and rides are undated until the next download.

### trip.csv

Read the columns by the header row's names. A cell is empty when that value wasn't fresh.

| Column | |
|---|---|
| `t_s` | seconds since the ride started |
| `rpm`, `kmh`, `gear` | gear is the estimate, empty when none |
| `throttle_pct`, `load_pct`, `map_kpa`, `coolant_c`, `iat_c`, `advance_deg`, `stft_pct` | as the ECU reports them |
| `fuel_sys` | PID 03's raw byte: 1 open loop cold, 2 closed loop, 4 open loop for load or decel fuel cut, 8/16 open loop on a fault |
| `fuel_lph_raw`, `fuel_ml_raw` | the estimated fuel flow and the ride's running total, uncalibrated (see "Fuel estimate") |
| `volts` | battery voltage (the adapter's `ATRV`) |
| `lean_deg`, `lat_g`, `lon_g`, `trip_km` | lean (left negative), lateral and longitudinal g, distance |
| `board_soc`, `board_mv`, `board_ma` | the board's LiPo: charge, voltage, current (+ charging); empty with no cell |
| `front_kpa`, `front_c`, `rear_kpa`, `rear_c` | tyres, gauge kPa and °C; empty when unpaired or not heard for 2 minutes |
| `chip_c`, `cell_c` | the ESP32-S3's temperature and the battery gauge's (beside the cell) |

### events.csv

Columns `t_s` (the same clock as `trip.csv`), `type` and `detail`:

- `alert`: `coolant`, `volt_high`, `volt_low`, `dtc`, `board_batt`, `board_hot`, or `tyre_loss`, `tyre_low`,
  `tyre_alarm`, `tyre_hot`, `tyre_high`, `tyre_cold`, `tyre_silent`, `tyre_batt` followed by the tyre
  (`tyre_low rear`);
- `code`: a fault code seen for the first time this ride (`P0118`);
- `obd`: the link's phase changed: `scan`, `connect`, `init`, `ecu_off` or `live`;
- `fillup`: litres pumped (`9.6 L`);
- `display`: battery saver switched (`saver on`, `saver off`, `saver on (hot)`, `saver off (cooled)`);
- `board`: the board switched itself off (`off battery empty`, `off too hot`);
- `tyre`: a paired sensor's reading, `front 194 21 -86` (gauge kPa, °C, signal in dBm), when it changes or at
  most once a minute.

### ride.json

One JSON object with `"format": 2`. Units are in the key names, and `null` means unknown.
[`test/fixtures/ride-format2.json`](../test/fixtures/ride-format2.json) is an example.

- **Identity:** `ride`, `uid` (8 random hex digits), `in_progress` (true until the ride's end is written;
  still true after a power cut), `start` (UTC, `null` when the clock wasn't set), `firmware`, `bike` (the
  profile's name).
- **Totals:** `duration_s`, `engine_s`, `distance_km`, `avg_kmh` (moving), `max_kmh`, `max_rpm`,
  `max_coolant_c`, `min_volts` (after 10 s of running, so not while cranking), `max_lean_left_deg`,
  `max_lean_right_deg` and `max_g` (each held for a second), `gyro_bias_dps`, `alerts`, `fault_codes`.
- **Fuel and battery:** `fuel_raw_l`, `fuel_raw_l100`, `fuel_k`, `batt_start_pct`, `batt_end_pct`,
  `max_chip_c`, `max_cell_c`.
- **Nested:** `fuel` (model, coverage_pct, since_fill_km, since_fill_raw_l, fills), `board_battery` (cell,
  end_mv, charging, avg_ma, full_mah, remaining_mah), `tyres.front` / `tyres.rear` (paired, min_kpa,
  max_kpa, max_c, heard, min_rssi_dbm), `adapter` (name, elm), `ecu` (protocol, address, vin, cal_ids,
  cvns), `link` (timeouts, reinits, link_drops, ecu_drops).

It's written to `ride.json.tmp` first and then renamed, so a power cut always leaves a whole copy.

### obd_raw.log

Seconds since boot, `>` (sent) or `<` (received), then the line: `  1234.567 > 010C0D1`,
`  1234.612 < 410C12D40D00`. Lines starting with `#` mark link up/down, phase changes, dropped lines and
tyre-sensor readings. The header line says which second is `t_s` 0 in the CSVs. The file opens with up to 16 KB of
the traffic from before the ride opened (the connect, the ELM init, protocol detection), between
`# --- before the ride started ---` and `# --- ride started ---`. It's the raw material for new test
fixtures. If your bike misbehaves, attach it to an issue (it holds your VIN, if the ECU reports one).

## Downloading over USB

With the board on the cable and running:

```bash
pip install pyserial
python3 tools/pull_logs.py /dev/cu.usbmodem1101      # Windows: COM5; Linux: /dev/ttyACM0
```

It copies every ride and `fillups.csv` to `logs/` (git-ignored) at about 450 KB/s, without resetting the
board, and then builds the website. Size and CRC are checked, complete files are skipped and growing ones
resumed. `ride.json` is fetched whole each time, and if `logs/ride_NNNN` already holds a different ride
(another `uid`), that folder is skipped rather than overwritten. `fillups.csv` is merged row by row. First it
sets the board's clock and raises its next ride number above the highest one already in `logs/`.

The firmware side is `main/log_export.c`, which answers `obd2 ls`, `obd2 get` and `obd2 sync` on the
USB-Serial-JTAG port.

## The ride website

`python3 tools/build_site.py` turns `logs/` into `site/` (`pull_logs.py` runs it for you). Open
`site/index.html` straight from disk: plain HTML, CSS and JS with each page's data embedded, so no server,
internet or libraries are needed.

- **Overview:** totals (rides, km, riding and engine time, fuel economy, top speed, max lean, alerts),
  per-ride charts, a sortable table of every ride, and the fill-ups with the fit that gives the fuel `k`.
- **Each ride:** tiles; charts on one shared time axis (speed, RPM, gear, throttle and load, MAP,
  temperatures, voltage, fuel, ignition, trim, lean, G, tyres, board battery and temperature, distance).
  Drag across a chart to zoom them all and double-click to reset. Events are markers on every chart. Also
  histograms, a G-G diagram, the fuel loop states, the ride's details and links to the raw files.
- **Data:** charts draw one point a second (lean and G keep each second's peak); tiles use every row. Max
  lean and max G are the largest values *held for a second* while riding, so a jolt doesn't count.
- **Notes:** an optional, hand-written `logs/notes.json` adds a note to a ride, or a date to one logged
  before the clock was set: `{"ride_0003": {"date": "2026-05-01", "note": "Alps, day 1"}}`.
- The page code is in `tools/site/` and is copied to `site/assets/`. Edit it there.

## Fuel estimate

Most bikes have no MAF and no fuel-level or fuel-rate PID, so fuel is **estimated** (`main/obd/fuel.c`,
host-tested):

- **Air from the calculated load (PID 04).** By its SAE definition, load is the airflow as a share of the
  peak wide-open airflow at that rpm, so air = load × 1.184 kg/m³ × displacement × VE_peak × rpm / 120.
  Displacement and VE_peak come from the bike profile.
- **MAP isn't used.** On a twin, each MAP sample catches one instant of the intake pulses: 33–97 kPa at a
  steady idle, while load held 27 % ± 1.
- **Air-fuel ratio from the fuel system status (PID 03):** closed loop 14.7, scaled by the short-term trim.
  Open loop cold or on a fault: 13.5. Status 4 covers a closed-throttle idle (fuelled normally), decel fuel
  cut (load < 15 % above 2,000 rpm, counted as zero) and power enrichment (load ≥ 70 %, 12.5).
- **Raw and calibrated:** the logs keep the **raw** estimate, the model version and the calibration factor
  `k`. The Fuel page shows raw × `k`, and says "uncalibrated" while `k` is 1.
- **Range:** (tank − fuel used since the fill-up) ÷ the L/km average since then, shown from 5 km on.

**Fill-ups are the calibration points.** Fill to the brim, tap **Filled up** on the Fuel page and enter the
litres pumped. That appends a row to `fillups.csv`
(`fill, ride, uptime_s, km_since_last, raw_l_since_last, litres_pumped, k, model, coverage_pct, time`) and an
event to the ride, and restarts the count. The count survives reboots. The first row only marks the
starting point. After that, `k` = Σ litres pumped ÷ Σ raw litres over fill-ups 2 onward (the website
shows the fit). Calibration isn't applied automatically yet. Rows with `ride` = `demo` came from demo mode.
