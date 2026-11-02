#include <math.h>
#include <stdio.h>
#include <string.h>
#include "demo.h"
#include "bike/bike.h"
#include "obd/gear.h"

#define CYCLE_S 300

// The route: speed (km/h) at each waypoint's time (s), straight lines in
// between. The engine stops at 285 s whatever the route says.
static const struct { float t, kmh; } ROUTE[] = {
    {0, 0}, {6, 0}, {14, 28}, {22, 48}, {40, 52}, {48, 24}, {56, 26}, {72, 82},
    {100, 96}, {122, 102}, {136, 62}, {143, 60}, {150, 30}, {156, 34}, {172, 78},
    {196, 72}, {205, 40}, {212, 0}, {226, 0}, {238, 42}, {252, 56}, {266, 28},
    {275, 0}, {CYCLE_S, 0},
};
#define ROUTE_N ((int)(sizeof(ROUTE) / sizeof(ROUTE[0])))

// Gearing for a profile without gear ratios: the demo still needs to turn
// speed into rpm (the Ride page shows no gear then).
static const float FALLBACK_RPK[] = {140.0f, 97.5f, 77.5f, 65.0f, 56.5f, 50.0f};
#define FALLBACK_GEARS ((int)(sizeof(FALLBACK_RPK) / sizeof(FALLBACK_RPK[0])))

static int64_t s_start, s_last;
static float s_throttle;          // smoothed synthetic throttle
static float s_coolant, s_iat;    // smoothed temperatures
static float s_prev_kmh;
static int s_gear;
static int s_cleared_cycle = -1;  // the cycle whose code the rider cleared

// A CB500X-like support bitmap: no PID 01, MAF, fuel level or fuel rate.
static const uint8_t SUPPORTED[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                                    0x11, 0x12, 0x1C, 0x20, 0x21, 0x40, 0x41};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int gears(void) { return gear_count() > 0 ? gear_count() : FALLBACK_GEARS; }

static float rpm_per_kmh(int g)
{
    return gear_count() > 0 ? gear_rpm_per_kmh(g) : FALLBACK_RPK[g - 1];
}

static float route_kmh(float ct)
{
    for (int i = 1; i < ROUTE_N; i++) {
        if (ct <= ROUTE[i].t) {
            float k = (ct - ROUTE[i - 1].t) / (ROUTE[i].t - ROUTE[i - 1].t);
            return ROUTE[i - 1].kmh + (ROUTE[i].kmh - ROUTE[i - 1].kmh) * k;
        }
    }
    return 0;
}

// Pick the gear a rider would: rev higher while accelerating, short-shift
// when cruising, drop a gear as the revs sag. Returns the engine rpm.
static float ride_gear(float kmh, float accel)
{
    float idle = BIKE_RPM_IDLE;
    if (kmh < 1.0f) {
        s_gear = 0;
        return idle;
    }
    if (s_gear == 0) s_gear = 1;
    float band = BIKE_RPM_RED - idle;
    float up = idle + band * (accel > 0.5f ? 0.55f : 0.35f);
    float down = idle * 1.9f;
    while (s_gear < gears() && kmh * rpm_per_kmh(s_gear) > up) s_gear++;
    while (s_gear > 1 && kmh * rpm_per_kmh(s_gear) < down) s_gear--;
    // Pulling away the clutch slips: the engine runs above the road speed.
    return fmaxf(kmh * rpm_per_kmh(s_gear), kmh < 12.0f ? idle * 1.6f : idle);
}

void demo_start(obd_state_t *s, int64_t now)
{
    obd_state_init(s);
    s_start = s_last = now;
    s_throttle = 8.0f;
    s_coolant = 62.0f;
    s_iat = 26.0f;
    s_prev_kmh = 0;
    s_gear = 0;
    s->phase = PHASE_LIVE;
    for (size_t i = 0; i < sizeof(SUPPORTED); i++) {
        s->supported[SUPPORTED[i]] = true;
    }
    s->support_known = true;
    snprintf(s->adapter, sizeof(s->adapter), "ELM327 v2.3 (demo)");
    snprintf(s->ble_name, sizeof(s->ble_name), "Demo ride");
    snprintf(s->protocol, sizeof(s->protocol), "7");
    snprintf(s->ecu, sizeof(s->ecu), "18DAF110");
    s->multi_pid = true;
    s->dtcs_read = true;
}

int demo_gear(void) { return s_gear; }

