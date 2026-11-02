#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tpms_tyres.h"

void tpms_tyre_init(tpms_tyre_t *t)
{
    memset(t, 0, sizeof(*t));
    t->kpa = t->temp_c = t->volts = NAN;
    t->battery_pct = -1;
    t->heard_at = t->value_at = -1;
}

// One sample per step: a newer pressure in the same step replaces the step's
// sample if lower, so a burst of packets does not flush the history.
static void remember(tpms_tyre_t *t, float k, int64_t now)
{
    if (t->hist_n > 0) {
        uint32_t last = (t->hist_n - 1) % TPMS_HISTORY_SIZE;
        if (now - t->hist_t[last] < TPMS_HISTORY_STEP_MS) {
            if (k < t->hist_k[last]) t->hist_k[last] = k;
            return;
        }
    }
    uint32_t idx = t->hist_n % TPMS_HISTORY_SIZE;
    t->hist_t[idx] = now;
    t->hist_k[idx] = k;
    t->hist_n++;
}

void tpms_tyre_update(tpms_tyre_t *t, const tpms_reading_t *r, int64_t now)
{
    t->format = r->format;
    t->heard_at = now;
    if (!r->awake) {
        if (!isnan(r->volts)) t->volts = r->volts;
        return;
    }
    if (!isnan(r->kpa)) {
        t->kpa = r->kpa;
        t->value_at = now;
        remember(t, r->kpa, now);
    }
    if (!isnan(r->temp_c)) t->temp_c = r->temp_c;
    if (!isnan(r->volts)) t->volts = r->volts;
    if (r->battery_pct >= 0) t->battery_pct = r->battery_pct;
    t->alarm = r->alarm;
}

int64_t tpms_tyre_age(const tpms_tyre_t *t, int64_t now)
{
    return t->value_at < 0 ? -1 : now - t->value_at;
}

float tpms_tyre_drop(const tpms_tyre_t *t, int64_t now, int64_t window_ms)
{
    if (isnan(t->kpa) || t->hist_n == 0) {
        return 0.0f;
    }
    float high = t->kpa;
    uint32_t n = t->hist_n < TPMS_HISTORY_SIZE ? t->hist_n : TPMS_HISTORY_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (t->hist_n - 1 - i) % TPMS_HISTORY_SIZE;
        if (now - t->hist_t[idx] > window_ms) {
            break;   // older samples only from here on
        }
        if (t->hist_k[idx] > high) high = t->hist_k[idx];
    }
    return high - t->kpa;
}

#define BATT_LOW_PCT 10
#define BATT_EMPTY_V 2.5f
#define BATT_FULL_V  3.0f

int tpms_battery_level(int pct, float volts)
{
    if (pct >= 0 && pct <= 100) {
        return pct;
    }
    if (isnan(volts)) {
        return -1;
    }
    int l = (int)((volts - BATT_EMPTY_V) * 100.0f / (BATT_FULL_V - BATT_EMPTY_V));
    return l < 0 ? 0 : l > 100 ? 100 : l;
}

bool tpms_battery_low(int pct, float volts)
{
    return (pct >= 0 && pct <= BATT_LOW_PCT) || (!isnan(volts) && volts < BATT_EMPTY_V);
}

#define KPA_PER_PSI 6.894757f

float tpms_from_kpa(float kpa, tpms_unit_t u)
{
    return u == TPMS_UNIT_PSI ? kpa / KPA_PER_PSI : u == TPMS_UNIT_KPA ? kpa : kpa / 100.0f;
}

float tpms_to_kpa(float v, tpms_unit_t u)
{
    return u == TPMS_UNIT_PSI ? v * KPA_PER_PSI : u == TPMS_UNIT_KPA ? v : v * 100.0f;
}

void tpms_fmt_pressure(char *out, size_t n, float kpa, tpms_unit_t u)
{
    float v = tpms_from_kpa(kpa, u);
    snprintf(out, n, u == TPMS_UNIT_BAR ? "%.2f" : u == TPMS_UNIT_PSI ? "%.1f" : "%.0f", v);
}

const char *tpms_unit_name(tpms_unit_t u)
{
    return u == TPMS_UNIT_PSI ? "psi" : u == TPMS_UNIT_KPA ? "kPa" : "bar";
}
