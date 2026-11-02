/*
 * When to warn about a tyre. Pure logic: tpms_alerts_evaluate() is called once
 * a second and returns at most one alert, the most urgent, with its tyre in
 * `slot`. Each alert fires once when its limit is crossed and re-arms only
 * when the value comes back past a hysteresis, so a value hovering at a limit
 * does not beep again. Only paired tyres are watched.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "tpms_tyres.h"

// In order of urgency.
typedef enum {
    TPMS_A_NONE,
    TPMS_A_LOSS,                 // fast pressure loss
    TPMS_A_LOW,                  // below the tyre's own "warn me below" pressure
    TPMS_A_ALARM,                // the sensor's own leak alarm
    TPMS_A_HOT,
    TPMS_A_HIGH,                 // above the tyre's high limit
    TPMS_A_COLD,
    TPMS_A_SILENT,               // a paired sensor unheard while riding
    TPMS_A_BATTERY,              // the sensor's battery
    TPMS_A_COUNT
} tpms_alert_t;

#define TPMS_LOSS_WINDOW_MS 300000

// The rider's limits, in kPa and °C whatever unit the screen shows. Saved in
// NVS as a blob, so fields are only ever appended, with `version` bumped.
typedef struct {
    uint8_t version;
    bool enabled;
    bool low_on[TPMS_TYRES];
    float low_kpa[TPMS_TYRES];   // kept when switched off
    bool high_on[TPMS_TYRES];
    float high_kpa[TPMS_TYRES];
    bool hot_on;
    float hot_c;
    bool cold_on;
    float cold_c;
    int loss_pct;                // % of the pressure lost within the window; 0 = off
    bool sensor_alarm;
    bool battery;
    int silent_min;              // a paired sensor unheard this long while riding; 0 = off
} tpms_cfg_t;

#define TPMS_CFG_VERSION 1

void tpms_cfg_defaults(tpms_cfg_t *c);

// A low pressure or a loss must hold this long, through a later packet, before
// it fires: a DJTPMS sensor's first packets after waking read ~1 kPa for a few
// seconds (4 s on the rear and 9 s on the front of one ride: two false alerts).
#define TPMS_CONFIRM_MS 20000

typedef struct {
    bool armed[TPMS_TYRES][TPMS_A_COUNT];
    int64_t silent_heard_at[TPMS_TYRES];
    int64_t low_since[TPMS_TYRES][2];      // LOSS, LOW: when first crossed, -1 = not
    int64_t low_value_at[TPMS_TYRES][2];   // the packet it was first crossed on
    int slot;                    // the tyre of the alert evaluate() last returned, -1 for none
} tpms_alerts_t;

void tpms_alerts_init(tpms_alerts_t *a);
tpms_alert_t tpms_alerts_evaluate(tpms_alerts_t *a, const tpms_cfg_t *c, const tpms_tyre_t tyres[TPMS_TYRES],
                                  const bool paired[TPMS_TYRES], int64_t now, bool riding);