void demo_feed(obd_state_t *s, int64_t now)
{
    float t = (now - s_start) / 1000.0f;
    float dt = clampf((now - s_last) / 1000.0f, 0.001f, 1.0f);
    s_last = now;
    int cycle = (int)(t / CYCLE_S);
    float ct = fmodf(t, CYCLE_S);
    bool stopped = ct >= 285.0f;

    float kmh = route_kmh(ct);
    if (kmh > 10.0f) kmh += 1.5f * sinf(t * 0.9f) + 0.8f * sinf(t * 2.1f);   // a rider's hand
    float accel = (kmh - s_prev_kmh) / dt;   // km/h per second
    s_prev_kmh = kmh;
    float rpm = stopped ? 0 : ride_gear(kmh, accel);
    if (stopped) {
        kmh = 0;
        s_gear = 0;
    }

    // Warm up from cold, run a little hotter standing still; overheat at 236-262 s.
    float target = stopped ? s_coolant : 86.0f + (kmh < 5.0f ? 6.0f : 0.0f) - kmh / 40.0f;
    s_coolant += (target - s_coolant) * fminf(1.0f, dt / 20.0f);
    float coolant = s_coolant;
    if (ct >= 236.0f && ct < 262.0f) {
        float x = ct < 250.0f ? (ct - 236.0f) / 14.0f : 1.0f - (ct - 250.0f) / 12.0f;
        coolant = fmaxf(coolant, 98.0f + 10.0f * clampf(x, 0, 1));
    }
    s_iat += ((kmh < 5.0f ? 36.0f : 27.0f) - s_iat) * fminf(1.0f, dt / 30.0f);

    // Throttle: what holds the speed against the air, plus what the
    // acceleration takes; shut while slowing down.
    float want = rpm < 100 ? 0 : clampf(4.0f + kmh * kmh / 550.0f + fmaxf(0, accel) * 7.0f, 0, 100);
    if (accel < -1.5f) want = 0;
    s_throttle += (want - s_throttle) * fminf(1.0f, dt / 0.25f);
    float thr = clampf(s_throttle, 0, 100);
    float load = rpm < 100 ? 0 : clampf(18.0f + thr * 0.75f, 0, 100);
    float map = rpm < 100 ? 101.0f : clampf(30.0f + thr * 0.68f, 20, 101);
    float adv = rpm < 100 ? 0 : clampf(8.0f + (rpm - BIKE_RPM_IDLE) / 220.0f - thr * 0.05f, -5, 38);
    float stft = 2.5f * sinf(t * 0.7f) + 1.2f * sinf(t * 2.3f);
    float volts = rpm < 100 ? 12.6f : 14.15f + 0.08f * sinf(t * 0.9f);

    obd_state_set(s, PID_RPM, rpm, now);
    obd_state_set(s, PID_SPEED, roundf(kmh), now);   // the PID is whole km/h
    obd_state_set(s, PID_THROTTLE, thr, now);
    obd_state_set(s, PID_LOAD, load, now);
    obd_state_set(s, PID_MAP, roundf(map), now);
    obd_state_set(s, PID_COOLANT, roundf(coolant), now);
    obd_state_set(s, PID_IAT, roundf(s_iat), now);
    obd_state_set(s, PID_ADVANCE, roundf(adv * 2) / 2, now);
    obd_state_set(s, PID_STFT1, stft, now);
    obd_state_set(s, PID_VOLTAGE, volts, now);
    obd_state_set(s, PID_FUEL_STATUS, coolant > 60 ? 2 : 1, now);
    s->raw[PID_FUEL_STATUS][0] = coolant > 60 ? 0x02 : 0x01;
    obd_state_set(s, PID_OBD_STD, 6, now);
    s->raw[PID_OBD_STD][0] = 6;
    obd_state_set(s, PID_SEC_AIR, 0, now);
    s->raw[PID_MONITOR][0] = 0x00;
    s->raw[PID_MONITOR][1] = 0x07;
    s->raw[PID_MONITOR][2] = 0x65;
    s->raw[PID_MONITOR][3] = 0x00;
    obd_state_set(s, PID_MONITOR, 0, now);

    bool fault = ct >= 260.0f && cycle != s_cleared_cycle;
    s->mil = fault;
    s->n_dtcs = fault ? 1 : 0;
    s->n_pending = fault ? 1 : 0;
    if (fault) {
        strcpy(s->dtcs[0], "P0133");
        strcpy(s->pending[0], "P0171");
    }
    obd_state_set(s, PID_MIL_DIST, fault ? 1 : 0, now);
    s->replies_per_sec = 14.0f + 1.5f * sinf(t * 0.3f);
    s->last_reply_at = now;
    if (s->clear == CLEAR_SENT) {
        s->clear = CLEAR_OK;
        s_cleared_cycle = cycle;
    }
}

void demo_request_clear(obd_state_t *s, int64_t now)
{
    float rpm;
    if (obd_value(s, PID_RPM, now, &rpm) && rpm >= 300.0f) {
        s->clear = CLEAR_FAILED;   // the UI already refuses; this is the backstop
        return;
    }
    s->clear = CLEAR_SENT;   // answered on the next feed
}
