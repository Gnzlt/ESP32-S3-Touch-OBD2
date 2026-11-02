/*
 * Gear estimate from RPM and speed: standard OBD2 has no gear PID. The ratio
 * rpm/km/h is compared with each gear's; a gear is shown only while the ratio
 * sits within its band (clutch in, neutral or a slipping clutch read 0), and
 * only once it has held for a few samples, so it doesn't flicker on a shift.
 * The gearing comes from the bike profile (bike/bike.h); a profile without
 * gear ratios has no estimate.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    int candidate;
    int count;
    int shown;      // 0 = unknown / neutral / clutch in
} gear_t;

// The profile's number of gears; 0 when it gives no ratios.
int gear_count(void);

// Engine rpm per km/h in that gear (1-based), 0 for no such gear.
float gear_rpm_per_kmh(int gear);

// The gear whose ratio matches, or 0.
int gear_match(float rpm, float kmh);

// Feed one RPM + speed sample (valid = both fresh). Returns the gear to show.
int gear_update(gear_t *g, bool valid, float rpm, float kmh);
