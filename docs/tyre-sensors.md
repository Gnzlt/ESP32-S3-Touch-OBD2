# Tyre sensors (TPMS)

The board reads BLE tyre-pressure sensors: the valve-cap and in-wheel kinds that broadcast their readings
in advertisements. It never connects to them, so a kit's own display or phone app keeps working alongside.

## Formats

`main/tpms/` decodes 8 open broadcast formats: **Sysgration / "TPMSII"**, **SYTPMS BR/KY**, **Michelin**,
**WODHMIEY**, **FOBO**, **Bekubee**, **Tesla-protocol** and **DJTPMS** (sold in many aftermarket motorcycle
kits). Every published capture of each format is a host test (`test/host/test_tpms.c`). Each advertisement
is split into its AD structures (length, type, data), and a packet cut short still yields what it holds.

Sensors that only talk to their own receiver (433 MHz, or BLE with a connection) can't be read.

## Pairing

On the **Tyres** page, long-press a tyre, or hold the page (or the BOOT button) to pair the front and
then the rear. The sheet lists every sensor heard, strongest first: the end of its id, its format, its
pressure and its signal. The id is what the sensor's app or the kit's display shows
(`ID:3CAB72xxxx26` ends in `xx26`). Hold the board near the valve, or spin the wheel to wake a parked
sensor, then tap the right one. A sensor already on the other tyre is marked, and moves over if you pick it.

## Alerts

Tap a tyre for its sheet. **Low alert** and **High alert** are each a pressure (in your unit) with an on/off
switch. Low starts at the bike profile's `BIKE_TYRE_LOW_*_KPA` (by default 10 % under the cold pressures),
and high is off. The other alerts:

- **Fast loss:** 15 % within 5 minutes;
- the sensor's own **leak alarm**;
- **hot:** 80 °C;
- **silent:** not heard for 30 minutes while riding;
- **low sensor battery:** once a ride.

Low and fast loss wait until the pressure has stayed low for 20 s and a later packet still shows it,
because some sensors read about 1 kPa for their first few seconds after waking. Each alert fires once and
re-arms when the value is back past a small margin (0.05 bar, 3 °C), so a tyre sitting on its limit
doesn't keep beeping. The overlay names the tyre ("REAR LOW", "1.72 bar"). Losing air, low and the leak
alarm beep urgently.

## tpms.cfg

The pairings and every limit live in `/obd2/tpms.cfg` on the card, as `key = value` lines (pressures in
gauge kPa). The board writes it whenever a pairing or limit changes, and reads it whenever a card mounts.
A file on the card wins over what the board had, so a card with this file sets up a freshly flashed board,
or another board, with no pairing. With no file on the card, the board writes its own. A file with nothing
the board recognises is left alone. You can edit it on a computer, and unknown keys are ignored.

## The radio

NimBLE runs one scan at a time, so the adapter search and the tyre sensors share it (`main/ble/ble_scan.c`).
It listens for tyres only once one is paired or pairing runs, passively: 50 ms of every 100, or 30 ms of every
100 while the adapter is connected, so the OBD link keeps most of the radio. The scan pauses for the second or
two of a connection attempt. **Status → Tyre sensors** switches it off.

Some sensors broadcast the `FFE0` service that HM-10 style OBD adapters use too. The adapter search skips
anything that decodes as a tyre sensor.

## Demo mode

Two synthetic tyres at the bike profile's pressures: in every 2 minutes, the rear starts leaking at 1:00
and drops below its low alert about 17 s later, and the front runs hot after 1:35. Pairing in demo mode lists them, and nothing is saved.
