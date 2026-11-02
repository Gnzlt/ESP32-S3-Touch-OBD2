# Troubleshooting

The serial log (`idf.py -p <port> monitor`) explains most of what the board does: the link (`link:`), the
session (`obd:`), alerts and memory.

**The screen stays black after power-on.** Don't hold BOOT while the board powers on or resets: it's a
strapping pin, and the chip starts in USB download mode. Release it and press PWR (or reset).

**It never finds the adapter ("SEARCHING").**
- Close every phone app that might be connected to it (Car Scanner, OBD Fusion…). The adapter takes one
  connection at a time, and some apps reconnect in the background.
- It must be a **BLE** adapter. Classic-Bluetooth (SPP) and Wi-Fi ELM327s can't be used.
- Check that the adapter has power: many bike diagnostic plugs are only live with the ignition on.
- If another adapter or device nearby is picked instead, set `DASH_ADAPTER_NAME` in menuconfig to part of
  your adapter's name.
- After swapping adapters, use **Status → Forget adapter**.

**"IGNITION OFF" with the engine running.** The adapter connected, but no ECU answered `0100`. Check the
cable: the pinout must suit your bike (cables for other makes or models often look identical). Check too
that your bike speaks OBD2 at all (see the README). The serial log and the ride's `obd_raw.log` show what
was sent and what came back.

**Some values show `--`.** Your ECU doesn't report that PID. Its answers to `0100`, `0120` and `0140` in
`obd_raw.log` are the bitmaps of what it supports. Many
bikes report no fuel level or rate, no oil temperature and no PID 01 (MIL status).

**The gear shows `-` all the time.** Either the profile has no `BIKE_GEAR_RATIOS` (then the badge is hidden),
or `BIKE_GEAR_K` is off by more than a few percent. Fit it from a ride with `tools/fit_gears.py`
([configure-your-bike.md](configure-your-bike.md#the-gear-estimate)).

**Lean reads left in right turns.** Flip `MOTION_FLIP` in `main/sensor_task.c` (the board is mounted the other
way round from the one it was developed on). Lean only shows from 15 km/h.

**Replies per second are low.** Each adapter answers at its own pace. Turning off **Status → Tyre sensors**
gives the OBD link the whole radio, which shows how much the tyre scan costs.

**Rides have no date.** The board's clock is set by `tools/pull_logs.py` and runs from the LiPo. Without a
cell it stops when the power goes. `logs/notes.json` can date a ride by hand.

**The SD card isn't mounted.** Use a FAT32 card (or format it from Status → Format SD card, which erases it).
The board retries the mount every 30 s.

**The build stops with "Bike profile 'x' not found".** The menuconfig value must match a file name in
`main/bike/profiles/` without `.h`.
