#include <math.h>
#include <string.h>
#include "alerts.h"
#include "bike/bike.h"
#include "board_guard.h"

#define REPEAT_MS          300000   // the same alert again after 5 minutes
#define COOLANT_HYST       3.0f     // °C below the limit to re-arm
#define VOLT_LOW_SUSTAIN   10000
#define VOLT_HIGH_SUSTAIN  5000
#define RUNNING_RPM        800.0f   // below this the alternator is not charging
#define BATT_REARM         5        // board battery points above a level to re-arm it

static const int BATT_LEVELS[2] = {BOARD_BATT_LOW_PCT, 10};

void alerts_init(alerts_t *a)
{
    memset(a, 0, sizeof(*a));
    a->enabled = true;
    a->coolant_limit = BIKE_COOLANT_ALERT;
    a->volt_low = BIKE_VOLT_LOW;
    a->volt_high = BIKE_VOLT_HIGH;
    a->coolant_armed = true;
    a->low_since = -1;
    a->high_since = -1;
    a->batt_armed[0] = a->batt_armed[1] = true;
    a->chip_armed[0] = a->chip_armed[1] = a->cell_armed[0] = a->cell_armed[1] = true;
    a->hot_c = NAN;
    for (int i = 0; i < ALERT_COUNT; i++) {
        a->shown_at[i] = -1;
    }
}

static bool repeat_due(const alerts_t *a, alert_t al, int64_t now)
{
    return a->shown_at[al] < 0 || now - a->shown_at[al] >= REPEAT_MS;
}

static alert_t shown(alerts_t *a, alert_t al, int64_t now)
{
    a->shown_at[al] = now;
    a->fired++;
    return al;
}

alert_t alerts_evaluate(alerts_t *a, bool has_coolant, float coolant, bool has_volts, float volts,
                        bool has_rpm, float rpm, int dtc_total, bool mil, int64_t now)
{
    // Track sustain timers on every call, even when an alert fires.
    bool running = has_rpm && rpm >= RUNNING_RPM;
    if (has_volts && running && volts < a->volt_low) {
        if (a->low_since < 0) a->low_since = now;
    } else {
        a->low_since = -1;
    }
    if (has_volts && volts > a->volt_high) {
        if (a->high_since < 0) a->high_since = now;
    } else {
        a->high_since = -1;
    }
    if (has_coolant && !a->coolant_armed && coolant <= a->coolant_limit - COOLANT_HYST) {
        a->coolant_armed = true;
    }
    // Remember a new code until it is alerted: a more urgent alert in the same
    // second must delay it, not swallow it.
    if (dtc_total > a->last_dtc_total || (mil && !a->last_mil)) {
        a->dtc_pending = true;
    }
    a->last_dtc_total = dtc_total;
    a->last_mil = mil;

    if (!a->enabled) {
        a->dtc_pending = false;
        return ALERT_NONE;
    }
    if (has_coolant && coolant >= a->coolant_limit && (a->coolant_armed || repeat_due(a, ALERT_COOLANT, now))) {
        a->coolant_armed = false;
        return shown(a, ALERT_COOLANT, now);
    }
    if (a->high_since >= 0 && now - a->high_since >= VOLT_HIGH_SUSTAIN && repeat_due(a, ALERT_VOLT_HIGH, now)) {
        return shown(a, ALERT_VOLT_HIGH, now);
    }
    if (a->low_since >= 0 && now - a->low_since >= VOLT_LOW_SUSTAIN && repeat_due(a, ALERT_VOLT_LOW, now)) {
        return shown(a, ALERT_VOLT_LOW, now);
    }
    if (a->dtc_pending) {
        a->dtc_pending = false;
        return shown(a, ALERT_DTC, now);
    }
    return ALERT_NONE;
}

alert_t alerts_battery(alerts_t *a, bool present, int soc, bool charging, int64_t now)
{
    if (!present) {
        return ALERT_NONE;
    }
    for (int i = 0; i < 2; i++) {
        if (charging || soc >= BATT_LEVELS[i] + BATT_REARM) a->batt_armed[i] = true;
    }
    if (!a->enabled || charging) {
        return ALERT_NONE;
    }
    // The lowest level crossed wins, and silences the levels above it: a board
    // that boots at 8 % warns once, not twice.
    for (int i = 1; i >= 0; i--) {
        if (a->batt_armed[i] && soc <= BATT_LEVELS[i]) {
            for (int j = 0; j <= i; j++) a->batt_armed[j] = false;
            return shown(a, ALERT_BOARD_BATT, now);
        }
    }
    return ALERT_NONE;
}

// One part's two levels: re-arm below each, then the critical level (first
// crossing, or a reminder every REPEAT_MS), then the warning.
static int temp_level(alerts_t *a, bool armed[2], bool has, float c, bool flag, float warn, float crit, int64_t now)
{
    if (has) {
        if (c <= warn - TEMP_REARM_C) armed[0] = true;
        if (c <= crit - TEMP_REARM_C) armed[1] = true;
    }
    if (!a->enabled) {
        return 0;   // alerts off: levels still re-arm, none is used up
    }
    if ((has && c >= crit) || flag) {
        if (armed[1] || repeat_due(a, ALERT_BOARD_HOT, now)) {
            armed[0] = armed[1] = false;
            return 2;
        }
        return 0;
    }
    if (has && c >= warn && armed[0]) {
        armed[0] = false;
        return 1;
    }
    return 0;
}

alert_t alerts_board_temp(alerts_t *a, bool has_chip, float chip_c, bool has_cell, float cell_c, bool cell_overtemp,
                          int64_t now)
{
    int cell = temp_level(a, a->cell_armed, has_cell, cell_c, cell_overtemp, CELL_WARN_C, CELL_CRIT_C, now);
    int chip = cell ? 0 : temp_level(a, a->chip_armed, has_chip, chip_c, false, CHIP_WARN_C, CHIP_CRIT_C, now);
    if (!cell && !chip) {
        return ALERT_NONE;
    }
    a->hot_source = cell ? HOT_CELL : HOT_CHIP;
    a->hot_critical = (cell ? cell : chip) == 2;
    a->hot_c = cell ? (has_cell ? cell_c : NAN) : chip_c;
    return shown(a, ALERT_BOARD_HOT, now);
}
