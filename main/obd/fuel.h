/*
 * Estimated fuel flow from the intake air, since most bikes have no MAF and
 * no fuel-level PID. The air comes from the calculated load (PID 04): by its
 * SAE definition the airflow as a share of the peak wide-open airflow at
 * that rpm, corrected to standard conditions:
 *
 *   air kg/s = load * rho_std * displacement * VE_peak * rpm / 120
 *   fuel L/h = air / AFR * trim * 3600 / density
 *
 * with the air-fuel ratio from the fuel system status (PID 03) and the
 * short-term trim. Not MAP: on a twin each MAP reading is an instant of
 * the intake's pulsing (33-97 kPa at a steady idle on a CB500X), while the
 * ECU's load is steady (27 % +- 1). The result is "raw": uncalibrated (k = 1). Logs keep raw
 * values so the model can be refitted later; the screen shows raw * k, with k
 * from fill-ups (fillups.csv). Pure C, host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bump when the formula changes: logs carry it, so a later calibration knows
// which model produced their numbers.
#define FUEL_MODEL_VERSION 1

typedef struct {
    bool has_rpm, has_load, has_stft;
    float rpm, load_pct, stft_pct;
    int status;                 // PID 03 byte A (1, 2, 4, 8, 16), 0 = unknown
} fuel_in_t;

// Raw fuel flow in L/h: 0 with the engine off or in a decel fuel cut, < 0
// when rpm or load is missing (unknown, not zero).
float fuel_rate_lph(const fuel_in_t *in);

// Fuel since the last fill-up, kept across rides and reboots (NVS "fill").
#define FILL_VERSION 1
typedef struct {
    uint32_t version;
    uint32_t fills;             // fill-ups marked so far (0 = no starting point yet)
    double km;                  // since the last fill-up
    double raw_ml;              // estimated fuel since then, uncalibrated
    uint32_t covered_ms;        // engine running with an estimate
    uint32_t missing_ms;        // engine running without one (stale inputs)
    float k;                    // calibration factor: litres pumped / raw litres (1 until calibrated)
} fill_t;

void fill_reset(fill_t *f);
// dt_ms of riding: distance from the speed, fuel as trip_add_fuel() does.
void fill_add(fill_t *f, int dt_ms, bool running, float lph, bool has_kmh, float kmh);
// The counters since the last fill-up start again; fills, k and version stay.
void fill_mark(fill_t *f);
// Share of running time the estimate covered, 0..100 (100 with no running time).
int fuel_coverage_pct(uint32_t covered_ms, uint32_t missing_ms);
