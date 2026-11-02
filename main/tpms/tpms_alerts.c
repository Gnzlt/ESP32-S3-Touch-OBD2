#include <math.h>
#include <string.h>
#include "bike/bike.h"
#include "tpms_alerts.h"

#define HYST_KPA      5.0f       // 0.05 bar, 0.7 psi
#define LOSS_MIN_KPA  10.0f
#define HYST_C        3.0f

void tpms_cfg_defaults(tpms_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = TPMS_CFG_VERSION;
    c->enabled = true;
    c->low_on[TPMS_FRONT] = c->low_on[TPMS_REAR] = true;
    c->low_kpa[TPMS_FRONT] = BIKE_TYRE_LOW_FRONT_KPA;
    c->low_kpa[TPMS_REAR] = BIKE_TYRE_LOW_REAR_KPA;
    c->high_kpa[TPMS_FRONT] = BIKE_TYRE_FRONT_KPA + 50.0f;   // a starting point once switched on
    c->high_kpa[TPMS_REAR] = BIKE_TYRE_REAR_KPA + 50.0f;
    c->hot_on = true;
    c->hot_c = 80.0f;
    c->cold_c = 0.0f;
    c->loss_pct = 15;
    c->sensor_alarm = true;
    c->battery = true;
    // The DJTPMS sensors send every 180 s and a test ride heard the rear in
    // 7 of 55 slots: gaps of 18 minutes while riding were normal for it.
    c->silent_min = 30;
}

void tpms_alerts_init(tpms_alerts_t *a)
{
    memset(a, 0, sizeof(*a));
    for (int t = 0; t < TPMS_TYRES; t++) {
        for (int i = 0; i < TPMS_A_COUNT; i++) a->armed[t][i] = true;
        a->silent_heard_at[t] = -1;
        a->low_since[t][0] = a->low_since[t][1] = -1;
    }
    a->slot = -1;
}

// 1: the limit is crossed; -1: clear of it by the hysteresis (re-arm);
// 0: neither, or no value to judge.
static int state(tpms_alerts_t *a, const tpms_cfg_t *c, tpms_alert_t al, int t, const tpms_tyre_t *tyre,
                 int64_t now, bool riding)
{
    float kpa = tyre->kpa, temp = tyre->temp_c;
    bool has_kpa = !isnan(kpa), has_temp = !isnan(temp);
    switch (al) {
    case TPMS_A_LOSS: {
        if (c->loss_pct <= 0 || !has_kpa) return -1;
        // The share of the window's highest pressure that is gone. At least
        // LOSS_MIN_KPA, so a sensor off its tyre, near zero, cannot trip it.
        float drop = tpms_tyre_drop(tyre, now, TPMS_LOSS_WINDOW_MS);
        float share = drop > 0.0f ? drop * 100.0f / (kpa + drop) : 0.0f;
        return share >= c->loss_pct && drop >= LOSS_MIN_KPA ? 1 : share < c->loss_pct / 2.0f ? -1 : 0;
    }
    case TPMS_A_LOW:
        if (!c->low_on[t] || c->low_kpa[t] <= 0.0f || !has_kpa) return -1;
        return kpa < c->low_kpa[t] ? 1 : kpa >= c->low_kpa[t] + HYST_KPA ? -1 : 0;
    case TPMS_A_HIGH:
        if (!c->high_on[t] || c->high_kpa[t] <= 0.0f || !has_kpa) return -1;
        return kpa > c->high_kpa[t] ? 1 : kpa <= c->high_kpa[t] - HYST_KPA ? -1 : 0;
    case TPMS_A_ALARM:
        return c->sensor_alarm && tyre->alarm ? 1 : -1;
    case TPMS_A_HOT:
        if (!c->hot_on || !has_temp) return -1;
        return temp > c->hot_c ? 1 : temp <= c->hot_c - HYST_C ? -1 : 0;
    case TPMS_A_COLD:
        if (!c->cold_on || !has_temp) return -1;
        return temp < c->cold_c ? 1 : temp >= c->cold_c + HYST_C ? -1 : 0;
    case TPMS_A_SILENT:
        if (c->silent_min <= 0 || tyre->heard_at < 0) return -1;
        if (tyre->heard_at != a->silent_heard_at[t]) {
            a->armed[t][al] = true;   // heard since it last fired: judge this silence afresh
        }
        return riding && now - tyre->heard_at > (int64_t)c->silent_min * 60000 ? 1 : 0;
    case TPMS_A_BATTERY:
        // Once a ride: a battery does not recover.
        return c->battery && tpms_battery_low(tyre->battery_pct, tyre->volts) ? 1 : 0;
    default:
        return 0;
    }
}

// A crossing counts once it has held TPMS_CONFIRM_MS and a later packet still
// shows it; until then it neither fires nor re-arms.
static int confirmed(tpms_alerts_t *a, int k, int t, const tpms_tyre_t *tyre, int s, int64_t now)
{
    if (s < 0) {
        a->low_since[t][k] = -1;
        return s;
    }
    if (s == 0) return 0;
    if (a->low_since[t][k] < 0) {
        a->low_since[t][k] = now;
        a->low_value_at[t][k] = tyre->value_at;
    }
    return now - a->low_since[t][k] >= TPMS_CONFIRM_MS && tyre->value_at > a->low_value_at[t][k] ? 1 : 0;
}

tpms_alert_t tpms_alerts_evaluate(tpms_alerts_t *a, const tpms_cfg_t *c, const tpms_tyre_t tyres[TPMS_TYRES],
                                  const bool paired[TPMS_TYRES], int64_t now, bool riding)
{
    a->slot = -1;
    if (!c->enabled) {
        return TPMS_A_NONE;
    }
    tpms_alert_t best = TPMS_A_NONE;
    int best_slot = -1;
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (!paired[t]) continue;
        for (int al = TPMS_A_LOSS; al < TPMS_A_COUNT; al++) {
            int s = state(a, c, (tpms_alert_t)al, t, &tyres[t], now, riding);
            if (al == TPMS_A_LOSS || al == TPMS_A_LOW) s = confirmed(a, al - TPMS_A_LOSS, t, &tyres[t], s, now);
            if (s < 0) {
                a->armed[t][al] = true;
            } else if (s > 0 && a->armed[t][al] && (best == TPMS_A_NONE || al < (int)best)) {
                best = (tpms_alert_t)al;
                best_slot = t;
            }
        }
    }
    if (best != TPMS_A_NONE) {
        a->armed[best_slot][best] = false;
        if (best == TPMS_A_SILENT) a->silent_heard_at[best_slot] = tyres[best_slot].heard_at;
    }
    a->slot = best_slot;
    return best;
}
