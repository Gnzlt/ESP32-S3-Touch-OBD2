#include <math.h>
#include <string.h>
#include "trip.h"
#include "sensors/held_peak.h"

#define MOVING_KMH   3.0f
#define RUNNING_RPM  800.0f
// The speed PID reads in whole km/h and a sample can be lost; integrating a
// value older than this would invent distance.
#define MAX_DT_MS    2000
// The battery reads low while the starter turns (10.6 V at 1,678 rpm on a test ride),
// so the minimum counts once the engine has run this long.
#define VOLTS_AFTER_MS 10000

// Per trip_t working state (the sensor task updates several each tick): the
// sub-second remainders of engine and moving time, how long the engine has
// run, and the held lean and G. Kept outside trip_t so its layout, saved in
// NVS, doesn't change.
#define MAX_TRIPS 4
typedef struct {
    const trip_t *trip;
    uint32_t engine_ms, moving_ms, running_ms;
    held_peak_t lean, g;
} work_t;
static work_t s_work[MAX_TRIPS];

static void work_clear(work_t *w, const trip_t *t)
{
    memset(w, 0, sizeof(*w));
    w->trip = t;
}

static work_t *work(const trip_t *t)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_TRIPS; i++) {
        if (s_work[i].trip == t) return &s_work[i];
        if (!s_work[i].trip && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;   // more trips than slots: share the first
    work_clear(&s_work[free_slot], t);
    return &s_work[free_slot];
}

void trip_reset(trip_t *t)
{
    memset(t, 0, sizeof(*t));
    t->version = TRIP_VERSION;
    work_clear(work(t), t);
}

void trip_update(trip_t *t, int dt_ms, bool has_kmh, float kmh, bool has_rpm, float rpm,
                 bool has_coolant, float coolant, bool has_volts, float volts)
{
    if (dt_ms <= 0 || dt_ms > MAX_DT_MS) {
        return;
    }
    bool running = has_rpm && rpm >= RUNNING_RPM;
    work_t *w = work(t);
    uint32_t *engine_ms = &w->engine_ms, *moving_ms = &w->moving_ms;
    if (!running) w->running_ms = 0;
    else if (w->running_ms < VOLTS_AFTER_MS) w->running_ms += dt_ms;
    if (running) {
        *engine_ms += dt_ms;
        t->engine_s += *engine_ms / 1000;
        *engine_ms %= 1000;
    }
    if (has_kmh) {
        t->distance_m += kmh * dt_ms / 3600.0;
        if (kmh > MOVING_KMH) {
            *moving_ms += dt_ms;
            t->moving_s += *moving_ms / 1000;
            *moving_ms %= 1000;
        }
        if (kmh > t->max_kmh) t->max_kmh = kmh;
    }
    if (has_rpm && rpm > t->max_rpm) t->max_rpm = rpm;
    if (has_coolant && coolant > t->max_coolant) t->max_coolant = coolant;
    if (has_volts && running && w->running_ms >= VOLTS_AFTER_MS && (t->min_volts == 0.0f || volts < t->min_volts)) {
        t->min_volts = volts;
    }
}

void trip_update_motion(trip_t *t, int dt_ms, float lean_deg, float g)
{
    work_t *w = work(t);
    held_add(&w->lean, dt_ms, lean_deg);
    held_add(&w->g, dt_ms, g);
    // fmaxf: a trip kept in NVS keeps its peaks across a reboot, which clears these.
    t->max_lean_left = fmaxf(t->max_lean_left, w->lean.peak_neg);
    t->max_lean_right = fmaxf(t->max_lean_right, w->lean.peak_pos);
    t->max_g = fmaxf(t->max_g, w->g.peak_pos);
}

float trip_avg_kmh(const trip_t *t)
{
    return t->moving_s > 0 ? (float)(t->distance_m / 1000.0 / (t->moving_s / 3600.0)) : 0.0f;
}

void trip_add_fuel(trip_t *t, int dt_ms, bool running, float lph)
{
    if (dt_ms <= 0 || dt_ms > MAX_DT_MS) {
        return;
    }
    if (lph > 0) {
        t->fuel_raw_ml += lph * dt_ms / 3600.0;   // L/h * ms -> mL
    }
    if (running) {
        if (lph >= 0) {
            t->fuel_covered_ms += dt_ms;
        } else {
            t->fuel_missing_ms += dt_ms;
        }
    }
}

float trip_raw_l100(const trip_t *t)
{
    return t->distance_m >= 1000.0 ? (float)(t->fuel_raw_ml / 1000.0 / (t->distance_m / 100000.0)) : -1.0f;
}
