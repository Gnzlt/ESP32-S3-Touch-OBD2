# How it works

## Layout

```
main/
  app_main.c       the board guard's wake check, NVS, display, splash, then the tasks
  app.h            the shared snapshot (dash_t), settings, requests from the UI
  bike/            bike.h and the bike profiles (profiles/*.h)
  obd_task.c       owns the ELM327 session (or the demo), ticks it every 20 ms, publishes its state
  sensor_task.c    50 Hz: IMU lean/G, gear, trip, alerts + beeps, coolant history; battery every 5 s
  sd_log.c         ride folders on the microSD: trip.csv, events.csv, obd_raw.log, ride.json (writer task,
                   5 s sync); reads ride.json back for the Rides view
  log_export.c     ride download and clock sync over USB (obd2 ls / get / sync), for tools/pull_logs.py
  tpms_service.c   the tyre sensors: decodes what the shared scan hears, pairing, tyre state, NVS, tpms.cfg
  board_power.c    deep sleep when the board switches itself off, and the check at each wake
  mem_report.c     internal RAM and stack use on the serial log
  ble/obd_link.c   NimBLE central: scan, connect, MTU, discover, subscribe, chunked writes, paced retries
  ble/ble_scan.c   the one BLE scan, shared by the adapter search and the tyre sensors
  elm/             reply assembler and parser; the ELM327 session state machine
  obd/             PIDs, state, poller, DTCs, alerts, the board guard, gear, trip, fuel, ride.json
  sensors/         motion (lean math), beeper (ES8311 tones), battery (BQ27220), chip_temp, wallclock and
                   rtc_regs (PCF85063)
  tpms/            tyre sensors, pure C: advertisement parser, the 8 formats, tyre state, alerts, pairing
  demo/            the synthetic demo ride
  ui/              shell (splash, pager, alerts, dialogs, sheets), one file per page, fonts and images
components/        Waveshare's BSP for the board, with two memory changes (see its README)
tools/             log download, ride website, gear fitting, artwork, fonts, screenshots
test/              host tests and ELM327 transcripts
```

Everything under `elm/`, `obd/`, `tpms/`, `demo/` and most of `sensors/` is plain C with no ESP-IDF, and
runs in the host tests.

## The adapter and the ELM327 session

