#include <math.h>
#include "gear.h"
#include "bike/bike.h"

#define HOLD_SAMPLES 3   // ~0.3 s at the RPM poll rate

#ifdef BIKE_GEAR_RATIOS
static const float RATIOS[] = BIKE_GEAR_RATIOS;
#define GEARS ((int)(sizeof(RATIOS) / sizeof(RATIOS[0])))
#else
static const float RATIOS[1] = {0};
#define GEARS 0
#define BIKE_GEAR_K 1.0f
#endif

int gear_count(void) { return GEARS; }

float gear_rpm_per_kmh(int gear)
{
    return gear >= 1 && gear <= GEARS ? RATIOS[gear - 1] * BIKE_GEAR_K : 0.0f;
}

int gear_match(float rpm, float kmh)
{
    if (kmh < BIKE_GEAR_MIN_KMH || rpm < 900.0f) {
        return 0;
    }
    float r = rpm / kmh / BIKE_GEAR_K;
    for (int i = 0; i < GEARS; i++) {
        if (fabsf(r - RATIOS[i]) <= RATIOS[i] * BIKE_GEAR_TOLERANCE) {
            return i + 1;
        }
    }
    return 0;
}

int gear_update(gear_t *g, bool valid, float rpm, float kmh)
{
    int m = valid ? gear_match(rpm, kmh) : 0;
    if (m == g->candidate) {
        if (g->count < HOLD_SAMPLES) {
            g->count++;
        }
    } else {
        g->candidate = m;
        g->count = 1;
    }
    if (g->count >= HOLD_SAMPLES || !valid) {
        g->shown = valid ? g->candidate : 0;
    }
    return g->shown;
}
