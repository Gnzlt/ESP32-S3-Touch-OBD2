/*
 * When to interrupt the rider. Pure logic, no UI: alerts_evaluate() is called
 * once a second and returns at most one alert, the most urgent. Each alert has
 * hysteresis or a sustain time so a value hovering at a limit doesn't fire
 * every second, and repeats only after 5 minutes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ALERT_NONE,
    ALERT_COOLANT,
    ALERT_VOLT_HIGH,
    ALERT_VOLT_LOW,
    ALERT_DTC,
    ALERT_BOARD_BATT,          // the board's own LiPo is low
    ALERT_BOARD_HOT,           // the ESP32-S3 or the LiPo is too hot (alerts_t.hot_*)
    // The tyre sensors (tpms/tpms_alerts.h, in its order); the tyre is in dash_t.alert_tyre.
    ALERT_TYRE_LOSS,
    ALERT_TYRE_LOW,
    ALERT_TYRE_ALARM,
    ALERT_TYRE_HOT,
    ALERT_TYRE_HIGH,
    ALERT_TYRE_COLD,
    ALERT_TYRE_SILENT,
    ALERT_TYRE_BATT,
    ALERT_COUNT
} alert_t;

typedef enum { HOT_NONE, HOT_CHIP, HOT_CELL } hot_source_t;

// Board temperatures, °C. The chip is the ESP32-S3's die, which runs about
// 15 °C above the air around it (the S3R8 is rated for 65 °C ambient); the
// cell is the LiPo, which shouldn't be used above 60 °C.
#define CHIP_WARN_C   80
#define CHIP_CRIT_C   90
#define CELL_WARN_C   50
#define CELL_CRIT_C   58
#define TEMP_REARM_C  5        // degrees below a level to re-arm it

typedef struct {
    bool enabled;
    float coolant_limit, volt_low, volt_high;
    bool coolant_armed;
    int64_t low_since, high_since;
    int last_dtc_total;
    bool last_mil;
    bool dtc_pending;
    bool batt_armed[2];        // the 20 % and 10 % board battery levels
    bool chip_armed[2], cell_armed[2];   // the warning and critical temperatures
    // The last ALERT_BOARD_HOT, for the overlay: which part, how bad, how hot.
    hot_source_t hot_source;
    bool hot_critical;
    float hot_c;               // NAN when only the gauge's over-temperature flag said so
    int64_t shown_at[ALERT_COUNT];
    uint32_t fired;            // how many alerts this ride, for the trip page
} alerts_t;

void alerts_init(alerts_t *a);

// Values are passed with a has_ flag: false when stale or unsupported.
// dtc_total counts stored and pending codes.
alert_t alerts_evaluate(alerts_t *a, bool has_coolant, float coolant, bool has_volts, float volts,
                        bool has_rpm, float rpm, int dtc_total, bool mil, int64_t now);

// The board's LiPo: fires once when the charge drops to 20 % and once at 10 %.
// A level re-arms when the board charges or the charge climbs 5 points above
// it. Called with each battery reading; present is false with no cell.
alert_t alerts_battery(alerts_t *a, bool present, int soc, bool charging, int64_t now);

// The board overheating, called with each reading (has_ false when unknown;
// cell_overtemp is the gauge's own flag). Each level fires once as it is
// crossed and re-arms TEMP_REARM_C below it; while at or above the critical
// level it repeats every 5 minutes. The cell is checked first.
alert_t alerts_board_temp(alerts_t *a, bool has_chip, float chip_c, bool has_cell, float cell_c, bool cell_overtemp,
                          int64_t now);
