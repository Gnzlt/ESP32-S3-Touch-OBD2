/*
 * The ride's summary: time, distance integrated from the speed PID, peaks and
 * lows. Pure logic; app code saves the struct to NVS as a blob every minute
 * and when the ECU goes quiet, so a reboot keeps it. Reset by hand.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRIP_VERSION 2   // 2: estimated fuel

typedef struct {
    uint32_t version;
    uint32_t engine_s;        // seconds with the engine running
    uint32_t moving_s;        // seconds above 3 km/h
    double distance_m;
    float max_kmh;
    float max_rpm;
    float max_coolant;
    float min_volts;          // after 10 s running (not while cranking); 0 = none yet
    float max_lean_left;      // degrees, positive, held for a second (sensors/held_peak.h)
    float max_lean_right;
    float max_g;              // largest horizontal G held for a second
    uint32_t alerts;
    double fuel_raw_ml;       // estimated fuel, uncalibrated (obd/fuel.h)
    uint32_t fuel_covered_ms; // engine running with an estimate
    uint32_t fuel_missing_ms; // engine running without one
} trip_t;

void trip_reset(trip_t *t);

// Integrate dt_ms of riding. has_ flags are false for stale values.
void trip_update(trip_t *t, int dt_ms, bool has_kmh, float kmh, bool has_rpm, float rpm,
                 bool has_coolant, float coolant, bool has_volts, float volts);

// Lean in degrees (negative = left) and horizontal G every tick, dt_ms since
// the last; NAN when not riding (which restarts the one-second hold).
void trip_update_motion(trip_t *t, int dt_ms, float lean_deg, float g);

float trip_avg_kmh(const trip_t *t);

// dt_ms of estimated fuel: lph < 0 = no estimate. Only running time counts
// towards the coverage.
void trip_add_fuel(trip_t *t, int dt_ms, bool running, float lph);
// Raw L/100 km, or < 0 under 1 km.
float trip_raw_l100(const trip_t *t);