Rules the session never breaks:
- one command in flight; the next goes out from the previous reply;
- a reply ends at the `>` prompt;
- a stray reply (a late answer after a timeout, or the adapter's banner arriving unasked at key-on) is
  dropped, never taken as the answer to the next command;
- only PIDs the ECU says it supports are polled;
- a disconnect is normal: the bike switching off cuts the adapter's power.

**Finding the adapter:** a scan matches the `18F0`, `FFF0` or `FFE0` services, or names like `VLINK`,
`VGATE`, `OBD`, `ELM`, `VEEPEAK`. It skips anything that decodes as a tyre sensor. The address, name and
detected protocol are saved, so later connects are quick. **Status → Forget adapter** starts over.

**Init:** `ATWS ATE0 ATL0 ATS0 ATH0 ATAT1 ATI`, then `0100` with the saved protocol (or a search), the ECU's
address locked with `ATCRA`, and the supported-PID bitmaps (`0100`, `0120`, `0140`). ECU-off needs six failed
requests in a row *and* 3 s with no value, so a few PIDs answering `NO DATA` can't end a session while RPM is
answering fine. After a timeout, the session checks that replies still line up with commands. If they
don't, it re-inits rather than reading every later reply one command late.

**Polling:** the display sits next to the adapter, so it polls as fast as replies come back. Jobs and their
periods: RPM + speed every 50 ms, throttle/load/MAP 150 ms, coolant/IAT/advance 1 s, fuel status + trim 2 s,
voltage (`ATRV`) 2 s, the rest 30 s. The most overdue job always goes next, so they interleave. When the ECU
accepts multi-PID requests, each group fits one CAN frame, and the `1` reply-count suffix lets the ELM
answer at once instead of waiting out its timeout. With a Vgate iCar Pro a reply takes about 30 ms, which is
about 30 requests a second. RPM and speed take what the others leave (16–20 Hz). Replies per second are on
the Status page.

**Fault codes:** Mode 03 (stored) and 07 (pending) at connect, and every minute after on ECUs without PID 01.
With PID 01, whenever its count or MIL changes. A new code raises an alert. **Clear codes** (Mode 04) is
refused while the engine runs.

**Mode 09**, once per session: `0900`, then the VIN (`0902`), calibration IDs (`0904`) and CVNs (`0906`)
the ECU has. They go to `ride.json`, and the calibration ID also goes to the Status page. These multi-frame
answers get a 1 s timeout, and a timeout there never restarts the link.

## Lean angle and G

The IMU is a QMI8658 at 50 Hz. While riding, lean comes from the turn itself:
tan(lean) = speed × yaw rate / g. That uses the speed PID, and the yaw is the gyro about the true vertical
(cos(lean) × up − sin(lean) × right). It's fused with the gyro's roll rate for quick changes. An
accelerometer alone reads about 0° in a steady turn. That same fact means its average while riding points
along the bike's own vertical, so the board learns which way is up within seconds of riding, however it's
mounted, and keeps following a case that shifts.

- Lean is only measured from 15 km/h (off below 10). Standing still, the tilt says nothing about the ride.
- **The gyro's offset** is learned whenever speed is 0 (nothing turns then) and subtracted. Uncorrected, a
  few °/s reads as a steady turn: ~10° of lean on every straight. It's in `ride.json` as `gyro_bias_dps`.
- **Jolts** (a board loose in a tank bag) are rejected. A sample turning or rolling faster than 200 °/s is
  skipped, lateral g is capped at 1.2, and for 3 s after a jolt the board re-learns which way is up.
- **Max lean and max G** are the largest values held for a full second, so a bump doesn't become the
  ride's record. The Lean page shows a half-second average, and the logs keep the raw value.

## Board power and heat

The board runs from USB (or the bike's 5 V supply) and/or its own LiPo, read through a BQ27220 gauge every 5 s.

- **Battery icon:** green, amber at 20 %, red under 10 %, breathing cyan while charging. Alerts at 20 % and
  10 %, which also turn on battery saver.
- **Heat:** two sensors, the ESP32-S3's own (its die, about 15 °C above the air around it) and the battery
  gauge's (on the board beside the cell connector). **Warning** at 80 °C chip / 50 °C battery area: an
  amber alert, and battery saver on. **Critical** at 90 °C / 58 °C, or the gauge's own over-temperature
  flag: a red alert. The levels are in `main/obd/alerts.h`. The backlight and the sun are the main heat
  sources, so give the board some shade.
- **Switching itself off** (`main/obd/board_guard.c`, host-tested): when the cell is empty (under 3.40 V for
  30 s, not charging) or the board is critically hot for 10 s. It writes a last `board` event, closes the
  ride, saves the trip and unmounts the card, shows why for 3 s, then goes into deep sleep. Software can't
  open the PWR latch, so "off" is deep sleep with the backlight held off. It wakes every 5 minutes to check,
  and starts again once USB power is back, the cell reads 3.7 V, or both temperatures are 5 °C under their
  warning levels.

**Battery saver** (Status → Battery saver, or automatic as above) is a black screen at 15 % backlight that
shows the link, the trip, the tyres and the board battery, updated every 30 s. Nothing else is drawn. Alerts
show at once and light it up for 6 s. A tap or BOOT peeks at the pages for 30 s. This is an IPS panel, so
black pixels alone save nothing: the saving comes from the backlight and from not rendering.

## Memory

The ESP32-S3 has about 340 KB of internal RAM for IRAM code, the heap, DMA, task stacks and the BLE
controller. The 8 MB of PSRAM takes everything else. `mem_report.c` logs internal RAM at each boot step and
every task's unused stack.

| Change | Where | Internal RAM won |
|---|---|---|
| One 40-row draw band in internal RAM, sent in one SPI transaction (PSRAM buffers made the SPI driver copy every chunk, and a failed copy dropped part of the frame) | BSP `bsp_display_lcd_init` | 72 KB of PSRAM buffers plus up to 36 KB of per-frame copies, for a fixed 28.8 KB |
| LVGL's heap in PSRAM | `ui/lv_mem_psram.c`, `CONFIG_LV_USE_CUSTOM_MALLOC` | about 41 KB |
| Stacks sized from measured use | `app_main.c`, `sd_log.c`, `sdkconfig.defaults` | 14 KB |
| Non-ISR FreeRTOS and ringbuf code, and the SPI ISR, in flash | `sdkconfig.defaults` | 24 KB |
| Big static buffers in PSRAM (`EXT_RAM_BSS_ATTR`) | several | 13.5 KB |
| No I2S RX (microphone) channel | BSP `bsp_audio_init` | about 6 KB |
| BLE controller trimmed to one scan + one connection; small USB ring buffers | `sdkconfig.defaults`, `log_export.c` | about 10 KB |

With BLE scanning, internal RAM after a minute is about 150 KB free and 120 KB at its lowest. The serial log
prints the frame rate every 10 s (`ui: 60.1 fps`).

## Assets

Generated files are committed, so a build needs only ESP-IDF. Regenerate them with the scripts, never by
hand:

- **Bike images:** `tools/make_bike_art.py` → `tools/art/bike.png`, then `tools/make_tyre_bike.py` and
  `tools/make_splash.py` (Pillow, numpy). See [configure-your-bike.md](configure-your-bike.md#artwork).
- **Fonts:** `tools/make_fonts.sh` runs Barlow / Barlow Condensed through `lv_font_conv` at 14–120 px (Node).
- **Screenshots:** build with `DASH_SCREENSHOTS` (and `DASH_SCREENSHOT_DELAY_S=160`), flash, and run
  `python3 tools/grab_screens.py <port> screenshots/` (pyserial, Pillow). The images are the real
  framebuffer. A screenshot build forces demo mode on and battery saver off without saving that. To keep your
  normal `build/`, build into another folder:
  `idf.py -B build-shots -D SDKCONFIG=build-shots/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;shots.defaults" build flash`,
  where `shots.defaults` holds the two `CONFIG_DASH_SCREENSHOT*` lines.
