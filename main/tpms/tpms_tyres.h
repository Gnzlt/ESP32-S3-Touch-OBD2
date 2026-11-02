/*
 * One tyre's state: the latest value of every field and when it was heard,
 * plus a short pressure history for the fast-loss alert. A packet that lacks
 * a field keeps that field's last value; a sleep frame only proves the sensor
 * alive. Also the sensor battery as a level, and the units pressures are shown
 * in. Pure C.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tpms_formats.h"

#define TPMS_TYRES 2             // front, rear
#define TPMS_FRONT 0
#define TPMS_REAR  1

// One sample per step, 6 minutes in all: a fast-loss window of up to 5.
#define TPMS_HISTORY_STEP_MS 30000
#define TPMS_HISTORY_SIZE    12

typedef struct {
    tpms_format_t format;
    float kpa, temp_c, volts;    // NAN until heard
    int battery_pct;             // -1 = never sent
    bool alarm;
    int64_t heard_at;            // ms of the last packet, sleep frames too; -1 = never
    int64_t value_at;            // ms of the last packet with a pressure; -1 = never
    int64_t hist_t[TPMS_HISTORY_SIZE];
    float hist_k[TPMS_HISTORY_SIZE];
    uint32_t hist_n;             // samples written, ever; the ring's head is hist_n % SIZE
} tpms_tyre_t;

// Also "forget": back to never heard.
void tpms_tyre_init(tpms_tyre_t *t);
void tpms_tyre_update(tpms_tyre_t *t, const tpms_reading_t *r, int64_t now);
// Milliseconds since the last pressure, or -1 if there has been none.
int64_t tpms_tyre_age(const tpms_tyre_t *t, int64_t now);
// How far the pressure has fallen within the last window_ms: the highest
// sample in the window minus the latest pressure. 0 without history.
float tpms_tyre_drop(const tpms_tyre_t *t, int64_t now, int64_t window_ms);

// The sensor battery, 0..100, or -1 when the sensor sent neither value. A
// percentage wins over a voltage; one above 100 is not a percentage. A coin
// cell holds about 3 V most of its life: 2.5 V is empty enough to replace.
int tpms_battery_level(int pct, float volts);
bool tpms_battery_low(int pct, float volts);

// Pressure units: inside, pressure is always gauge kPa.
typedef enum { TPMS_UNIT_BAR, TPMS_UNIT_PSI, TPMS_UNIT_KPA, TPMS_UNIT_COUNT } tpms_unit_t;

float tpms_from_kpa(float kpa, tpms_unit_t u);
float tpms_to_kpa(float v, tpms_unit_t u);
// "2.25" bar, "32.6" psi, "225" kPa.
void tpms_fmt_pressure(char *out, size_t n, float kpa, tpms_unit_t u);
const char *tpms_unit_name(tpms_unit_t u);
